#pragma once

#include <cstdint>
#include <cstring>
#include <ctime>
#include <iostream>
#include <coroutine>
#include <functional>
#include <queue>
#include <sys/socket.h>
#include <unordered_map>
#include <liburing.h>
#include <variant>

#ifndef PRINT_STACK_ON_EXCEPTION
#define PRINT_STACK_ON_EXCEPTION 0
#endif

#if PRINT_STACK_ON_EXCEPTION

#include <libunwind.h>
#include <cxxabi.h>
#include <sstream>

namespace yyasio
{

namespace detail
{
struct CoroDebugInfo
{
    void* caller = nullptr;
    std::string proc;
    unw_word_t ip = 0;
    unw_word_t off = 0;

    CoroDebugInfo()
    {
        proc.resize(256);
    }
};

inline void get_debuginfo(CoroDebugInfo* hint)
{
    unw_cursor_t cursor;
    unw_context_t ctx;

    unw_getcontext(&ctx);
    unw_init_local(&cursor, &ctx);

    if (unw_step(&cursor) <= 0)
    {
        return;
    }

    if (unw_step(&cursor) > 0)
    {
        unw_get_reg(&cursor, UNW_REG_IP, &hint->ip);
        unw_get_proc_name(&cursor, hint->proc.data(), hint->proc.size(), &hint->off);
    }
}

inline void print_hardware_stack()
{
    unw_context_t ctx;
    unw_cursor_t cursor;

    unw_getcontext(&ctx);
    unw_init_local(&cursor, &ctx);

    std::cerr << "==== Hardware stack (libunwind) ====\n";

    while (unw_step(&cursor) > 0)
    {
        unw_word_t ip, off;
        char sym[256]{};
        unw_get_reg(&cursor, UNW_REG_IP, &ip);

        int rc = unw_get_proc_name(&cursor, sym, sizeof(sym), &off);
        if (rc != 0)
        {
            std::cerr << "0x" << std::hex << ip << " : ???\n";
            continue;
        }

        // demangle c++符号
        int status;
        char* demangled = abi::__cxa_demangle(sym, nullptr, nullptr, &status);
        if (status == 0 && demangled)
        {
            std::cerr << "0x" << std::hex << ip << " : " << demangled << " +0x" << off << "\n";
            free(demangled);
        }
        else
        {
            std::cerr << "0x" << std::hex << ip << " : " << sym << " +0x" << off << "\n";
        }
    }
    std::cerr << "====================================\n\n";
}

class DebugInfoStore
{
public:
    static void store_debuginfo(void* coro_addr, detail::CoroDebugInfo debuginfo)
    {
        s_debuginfo[coro_addr] = std::move(debuginfo);
    }

    static void set_caller(void* callee, void* caller)
    {
        auto iter = s_debuginfo.find(callee);
        if (iter != s_debuginfo.end())
        {
            iter->second.caller = caller;
        }
    }

    static void remove_coro(void* coro_addr)
    {
        s_debuginfo.erase(coro_addr);
    }

    inline static std::unordered_map<void*, detail::CoroDebugInfo> s_debuginfo; // maps from coroutine address to frame hint
};

inline std::string get_demangle_debuginfo(const detail::CoroDebugInfo& debuginfo)
{
    int status;
    std::stringstream os;
    char* demangled = abi::__cxa_demangle(debuginfo.proc.c_str(), nullptr, nullptr, &status);
    if (status == 0 && demangled)
    {
        os << "0x" << std::hex << debuginfo.ip << " : " << demangled << " +0x" << debuginfo.off;
        free(demangled);
    }
    else
    {
        os << "0x" << std::hex << debuginfo.ip << " : " << debuginfo.proc << " +0x" << debuginfo.off;
    }
    return os.str();
}

inline void print_coroutine_stack(void* coro_addr)
{
    std::cerr << "==== Coroutine stack ====\n";
    while (coro_addr)
    {
        const auto iter = detail::DebugInfoStore::s_debuginfo.find(coro_addr);
        if (iter == detail::DebugInfoStore::s_debuginfo.end())
            break;

        std::cerr << detail::get_demangle_debuginfo(iter->second) << "\n";
        coro_addr = iter->second.caller;
    }
}

} // namespace detail

} // namespace yyasio

#else // !PRINT_STACKTRACE_ON_EXCEPTION

namespace yyasio
{

namespace detail
{
struct CoroDebugInfo {};

inline void get_debuginfo(CoroDebugInfo* hint) {}

inline void print_hardware_stack()
{
    std::cerr << "Dump hardware stack disabled, compile with -DPRINT_CORO_STACK_ON_EXCEPTION=1\n";
}

class DebugInfoStore
{
public:
    static void store_debuginfo(void* coro_addr, detail::CoroDebugInfo debuginfo) {}
    static void set_caller(void* callee, void* caller) {}
    static void remove_coro(void* coro_addr) {}
};

inline void print_coroutine_stack(void* coro_addr)
{
    std::cerr << "Dump coro stack disabled, compile with -DPRINT_CORO_STACK_ON_EXCEPTION=1\n";
}
}

}
#endif

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

#define PRINT_CORO_RUNTIMEINFO 0
#if PRINT_CORO_RUNTIMEINFO
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

