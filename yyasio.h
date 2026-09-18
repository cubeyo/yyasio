#pragma once

#include <cstring>
#include <ctime>
#include <iostream>
#include <coroutine>
#include <functional>
#include <queue>
#include <sys/socket.h>
#include <unordered_map>
#include <liburing.h>

// This macro is not defined by liburing <= 2.0
#ifndef IO_URING_VERSION_MAJOR
#define IO_URING_VERSION_MAJOR 0
#define IO_URING_VERSION_MINOR 0
#endif

#define YYASIO_LIBURING_AT_LEAST(major, minor) \
    (IO_URING_VERSION_MAJOR > (major) || \
     (IO_URING_VERSION_MAJOR == (major) && IO_URING_VERSION_MINOR >= (minor)))

// Cancel fd API not provided by liburing < 2.3
#if !YYASIO_LIBURING_AT_LEAST(2, 3)
#ifndef IORING_ASYNC_CANCEL_FD
#define IORING_ASYNC_CANCEL_FD (1U << 1)
#endif
#endif

#define DEBUG 0
#if DEBUG
#include <iomanip>
constexpr const char* _filename_only(const char* path) {
    const char* name = path;
    for (const char* p = path; *p; ++p) { if (*p == '/') name = p + 1; }
    return name;
}
#define debug_coro(hint, coro) \
    do { \
        struct timespec _ts; \
        clock_gettime(CLOCK_REALTIME, &_ts); \
        struct tm _tm; \
        localtime_r(&_ts.tv_sec, &_tm); \
        char _buf[32]; \
        strftime(_buf, sizeof(_buf), "%Y-%m-%d %H:%M:%S", &_tm); \
        std::cout << "[" << _buf << "." << std::setw(9) << std::setfill('0') << _ts.tv_nsec << "]" \
                  << " [" << _filename_only(__FILE__) << ":" << __LINE__ << "]" \
                  << " [Coro] " << hint << std::hex << coro.address() << std::dec << "(" << reinterpret_cast<uint64_t>(coro.address()) << ")\n"; \
    } while(0)
#else
#define debug_coro(hint, coro) do {} while(0)
#endif

namespace yyasio
{

enum ErrorCode
{
    YYASIO_OK = 0,
    YYASIO_INIT_ERROR,
    YYASIO_SUBMIT_ERROR
};

template <typename T>
class Promise
{
public:
    struct Awaiter
    {
        Awaiter(Promise* promise): _promise(promise) {}
        bool await_ready() const noexcept { return false; }
        bool await_suspend(std::coroutine_handle<> handle) noexcept {
            _promise->_coro = handle;
            return true;
        }
        T await_resume() const noexcept {
            _promise->_coro = nullptr;
            return _promise->_value;
        }
        Promise* _promise;
    };

    Awaiter wait() { return Awaiter(this); }

    void resume(T value)
    {
        if (_coro)
        {
            _value = value;
            _coro.resume();
        }
    }
private:
    std::coroutine_handle<> _coro {};
    T _value;
};

struct CoroIdAwaiter {
    uint64_t id = 0;
    bool await_ready() const noexcept { return false; }
    template<typename promise_type>
    bool await_suspend(std::coroutine_handle<promise_type> handle) noexcept {
        id = reinterpret_cast<uint64_t>(handle.address());
        return false;
    }
    uint64_t await_resume() const noexcept { return id; }
};

inline CoroIdAwaiter current_coro_id()
{
    return CoroIdAwaiter();
}

struct FinalAwaiter {
    std::coroutine_handle<> continuation;
    FinalAwaiter(std::coroutine_handle<> continuation) : continuation(continuation) {}
    bool await_ready() const noexcept { return false; }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> handle) noexcept {
        if (continuation)
        {
            /* Case1: this sub coroutine is not fire-and-forget,
             * the parent is waiting for its result
             * handle to the parent, the parent will destroy
             * frame of sub coroutine in await_resume.
             */
            debug_coro("switch to next coro:", continuation);
            return continuation;
        }
        else
        {
            /* Case2: this sub coroutine is fire-and-forget,
             * the parent does not care about its result,
             * just destroy the frame here.
             */
            debug_coro("destroy coro in final_awaiter(fire-and-forget mode):", handle);
            handle.destroy();
            return std::noop_coroutine(); // return noop coroutine to avoid resuming null handle
        }
    }
    void await_resume() const noexcept { }
};

