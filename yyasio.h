#pragma once

#include <cmath>
#include <cstring>
#include <iostream>
#include <coroutine>
#include <functional>
#include <queue>
#include <liburing.h>

namespace yyasio
{

enum ErrorCode
{
    YYASIO_OK = 0,
    YYASIO_INIT_ERROR,
    YYASIO_SUBMIT_ERROR
};

struct Task
{
    struct promise_type
    {
        int result = 0;
        Task get_return_object() { return {}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() {std::terminate(); }
    };
};

class Scheduler
{
public:
    using PrepSqeCb = std::function<void(io_uring_sqe*)>;

public:
    ErrorCode init(size_t entries) {
        if (io_uring_queue_init(entries, &ring, 0) < 0)
        {
            return YYASIO_INIT_ERROR;
        }
        return YYASIO_OK;
    }

    void schedule(std::coroutine_handle<> coro, PrepSqeCb prep_seq_fn)
    {
        struct timespec enter_ts {};
        clock_gettime(CLOCK_MONOTONIC, &enter_ts);
        pending_queue.push(std::make_tuple(coro, prep_seq_fn, enter_ts));
    }

    ErrorCode run()
    {
        while (true)
        {
            batch_prepare_sqe();
            int submitted = io_uring_submit_and_wait(&ring, 1);
            if (submitted < 0)
            {
                std::cerr << "io_uring_submit_and_wait failed:, ret = " << submitted << "\n";
                return YYASIO_SUBMIT_ERROR;
            }

            tot_submit_items += submitted;
            tot_submit_count += 1;

            io_uring_cqe* cqe = nullptr;
            while (io_uring_peek_cqe(&ring, &cqe) == 0)
            {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                auto handle = std::coroutine_handle<>::from_address(reinterpret_cast<void*>(cqe->user_data)); 
                io_uring_cqe_seen(&ring, cqe);
                auto task_handle = std::coroutine_handle<Task::promise_type>::from_address(handle.address());
                task_handle.promise().result = cqe->res;
                handle.resume();
                if (handle.done())
                {
                    handle.destroy();
                }
            }
        }
    }

    void print_sched_stats()
    {
        std::cout << "Total schedule time: " << tot_sched_time << " ns\n";
        std::cout << "Total schedule count: " << tot_sched_count << "\n";
        std::cout << "Average schedule time: " << tot_sched_time / tot_sched_count << " ns\n";
        std::cout << "Total submit items: " << tot_submit_items << "\n";
        std::cout << "Total submit count: " << tot_submit_count << "\n";
        std::cout << "Average submit items per submit: " << tot_submit_items / tot_submit_count << "\n";
        std::cout << "Average pending queue size: " << tot_pending_queue_size / tot_pending_queue_sample_count << "\n";
    }

private:
    void batch_prepare_sqe()
    {
        tot_pending_queue_size += pending_queue.size();
        tot_pending_queue_sample_count += 1;

        while (!pending_queue.empty())
        {
            auto* sqe = io_uring_get_sqe(&ring);
            if (!sqe)
            {
                break;
            }
            auto [coro, prep_sqe_fn, enter_ts] = pending_queue.front();
            
            struct timespec sched_ts {};
            clock_gettime(CLOCK_MONOTONIC, &sched_ts);
            uint64_t cost_ns = (static_cast<uint64_t>(sched_ts.tv_sec) * 1000000000ULL + sched_ts.tv_nsec) -
                               (static_cast<uint64_t>(enter_ts.tv_sec) * 1000000000ULL + enter_ts.tv_nsec);
            tot_sched_time += cost_ns;
            tot_sched_count++;

            prep_sqe_fn(sqe);
            io_uring_sqe_set_data(sqe, coro.address());
            pending_queue.pop();
        }
    }

    io_uring ring {};
    std::queue<std::tuple<std::coroutine_handle<>, PrepSqeCb, struct timespec>> pending_queue;
    uint64_t tot_sched_time = 0;
    uint64_t tot_sched_count = 0;
    uint64_t tot_submit_items = 0;
    uint64_t tot_submit_count = 0;
    uint64_t tot_pending_queue_size = 0;
    uint64_t tot_pending_queue_sample_count = 0;
};

struct UringAwaiter
{
public:
    explicit UringAwaiter(Scheduler* scheduler)
        : scheduler(scheduler){}
    