// One coroutine can co_await on this Event
// and another coroutine can set it to wakeup the first coroutine
template <typename T>
class Event
{
public:
    struct Awaiter
    {
        Awaiter(Event* event): _event(event) {}
        bool await_ready() const noexcept { return false; }
        bool await_suspend(std::coroutine_handle<> handle) noexcept {
            _event->_coro = handle;
            return true;
        }
        T await_resume() const noexcept {
            _event->_coro = nullptr;
            return _event->_value;
        }
        Event* _event;
    };

    Awaiter wait() { return Awaiter(this); }

    void set(T value)
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

namespace detail
{
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

} // namespace detail
inline detail::CoroIdAwaiter current_coro_id()
{
    return detail::CoroIdAwaiter();
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
            detail::DebugInfoStore::remove_coro(handle.address());
            handle.destroy();
            return std::noop_coroutine(); // return noop coroutine to avoid resuming null handle
        }
    }
    void await_resume() const noexcept { }
};

// Helper base to provide return_value or return_void via specialization.
// GCC 11 doesn't support requires clauses to filter member functions in class templates.
template<typename T>
struct promise_return_base
{
    T result;
    void return_value(T value) { result = value; }
};

template<>
struct promise_return_base<void>
{
    std::monostate result;
    void return_void() {}
};

template<typename T>
struct Task
{
    struct promise_type: public promise_return_base<T>
    {
        std::suspend_always initial_suspend() noexcept { return {}; }
        FinalAwaiter final_suspend() noexcept { return {_caller}; }
        Task get_return_object() {
            auto coro = std::coroutine_handle<promise_type>::from_promise(*this);

            // This is the only position we can get coroutine's address
            // But we still cannot get parent coroutine's address here
            // Parent coroutine's address will be updated when co_await called (via await_suspend)
            detail::CoroDebugInfo debug_info;
            get_debuginfo(&debug_info);
            detail::DebugInfoStore::store_debuginfo(coro.address(), std::move(debug_info));
            return Task(coro);
        }

        void unhandled_exception() noexcept {
            auto coro = std::coroutine_handle<promise_type>::from_promise(*this);
            detail::print_hardware_stack();
            detail::print_coroutine_stack(coro.address());

            std::terminate();
        }
        
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
                detail::DebugInfoStore::set_caller(_callee.address(), caller.address());
                debug_coro("suspend coro:", caller);
                return _callee;
            }

            T await_resume() const noexcept requires (!std::same_as<T, void>)
            {
                // awaiter inside parent's frame,
                // can safely destroy sub coroutine's frame here
                debug_coro("subcoro finished:", _callee);
                auto result = _callee.promise().result;
                debug_coro("destroy coro:", _callee);
                detail::DebugInfoStore::remove_coro(_callee.address());
                _callee.destroy();
                return static_cast<T>(result);
            }
            void await_resume() const noexcept requires std::same_as<T, void>
            {
                debug_coro("subcoro finished:", _callee);
                debug_coro("destroy coro:", _callee);
                detail::DebugInfoStore::remove_coro(_callee.address());
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
        // These two flags are supported by kernel >= 6.0
        if (io_uring_queue_init(entries, &ring, IORING_SETUP_SINGLE_ISSUER|IORING_SETUP_DEFER_TASKRUN) < 0 &&
            io_uring_queue_init(entries, &ring, 0) < 0)
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

            tot_submit_items += submitted;
            tot_submit_count += 1;

            io_uring_cqe* cqe = nullptr;
            while (io_uring_peek_cqe(&ring, &cqe) == 0)
            {
                auto inflight_idx = io_uring_cqe_get_data64(cqe);
                int cqe_res = cqe->res;
                io_uring_cqe_seen(&ring, cqe);

                auto iter = inflight_map.find(inflight_idx);
                if (iter == inflight_map.end())
                {
                    std::cerr << "duplicate cqe for index = " << inflight_idx << "\n";
                    // Duplicate CQE: the SQE already completed and the coroutine may
                    // have been resumed/destroyed. Skip this CQE and continue
                    // processing the remaining ones.
                    continue;
                }

                auto [coro_addr, res_addr] = iter->second;
                auto handle = std::coroutine_handle<>::from_address(reinterpret_cast<void*>(coro_addr));
                inflight_map.erase(iter);
                debug_coro("io returned:", handle);
                if (res_addr)
                {
                    *res_addr = cqe_res;
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
                std::cerr << "WARNING: io_uring_get_sqe returned NULL! pending_queue.size="
                          << pending_queue.size()
                          << ", ring_entries=" << ring.sq.ring_entries
                          << ", khead=" << *ring.sq.khead
                          << ", ktail=" << *ring.sq.ktail << "\n";
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
            memset(sqe, 0, sizeof(*sqe)); // ensure SQE is clean before prep
            prep_sqe_fn(sqe);
            inflight_map[io_cnt] = std::make_tuple(coro.address(), res_addr);
            io_uring_sqe_set_data64(sqe, io_cnt);
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
inline UringAwaiter timeout(Scheduler* scheduler, struct __kernel_timespec* time_spec, unsigned int count = 1, unsigned int flags = 0)
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