template<typename T>
struct Task
{
    struct promise_type
    {
        T result;
        Task get_return_object() {
            auto coro = std::coroutine_handle<promise_type>::from_promise(*this);
            return Task(coro);
        }
        // always suspend on initializetion,
        // will be resumed in either case:
        // 1. caller co_await on this task, will resume on caller's await_suspend
        // 2. caller fire-and-forget, will resume on detach() called
        std::suspend_always initial_suspend() noexcept { return {}; }
        FinalAwaiter final_suspend() noexcept { return {_caller}; }
        void return_value(T value)
        {
            result = value;
        }
        void unhandled_exception() {std::terminate(); }
        ~promise_type() = default;
        std::coroutine_handle<> _caller;
    };

    std::coroutine_handle<promise_type> _callee = nullptr;
    Task(std::coroutine_handle<promise_type> handle) : _callee(handle)
    {
        debug_coro("create coro for non-void task:", _callee);
    }
    Task(Task&& other) noexcept : _callee(other._callee) { other._callee = nullptr; }
    Task& operator=(Task&&) = delete;

    // Frame is self-destroyed in FinalAwaiter::await_suspend.
    // ~Task() is intentionally a no-op to support fire-and-forget usage.
    ~Task() = default;

    void detach()
    {
        _callee.resume();
        _callee = nullptr;
    }

    // to support co_await on Task
    auto operator co_await() const noexcept {
        struct Awaiter {
            std::coroutine_handle<promise_type> _callee;
            Awaiter(std::coroutine_handle<promise_type> callee) : _callee(callee) {}
            bool await_ready() const noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept
            {
                _callee.promise()._caller = caller;
                debug_coro("suspend coro:", caller);
                return _callee;
            }
            T await_resume() const noexcept
            {
                // awaiter inside parent's frame,
                // can safely destroy sub coroutine's frame here
                debug_coro("subcoro finished:", _callee);
                T result = _callee.promise().result;
                debug_coro("destroy coro:", _callee);
                _callee.destroy();
                return result;
            }
        };
        return Awaiter(_callee);
    }
};

template<>
struct Task<void>
{
    struct promise_type
    {
        Task get_return_object() {
            auto coro = std::coroutine_handle<promise_type>::from_promise(*this);
            return Task(coro);
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        FinalAwaiter final_suspend() noexcept { return {_caller}; }
        void return_void() {}
        void unhandled_exception() {std::terminate(); }
        ~promise_type() = default;
        std::coroutine_handle<> _caller; // will set value on await_suspend
    };

    std::coroutine_handle<promise_type> _callee = nullptr;
    Task(std::coroutine_handle<promise_type> handle) : _callee(handle)
    {
        debug_coro("create coro for void task:", _callee);
    }
    Task(Task&& other) noexcept : _callee(other._callee) { other._callee = nullptr; }
    Task& operator=(Task&&) = delete;
    ~Task() = default;

    void detach()
    {
        _callee.resume();
        _callee = nullptr;
    }

    // to support co_await on Task
    auto operator co_await() const noexcept {
        struct Awaiter {
            std::coroutine_handle<promise_type> _callee;
            Awaiter(std::coroutine_handle<promise_type> handle) : _callee(handle) {}
            bool await_ready() const noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept
            {
                _callee.promise()._caller = caller;
                debug_coro("suspend coro:", caller);
                return _callee;
            }
            void await_resume() const noexcept
            {
                debug_coro("subcoro finished:", _callee);
                debug_coro("destroy coro:", _callee);
                _callee.destroy();
            }
        };
        return Awaiter(_callee);
    }
};

using PrepSqeClosure = std::function<void(io_uring_sqe*)>;

class Scheduler
{
public:
    ErrorCode init(size_t entries) {
        if (io_uring_queue_init(entries, &ring, 0) < 0)
        {
            return YYASIO_INIT_ERROR;
        }
        return YYASIO_OK;
    }