    UringAwaiter(const UringAwaiter&) = delete;
    UringAwaiter& operator=(const UringAwaiter&) = delete;
    UringAwaiter(UringAwaiter&&) = delete;
    UringAwaiter& operator=(UringAwaiter&&) = delete;

    virtual ~UringAwaiter() = default;

    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> handle) noexcept
    {
        coro_handle = handle;
        scheduler->schedule(handle, [this](io_uring_sqe* sqe) { prepare_sqe(sqe); });
        return true;
    }

    int await_resume() const noexcept
    {
        auto task_handle = std::coroutine_handle<Task::promise_type>::from_address(coro_handle.address());
        return task_handle.promise().result;
    }

    virtual void prepare_sqe(io_uring_sqe* sqe) = 0;

private:
    Scheduler* scheduler = nullptr;
    std::coroutine_handle<> coro_handle{};
};

struct accept: UringAwaiter
{
public:
    explicit accept(Scheduler* scheduler, int listen_fd, struct sockaddr* paddr, socklen_t* plen, int flags = SOCK_CLOEXEC | SOCK_NONBLOCK)
        : UringAwaiter(scheduler), listen_fd(listen_fd), paddr(paddr), plen(plen), flags(flags) {}

    void prepare_sqe(io_uring_sqe* sqe) override
    {
        io_uring_prep_accept(sqe, listen_fd, paddr, plen, flags);
    }
private:
    int listen_fd = 0;
    struct sockaddr* paddr = nullptr;
    socklen_t* plen = nullptr;
    int flags = 0;
};

struct read: UringAwaiter
{
public:
    explicit read(Scheduler* scheduler, int fd, void* buffer, size_t buffer_size, size_t offset = 0)
        : UringAwaiter(scheduler), fd(fd), buffer(buffer), buffer_size(buffer_size), offset(offset) {}

    void prepare_sqe(io_uring_sqe* sqe) override
    {
        io_uring_prep_read(sqe, fd, buffer, buffer_size, offset);
    }
private:
    int fd = 0;
    void* buffer = nullptr;
    size_t buffer_size = 0;
    size_t offset = 0;
};

struct write: UringAwaiter
{
public:
    explicit write(Scheduler* scheduler, int fd, const void* buffer, size_t buffer_size, size_t offset = 0)
        : UringAwaiter(scheduler), fd(fd), buffer(buffer), buffer_size(buffer_size), offset(offset) {}

    void prepare_sqe(io_uring_sqe* sqe) override
    {
        io_uring_prep_write(sqe, fd, buffer, buffer_size, offset);
    }
private:
    int fd = 0;
    const void* buffer = nullptr;
    size_t buffer_size = 0;
    size_t offset = 0;
};

struct timeout: UringAwaiter
{
public:
    explicit timeout(Scheduler* scheduler, struct __kernel_timespec time_spec, unsigned int count = 0, unsigned int flags = 0)
        : UringAwaiter(scheduler), ts(time_spec), count(count), flags(flags) {}

    void prepare_sqe(io_uring_sqe* sqe) override
    {
        io_uring_prep_timeout(sqe, &ts, count, flags);
    }
private:
    struct __kernel_timespec ts;
    unsigned int count = 0;
    unsigned int flags = 0;
};

struct openat: UringAwaiter
{
public:
    explicit openat(Scheduler* scheduler, int dirfd, const char* pathname, int flags, mode_t mode = 0)
        : UringAwaiter(scheduler), dirfd(dirfd), pathname(pathname), flags(flags), mode(mode) {}

    void prepare_sqe(io_uring_sqe* sqe) override
    {
        io_uring_prep_openat(sqe, dirfd, pathname, flags, mode);
    }
private:
    int dirfd = 0;
    const char* pathname = nullptr;
    int flags = 0;
    mode_t mode = 0;
};

struct close: UringAwaiter
{
public:
    explicit close(Scheduler* scheduler, int fd)
        : UringAwaiter(scheduler), fd(fd) {}

    void prepare_sqe(io_uring_sqe* sqe) override
    {
        io_uring_prep_close(sqe, fd);
    }
private:
    int fd = 0;
};

} // namespace yyasio