    void schedule(std::coroutine_handle<> coro, PrepSqeClosure prep_seq_fn, int* presult)
    {
        struct timespec enter_ts {};
        clock_gettime(CLOCK_MONOTONIC, &enter_ts);
        debug_coro("schedule coro:", coro);
        pending_queue.push(std::make_tuple(coro, prep_seq_fn, presult, enter_ts));
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
            else if (submitted == 0)
            {
                std::cerr << "WARNING: io_uring_submit_and_wait returned 0 (no SQEs submitted), pending_queue.size=" << pending_queue.size() << "\n";
            }

            tot_submit_items += submitted;
            tot_submit_count += 1;

            io_uring_cqe* cqe = nullptr;
            while (io_uring_peek_cqe(&ring, &cqe) == 0)
            {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                io_uring_cqe_seen(&ring, cqe);
                auto inflight_idx = cqe->user_data;
                auto iter = inflight_map.find(inflight_idx);
                if (iter == inflight_map.end())
                {
                    // Duplicate CQE: the SQE already completed and the coroutine may
                    // have been resumed/destroyed. Observed on WSL2 kernels where a
                    // single connect SQE can deliver two or more CQEs.
                    std::cerr << "WARNING: duplicate CQE for io_cnt=" << inflight_idx << "\n";
                    continue;
                }

                auto [coro_addr, res_addr] = iter->second;
                auto handle = std::coroutine_handle<>::from_address(reinterpret_cast<void*>(coro_addr));
                inflight_map.erase(iter);
                debug_coro("io returned:", handle);
                if (res_addr)
                {
                    *res_addr = cqe->res;
                }

                handle.resume();
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
                std::cerr << "WARNING: io_uring_get_sqe returned NULL! pending_queue.size=" << pending_queue.size() << "\n";
                break;
            }
            auto [coro, prep_sqe_fn, res_addr, enter_ts] = pending_queue.front();

            struct timespec sched_ts {};
            clock_gettime(CLOCK_MONOTONIC, &sched_ts);
            uint64_t cost_ns = (static_cast<uint64_t>(sched_ts.tv_sec) * 1000000000ULL + sched_ts.tv_nsec) -
                               (static_cast<uint64_t>(enter_ts.tv_sec) * 1000000000ULL + enter_ts.tv_nsec);
            tot_sched_time += cost_ns;
            tot_sched_count++;

            debug_coro("prepare sqe for coro:", coro);
            prep_sqe_fn(sqe);
            inflight_map[io_cnt] = std::make_tuple(coro.address(), res_addr);
            io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(static_cast<uintptr_t>(io_cnt)));
            io_cnt++;
            pending_queue.pop();
        }
    }

    io_uring ring {};
    std::queue<std::tuple<std::coroutine_handle<>, PrepSqeClosure, int*, struct timespec>> pending_queue;
    uint64_t io_cnt = 0;
    std::unordered_map<uint64_t, std::tuple<void*, int*>> inflight_map; // io_idx -> <coro addr, result addr>

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
    explicit UringAwaiter(Scheduler* scheduler, PrepSqeClosure prepare_sqe_fn)
        : scheduler(scheduler), _prepare_sqe_fn(prepare_sqe_fn) {}
    
    // UringAwaiter(const UringAwaiter&) = delete;
    // UringAwaiter& operator=(const UringAwaiter&) = delete;
    // UringAwaiter(UringAwaiter&&) = delete;
    // UringAwaiter& operator=(UringAwaiter&&) = delete;

    virtual ~UringAwaiter()
    {
        debug_coro("awaiter destroyed by coro:", coro_handle);
    }

    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> handle) noexcept
    {
        coro_handle = handle;
        scheduler->schedule(handle, _prepare_sqe_fn, &result);
        return true;
    }

    int await_resume() const noexcept
    {
        debug_coro("await_resume for coro:", coro_handle);
        return result;
    }

private:
    Scheduler* scheduler = nullptr;
    PrepSqeClosure _prepare_sqe_fn;
    std::coroutine_handle<> coro_handle{};
    int result = 0;
};

inline UringAwaiter accept(Scheduler* scheduler, int listen_fd, struct sockaddr* paddr, socklen_t* plen, int flags = SOCK_CLOEXEC | SOCK_NONBLOCK)
{
    PrepSqeClosure prepare_sqe_cb = [listen_fd, paddr, plen, flags](io_uring_sqe* sqe) -> void {
        io_uring_prep_accept(sqe, listen_fd, paddr, plen, flags);
    };
    return UringAwaiter{ scheduler, prepare_sqe_cb };
}

inline UringAwaiter read(Scheduler* scheduler, int fd, void* buffer, size_t buffer_size, size_t offset = 0)
{
    PrepSqeClosure prepare_sqe_cb = [fd, buffer, buffer_size, offset](io_uring_sqe* sqe) -> void {
        io_uring_prep_read(sqe, fd, buffer, buffer_size, offset);
    };
    return UringAwaiter{ scheduler, prepare_sqe_cb };
}

inline UringAwaiter write(Scheduler* scheduler, int fd, const void* buffer, size_t buffer_size, size_t offset = 0)
{
    PrepSqeClosure prepare_sqe_cb = [fd, buffer, buffer_size, offset](io_uring_sqe* sqe) -> void {
        io_uring_prep_write(sqe, fd, buffer, buffer_size, offset);
    };
    return UringAwaiter{ scheduler, prepare_sqe_cb };
}

// for kernel <= 5.9, time_spec should be valid until the operation completes
inline UringAwaiter timeout(Scheduler* scheduler, struct __kernel_timespec* time_spec, unsigned int count = 0, unsigned int flags = 0)
{
    PrepSqeClosure prepare_sqe_cb = [time_spec, count, flags](io_uring_sqe* sqe) -> void {
        io_uring_prep_timeout(sqe, time_spec, count, flags);
    };
    return UringAwaiter{ scheduler, prepare_sqe_cb };
}

inline UringAwaiter openat(Scheduler* scheduler, int dirfd, const char* pathname, int flags, mode_t mode = 0)
{
    PrepSqeClosure prepare_sqe_cb = [dirfd, pathname, flags, mode](io_uring_sqe* sqe) -> void {
        io_uring_prep_openat(sqe, dirfd, pathname, flags, mode);
    };
    return UringAwaiter{ scheduler, prepare_sqe_cb };
}

inline UringAwaiter cancel_fd(Scheduler* scheduler, int fd, unsigned flags = 0)
{
    PrepSqeClosure prepare_sqe_cb = [fd, flags](io_uring_sqe* sqe) -> void {
#if YYASIO_LIBURING_AT_LEAST(2, 3)
        io_uring_prep_cancel_fd(sqe, fd, flags);
#else
        // Requires: kernel >= 5.19
        io_uring_prep_cancel(sqe, nullptr, static_cast<int>(flags | IORING_ASYNC_CANCEL_FD));
        sqe->fd = fd;
#endif
    };
    return UringAwaiter{ scheduler, prepare_sqe_cb };
}

inline UringAwaiter close(Scheduler* scheduler, int fd)
{
    PrepSqeClosure prepare_sqe_cb = [fd](io_uring_sqe* sqe) -> void {
        io_uring_prep_close(sqe, fd);
    };
    return UringAwaiter{ scheduler, prepare_sqe_cb };
}

inline UringAwaiter connect(Scheduler* scheduler, int fd, const struct sockaddr *addr, socklen_t len)
{
    PrepSqeClosure prepare_sqe_cb = [fd, addr, len](io_uring_sqe* sqe) -> void {
        io_uring_prep_connect(sqe, fd, addr, len);
    };
    return UringAwaiter{ scheduler, prepare_sqe_cb };
}

inline UringAwaiter listen(Scheduler* scheduler, int fd, int backlog = SOMAXCONN)
{
    PrepSqeClosure prepare_sqe_cb = [fd, backlog](io_uring_sqe* sqe) -> void {
        io_uring_prep_listen(sqe, fd, backlog);
    };
    return UringAwaiter{ scheduler, prepare_sqe_cb };
}

} // namespace yyasio

