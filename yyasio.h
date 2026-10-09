#pragma once

#include <cassert>
#include <cerrno>
#include <chrono>
#include <limits>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <iostream>
#include <coroutine>
#include <functional>
#include <vector>
#include <climits>
#include <queue>
#include <memory>
#include <set>
#include <unistd.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <concepts>
#include <unordered_map>
#include <liburing.h>
#include <variant>
#include <atomic>
#include <thread>
#include <utility>
#include <type_traits>

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
// Both arguments stay name-checked even when logging is compiled out:
// sizeof(decltype(...)) is not evaluated, so there is no runtime cost, but a call
// site referencing a renamed or not-yet-declared variable still breaks the build
// here instead of only when PRINT_CORO_RUNTIMEINFO gets turned on.
#define debug_coro(hint, coro) \
    do { (void)sizeof(decltype(hint)); (void)sizeof(decltype(coro)); } while(0)
#endif

// Forward declaration for the statx() wrapper, same trick liburing.h uses for its
// io_uring_prep_statx: the wrapper only carries the pointer, so <sys/stat.h> stays
// out of this header. Whoever reads the result includes <sys/stat.h> (and needs it
// anyway for the AT_* flags and the STATX_* mask bits).
struct statx;

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
        bool await_ready() const noexcept {
            // if has pending value, return true to skip suspend coroutine
            // else, return false to suspend coroutine
            return _event->_pending;
        }
        bool await_suspend(std::coroutine_handle<> handle) noexcept {
            assert (!_event->_coro);
            _event->_coro = handle;
            return true;
        }
        T await_resume() const
        {
            _event->_coro = nullptr;
            _event->_pending = false;
            return std::move(_event->_value);
        }
        Event* _event;
    };

    // CAUTION: only allow one coroutine waiting on this event
    Awaiter wait() { return Awaiter(this); }

    // CAUTION: if set() is called multiple times before the coroutine is resumed,
    // the last value will be returned and previous set values will be discarded.
    // CONTRACT: resume() is inline, so the waiting coroutine runs on the caller's
    // stack: it re-enters await_resume(), its locals and its frame are touched here,
    // and it keeps executing until its next suspension. Calling set() from another
    // thread therefore moves that whole coroutine into this thread -- it would reach
    // Scheduler::schedule()/abandon() (they assert ring affinity) and it would race
    // every non-thread-safe object it uses. set() returns only after the waiter has
    // run, which callers and tests rely on; handing the resume off to the loop thread
    // to make this cross-thread safe would break that synchronously-observed order.
    void set(T value)
    {
        _value = std::move(value);
        if (_coro)
        {
            _coro.resume();
        }
        else
        {
            _pending = true;
        }
    }

    Event() = default;
    Event(const Event& other) = delete;
    Event& operator=(const Event&) = delete;
    Event(Event&& other) = delete;
    Event& operator=(Event&& other) = delete;
private:
    std::coroutine_handle<> _coro {};
    T _value {};
    bool _pending = false;
};

struct Mutex
{
    struct Awaiter
    {
        Awaiter(Mutex* mtx): _mtx(mtx) {}
        // Fast path: grab the lock here and now, co_await returns without suspending.
        // Otherwise leave _locked == true and fall through to await_suspend.
        bool await_ready() noexcept {
            if (!_mtx->_locked)
            {
                _mtx->_locked = true;
                return true;
            }
            return false;
        }
        bool await_suspend(std::coroutine_handle<> handle) noexcept {
            _mtx->_waiters.push(handle);
            return true;
        }
        void await_resume() const noexcept { }
        Mutex* _mtx;
    };

    struct LockGuard
    {
        LockGuard(Mutex& mtx): _mtx(mtx) {}
        LockGuard(const LockGuard&) = delete;
        LockGuard& operator=(const LockGuard&) = delete;
        LockGuard(LockGuard&&) = delete;
        LockGuard& operator=(LockGuard&&) = delete;
        ~LockGuard() { _mtx.unlock(); }
    private:
        Mutex& _mtx;
    };

    struct AwaiterWithGuard
    {
        AwaiterWithGuard(Mutex* mtx): _mtx(mtx) {}
        bool await_ready() noexcept
        {
            if (!_mtx->_locked)
            {
                _mtx->_locked = true;
                return true;
            }
            return false;
        }
        bool await_suspend(std::coroutine_handle<> handle) noexcept {
            _mtx->_waiters.push(handle);
            return true;
        }
        LockGuard await_resume() const noexcept
        {
            return {*_mtx};
        }
        Mutex* _mtx;
    };

    Mutex() = default;

    Mutex(const Mutex& other) = delete;
    Mutex& operator=(const Mutex&) = delete;
    Mutex(Mutex&& other) = delete;
    Mutex& operator=(Mutex&& other) = delete;

    AwaiterWithGuard lock_guard() { return AwaiterWithGuard(this); }
    Awaiter lock() { return Awaiter(this); }

    // CONTRACT: like Event::set(), unlock() resumes the next waiter inline, so that
    // coroutine takes over the rest of this thread's stack (and of this call's stack
    // frames) until it suspends again -- stay on the thread that locks.
    void unlock()
    {
        if (_waiters.empty())
        {
            _locked = false;
            return;
        }
        // Keep _locked == true: ownership is transferred to the woken waiter,
        // its await_resume() completes without touching the flag.
        auto next = _waiters.front();
        _waiters.pop();
        next.resume();
    }

    bool try_lock() noexcept
    {
        if (_locked)
        {
            return false;
        }
        _locked = true;
        return true;
    }

    // Number of coroutines currently suspended in lock()
    size_t waiter_count() const { return _waiters.size(); }

private:
    bool _locked = false;
    std::queue<std::coroutine_handle<>> _waiters;
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
    // value arrives by value, so move it in: a copy assignment here would make
    // move-only results (unique_ptr and friends) fail to compile
    void return_value(T value) { result = std::move(value); }
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
            return Task(coro, _done_guard);
        }

        void unhandled_exception() noexcept {
            auto coro = std::coroutine_handle<promise_type>::from_promise(*this);
            detail::print_hardware_stack();
            detail::print_coroutine_stack(coro.address());

            std::terminate();
        }

        ~promise_type()
        {
            *_done_guard = true;
        }

        std::shared_ptr<std::atomic<bool>> _done_guard = std::make_shared<std::atomic<bool>>(false);
        std::coroutine_handle<> _caller { nullptr };
    };

    std::coroutine_handle<promise_type> _callee = nullptr;
    std::shared_ptr<std::atomic<bool>> _done_guard = nullptr;

    Task() = default;

    Task(std::coroutine_handle<promise_type> handle, std::shared_ptr<std::atomic<bool>> done_guard) : _callee(handle), _done_guard(done_guard)
    {
        debug_coro("create coro for non-void task:", _callee);
    }

    Task(const Task& other) = delete;
    Task& operator=(const Task&) = delete;

    Task(Task&& other) noexcept : _callee(other._callee), _done_guard(std::move(other._done_guard))
    { other._callee = nullptr; }

    Task& operator=(Task&& other) noexcept
    {
        if (this == &other) [[unlikely]]
        {
            return *this;
        }
        _callee = other._callee;
        _done_guard = std::move(other._done_guard);
        other._callee = nullptr;
        return *this;
    }

    // Frame is self-destroyed in FinalAwaiter::await_suspend.
    // ~Task() is intentionally a no-op to support fire-and-forget usage.
    ~Task() = default;

    void detach()
    {
        assert(_callee);
        _callee.resume();
        _callee = nullptr;
    }

    // Returns true only after the coroutine frame has been destroyed
    // (~promise_type sets the shared done_guard to true). Combined with detach(),
    // this is the way to observe completion of a fire-and-forget coroutine.
    // Note: for `co_await task` / temporaries, the Task object usually dies
    // together with the frame, so there is no window to observe it.
    bool is_finished() const
    {
        return _done_guard && _done_guard->load();
    }

    // to support co_await on Task
    auto operator co_await() const noexcept {
        assert(_callee);
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

            T await_resume() const requires (!std::same_as<T, void>)
            {
                // awaiter inside parent's frame,
                // can safely destroy sub coroutine's frame here
                debug_coro("subcoro finished:", _callee);
                auto result = std::move(_callee.promise().result);
                debug_coro("destroy coro:", _callee);
                detail::DebugInfoStore::remove_coro(_callee.address());
                _callee.destroy();
                return static_cast<T>(std::move(result));
            }
            void await_resume() const requires std::same_as<T, void>
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
        if (status != Uninitialized)
        {
            std::cerr << "Scheduler already initialized" << std::endl;
            return YYASIO_INIT_ERROR;
        }

        // These two flags are supported by kernel >= 6.0
        if (io_uring_queue_init(entries, &ring, IORING_SETUP_SINGLE_ISSUER|IORING_SETUP_DEFER_TASKRUN) < 0 &&
            io_uring_queue_init(entries, &ring, 0) < 0)
        {
            return YYASIO_INIT_ERROR;
        }

        if (!init_control_pipe())
        {
            io_uring_queue_exit(&ring);
            return YYASIO_INIT_ERROR;
        }

        // io_uring is set up SINGLE_ISSUER: only this thread may submit. The plain
        // containers below (pending_queue/inflight_map/abandoned) are not thread-safe
        // either, so this thread is also the only one allowed on the IO paths.
        // A value-initialized std::thread::id denotes no thread at all, so it doubles
        // as the "no owner recorded yet" sentinel.
        ring_owner.store(std::this_thread::get_id(), std::memory_order_release);

        status = Initialized;

        return YYASIO_OK;
    }

    bool is_accepting_io()
    {
        assert (is_on_ring_thread() && "Scheduler::is_accepting_io() must run on the init()/run() thread");
        return status == Running || status == Initialized;
    }

    int schedule(std::coroutine_handle<> coro, PrepSqeClosure prep_seq_fn, int* presult, std::uint64_t* index = nullptr)
    {
        assert(is_on_ring_thread() && "Scheduler::schedule() must run on the init()/run() thread");
        if (!is_accepting_io()) [[unlikely]]
        {
            std::cerr << "Scheduler not running" << std::endl;
            return -ECANCELED;
        }

        debug_coro("schedule coro:", coro);
        uint64_t cancel_index = append_pending_queue(coro, prep_seq_fn, presult);
        if (index)
            *index = cancel_index;
        return 0;
    }

    // Calling abandon means the caller does not care about the io result and will 
    // take responsibility for resuming the coroutine. Thus, the scheduler will ignore
    // cqe for this io and will NOT call resume for the coroutine which is suspended
    // by inflight io.
    // abandon must be called inside some coroutine
    // (from hardware thread which calls scheduler.run())
    void abandon(uint64_t index)
    {
        assert(is_on_ring_thread() && "Scheduler::abandon() must run on the init()/run() thread");
        bool io_is_pending = !pending_queue.empty() && index >= pending_queue.front().index && index <= pending_queue.back().index;
        bool io_is_inflight = inflight_map.contains(index);
        if (!io_is_pending && !io_is_inflight)
        {
            return;
        }

        if (io_is_inflight)
        {
            // submit a pending item into pending queue, set the coro address to nullptr
            // which the scheduler will not resume anything when cqe is peeked
            auto prep_cancel_fn = [index](io_uring_sqe* sqe) -> void {
                io_uring_prep_cancel64(sqe, index, 0);
            };

            // not care about result
            append_pending_queue(nullptr, prep_cancel_fn, nullptr);
        }

        abandoned.insert(index);
    }

    // stop can be called from external thread
    void stop()
    { 
        ::write(control_pipe[1], &kStopTicket, sizeof(kStopTicket));
    }

    void run()
    {
        // Until init() succeeded the ring is zero-initialized, so every liburing call
        // below would dereference NULL ring pages and crash. Refuse to start instead.
        if (status != Initialized)
        {
            std::cerr << "Scheduler::run() called without a successful init()\n";
            return;
        }

        assert(is_on_ring_thread() && "Scheduler::run() must be called on the init() thread (SINGLE_ISSUER)");

        status = Running;
        while (true)
        {
            if (!run_once()) [[unlikely]]
                break;
            if (status != Running && 
                pending_queue.empty() && 
                inflight_map.empty() &&
                coros_waiting_for_extern_wakeup.empty()) [[unlikely]]
                break;
        }

        do_recycle();
    }

    void do_stop()
    {
        status = Stopping;
    }

    // register a coro waiting for a external wakeup
    int register_coro_waiting_extern(std::coroutine_handle<> coro, std::uint64_t* ticket)
    {
        assert(is_on_ring_thread() && "Scheduler::register_coro_waiting_for_thread() must be called on the init() thread (SINGLE_ISSUER)");

        if (!is_accepting_io())
        {
            return -ECANCELED;
        }
        auto index = wakeup_tickets++;
        coros_waiting_for_extern_wakeup[index] = coro;
        *ticket = index;
        return 0;
    }

    // This function is called from the external worker thread
    void wakeup_coro_by_ticket(std::uint64_t ticket)
    {
        // from man 7 pipe:
        // POSIX.1 says that write(2)s of less than PIPE_BUF bytes must be atomic:
        // the output data is written to the pipe as a contiguous sequence.
        static_assert(sizeof(std::uint64_t) < PIPE_BUF);
        while (true)
        {
            ssize_t ret = ::write(control_pipe[1], &ticket, sizeof(std::uint64_t));
            
            if (ret == (ssize_t)sizeof(ticket))
                return;

            if (ret < 0 && errno == EAGAIN)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
                continue;
            }

            int e = errno;
            std::cerr << "notify write failed errno = " << e << "\n";
            std::terminate();
        }
    }

    void print_sched_stats()
    {
        std::cout << "Total schedule time: " << tot_sched_time << " ns\n";
        std::cout << "Total schedule count: " << tot_sched_count << "\n";
        if (tot_sched_count > 0)
            std::cout << "Average schedule time: " << tot_sched_time / tot_sched_count << " ns\n";
        std::cout << "Total submit items: " << tot_submit_items << "\n";
        std::cout << "Total submit count: " << tot_submit_count << "\n";
        if (tot_submit_count > 0)
            std::cout << "Average submit items per submit: " << tot_submit_items / tot_submit_count << "\n";
        if (tot_pending_queue_sample_count > 0)
            std::cout << "Average pending queue size: " << tot_pending_queue_size / tot_pending_queue_sample_count << "\n";
    }

private:
    void handle_cqe(io_uring_cqe* cqe)
    {
        auto inflight_idx = io_uring_cqe_get_data64(cqe);
        if (inflight_idx == kUserDataControlPipe) [[unlikely]]
        {
            bool more = cqe->flags & IORING_CQE_F_MORE;
            int cqe_res = cqe->res;
            io_uring_cqe_seen(&ring, cqe);
            on_control_pipe_received();

            // if control_pipe[1] closed unexpectedly, polling pipe[0] will get POLLHUP
            // just print message as hint
            if (cqe_res & (POLLHUP | POLLERR | POLLNVAL))
                std::cerr << "control pipe got POLLHUP/ERR, stop() may never take effect\n";

            if (!more)
            {
                control_pipe_armed = false;
                std::cerr << "control pipe terminated unexpectedly, res=" << cqe_res << std::endl;
            }
            return;
        }

        int cqe_res = cqe->res;
        io_uring_cqe_seen(&ring, cqe);
        on_inflight_io_returned(inflight_idx, cqe_res);
    }

    bool run_once()
    {
        batch_prepare_sqe();

        int submitted = submit_sqe();
        if (submitted < 0)
        {
            return false;
        }

        tot_submit_items += submitted;
        tot_submit_count += 1;

        size_t cqe_cnt = 0;
        io_uring_cqe* cqe = nullptr;
        while (io_uring_peek_cqe(&ring, &cqe) == 0)
        {
            cqe_cnt++;
            handle_cqe(cqe);
        }

        // cqe_cnt == 0 means we did nothing in the loop,
        // not adding any new items to pending queue, so
        // there is no work to do, just block on wait_cqe.
        if (cqe_cnt == 0)
        {
            int ret = io_uring_wait_cqe(&ring, &cqe);
            if (ret < 0)
            {
                std::cerr << "io_uring_wait_cqe failed:, ret = " << ret << "\n";
                std::terminate();
            }
            handle_cqe(cqe);
        }

        return true;
    }

    int submit_sqe()
    {
        int submitted = 0;
        while(true)
        {
            submitted = io_uring_submit_and_wait(&ring, 1);
            if (submitted >= 0) [[likely]]
                return submitted;
            if (submitted == -EINTR || submitted == -EAGAIN)
            {
                continue;
            }
            std::cerr << "io_uring_submit_and_wait failed:, ret = " << submitted << "\n";
            return submitted;
        }
    }

    void on_ticket_received(uint64_t ticket)
    {
        if (ticket == kStopTicket)
        {
            do_stop();
            return;
        }

        // the ticket is sent by external worker thread, and maybe untrustworthy
        // just ignore it if invalid, instead of assert it
        auto iter = coros_waiting_for_extern_wakeup.find(ticket);
        if (iter == coros_waiting_for_extern_wakeup.end())
        {
            std::cerr << "WARNING: no coro waiting for thread finish with ticket " << ticket << "\n";
            return;
        }
        auto coro = iter->second;
        coros_waiting_for_extern_wakeup.erase(iter);
        coro.resume();
    }

    void on_inflight_io_returned(uint64_t index, int res)
    {
        auto iter = inflight_map.find(index);
        if (iter == inflight_map.end()) [[unlikely]]
        {
            std::cerr << "duplicate cqe for index = " << index << "\n";
            // Duplicate CQE: the SQE already completed and the coroutine may
            // have been resumed/destroyed. Skip this CQE and continue
            // processing the remaining ones.
            return;
        }

        auto [coro_addr, res_addr] = iter->second;
        inflight_map.erase(iter);
        if (check_abandoned_and_erase(index))
        {
            // the inflight io is abandoned, scheduler need not resume it
            return;
        }

        // Empty coroutine address means this IO is emitted by
        // scheduler itself, no coroutine need to be continued
        if (!coro_addr)
        {
            return;
        }

        auto handle = std::coroutine_handle<>::from_address(reinterpret_cast<void*>(coro_addr));
        debug_coro("io returned:", handle);
        if (res_addr) [[likely]]
        {
            *res_addr = res;
        }

        handle.resume();
    }

    void batch_prepare_sqe()
    {
        tot_pending_queue_size += pending_queue.size();
        tot_pending_queue_sample_count += 1;

        if (!control_pipe_armed)
        {
            control_pipe_armed = do_prep_control_pipe_sqe();
        }

        while (!pending_queue.empty())
        {
            const auto& item = pending_queue.front();
            if (check_abandoned_and_erase(item.index))
            {
                pending_queue.pop();
                continue;
            }

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

            struct timespec sched_ts {};
            clock_gettime(CLOCK_MONOTONIC, &sched_ts);
            uint64_t cost_ns = (static_cast<uint64_t>(sched_ts.tv_sec) * 1000000000ULL + sched_ts.tv_nsec) -
                               (static_cast<uint64_t>(item.enter_ts.tv_sec) * 1000000000ULL + item.enter_ts.tv_nsec);
            tot_sched_time += cost_ns;
            tot_sched_count++;

            debug_coro("prepare sqe for coro:", item.coro);
            memset(sqe, 0, sizeof(*sqe)); // ensure SQE is clean before prep
            item.prep_sqe_fn(sqe);
            inflight_map.emplace(item.index, InflightItem {
                item.coro.address(), item.res_addr
            });
            io_uring_sqe_set_data64(sqe, item.index);
            pending_queue.pop();
        }
    }

    std::uint64_t append_pending_queue(std::coroutine_handle<> coro, PrepSqeClosure prep_sqe_fn, int* res_addr)
    {
        struct timespec enter_ts {};
        clock_gettime(CLOCK_MONOTONIC, &enter_ts);
        uint64_t index = last_index++;
        pending_queue.emplace(coro, std::move(prep_sqe_fn), res_addr, enter_ts, index);
        return index;
    }

    // return true if the index is canceled
    bool check_abandoned_and_erase(uint64_t index)
    {
        auto iter = abandoned.find(index);
        bool ret = iter != abandoned.end();
        if (ret)
            abandoned.erase(iter);
        return ret;
    }

    bool is_on_ring_thread() const noexcept
    {
        auto owner = ring_owner.load(std::memory_order_acquire);
        return owner == std::thread::id {} || owner == std::this_thread::get_id();
    }

    void on_control_pipe_received()
    {
        uint64_t buf[32];
        std::vector<uint64_t> ready_tickets;
        // io uring cqe for poll multishot is edge-triggered,
        // need to read all available data
        while (true)
        {
            // control_pipe is created with O_NONBLOCK, so read may return EAGAIN
            // when no data available
            ssize_t n = ::read(control_pipe[0], buf, sizeof(buf));
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                break;
            }
            if (n < 0)
            {
                int e = errno;
                std::cerr << "WARNING: read from control_pipe failed: " << strerror(e) << "\n";
                // some coroutine may forever block waiting for the pipe, terminate is better
                std::terminate();
            }
            if (n == 0)
                break;
            assert (n % (ssize_t)sizeof(uint64_t) == 0);
            for (ssize_t i = 0; i < n / (ssize_t)sizeof(uint64_t); i++)
                ready_tickets.push_back(buf[i]);
        }
        for (auto ticket : ready_tickets)
        {
            on_ticket_received(ticket);
        }
    }

    bool do_prep_control_pipe_sqe()
    {
        auto* sqe = io_uring_get_sqe(&ring);
        if (!sqe)
        {
            return false;
        }
        memset(sqe, 0, sizeof(*sqe));
        io_uring_sqe_set_data64(sqe, kUserDataControlPipe);
        io_uring_prep_poll_multishot(sqe, control_pipe[0], POLLIN);
        return true;
    }

    bool init_control_pipe()
    {        
        if (pipe2(control_pipe, O_CLOEXEC | O_NONBLOCK) < 0)
        {
            return false;
        }

        control_pipe_armed = do_prep_control_pipe_sqe();
        if (!control_pipe_armed)
        {
            ::close(control_pipe[0]);
            ::close(control_pipe[1]);
            return false;
        }

        return true;
    }

    // recycle all resouces
    void do_recycle()
    {
        assert (is_on_ring_thread() && "Scheduler::do_recycle() must run on the init()/run() thread");
        io_uring_queue_exit(&ring);
        close(control_pipe[0]);
        close(control_pipe[1]);
        control_pipe[0] = -1;
        control_pipe[1] = -1;
        status = Uninitialized;
    }

    io_uring ring {};
    struct PendingItem {
        std::coroutine_handle<> coro;
        PrepSqeClosure prep_sqe_fn;
        int* res_addr;
        struct timespec enter_ts;
        uint64_t index;
    };

    std::queue<PendingItem> pending_queue;
    uint64_t last_index = 0;
    std::set<uint64_t> abandoned;

    struct InflightItem
    {
        void* coro_address;
        int* result_addr;
    };
    std::unordered_map<uint64_t, InflightItem> inflight_map; // io_idx -> <coro addr, result addr>

    // The control pipe is used to communicate between in-band coroutines
    // and out-of-band workers.
    // In-band coroutines: coroutines spawned by the scheduler and run in
    // single-thread mode.
    // Out-of-band(OOB) workers: they run in thread different from the
    // in-band thread. They can not visit/control the scheduler directly
    // and need to communicate with the scheduler through the control pipe.
    // By sending a uint64 byte to control_pipe[1], an OOB worker can notify
    // the scheduler to resume some coroutine or perform some specific actions.
    int control_pipe[2] {-1, -1};
    bool control_pipe_armed = false;


    // Any in-band coroutine can register itself to wait for external signals.
    // After calling register_coro_waiting_extern, the caller will get a ticket
    // of type uint64, which can be handled to an external thread. If the extern
    // thread need to wakeup the coro, it only need to write this ticket to 
    // control_pipe[1], and the scheduler will resume the coro.
    // The scheduler is NOT responsible for passing return values. Values may be
    // passed by shared objects.
    std::unordered_map<uint64_t, std::coroutine_handle<>> coros_waiting_for_extern_wakeup;
    uint64_t wakeup_tickets = 0;
    
    // special ticket for control purpose
    static constexpr uint64_t kStopTicket = std::numeric_limits<uint64_t>::max() - 1;

    static constexpr __kernel_timespec kIdleTs {0, 1000000};
    // Some magic user data values to distinguish cqe for different purposes
    // They must be large enough to avoid conflict with other user trigger operations,
    // which carries user data values counted from 0
    static constexpr uint64_t kUserDataControlPipe = std::numeric_limits<uint64_t>::max() - 1;

    enum {
        Uninitialized,
        Initialized,
        Running,
        Stopping,
        Stopped
    } status = Uninitialized;

    // Thread that owns the ring, recorded by init(); value-initialized means "nobody yet".
    std::atomic<std::thread::id> ring_owner {};
    uint64_t tot_sched_time = 0;
    uint64_t tot_sched_count = 0;
    uint64_t tot_submit_items = 0;
    uint64_t tot_submit_count = 0;
    uint64_t tot_pending_queue_size = 0;
    uint64_t tot_pending_queue_sample_count = 0;
};


// CAUTION: Users of TimeoutEvent should check the result
// of wait_until: int ret = co_await event.wait_until(...)
// Returning any negative value means the scheduler is not
// working properly, the co_await is resumed immediately,
// The TimeoutEvent has not got a set() or a timeout signal
// before coroutine continues.
class TimeoutEvent
{
public:
    struct Awaiter
    {
        Awaiter(Scheduler* scheduler, struct __kernel_timespec* time_spec, TimeoutEvent* event):
            _scheduler(scheduler), _time_spec(time_spec), _event(event) {}

        bool await_ready() const noexcept {
            // if has pending value, return true to skip suspend coroutine
            // else, return false to suspend coroutine
            return _event->_pending;
        }

        bool await_suspend(std::coroutine_handle<> coro) noexcept {
            assert (!_event->_coro);
            _event->_coro = coro;
            PrepSqeClosure prepare_sqe_cb = [ts = _time_spec](io_uring_sqe* sqe) -> void {
                io_uring_prep_timeout(sqe, ts, 0, 0);
            };
            assert (_event->_cancel_index == std::numeric_limits<uint64_t>::max());
            int ret = _scheduler->schedule(coro, prepare_sqe_cb, nullptr, &_event->_cancel_index);
    
            // nagtive return means scheduler has not schedule anything
            // we can not expect scheduler to wakeup this coroutine in future
            // so just return false to continue coroutine
            if (ret < 0) [[unlikely]]
            {
                _result = ret;
                return false;
            }
            return true;
        }

        int await_resume() const
        {
            _event->_cancel_index = std::numeric_limits<uint64_t>::max();
            _event->_coro = nullptr;
            _event->_pending = false;
            return _result;
        }

        Scheduler* _scheduler;
        struct __kernel_timespec* _time_spec;
        TimeoutEvent* _event;
        int _result = 0;
    };

public:
    explicit TimeoutEvent(Scheduler* scheduler): _scheduler(scheduler) {}
    TimeoutEvent(const TimeoutEvent& other) = delete;
    TimeoutEvent& operator=(const TimeoutEvent&) = delete;
    TimeoutEvent(TimeoutEvent&& other) = delete;
    TimeoutEvent& operator=(TimeoutEvent&& other) = delete;

    template<typename Rep, typename Period>
    Awaiter wait_until(std::chrono::duration<Rep, Period> duration) {
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(duration);
        auto nsecs = std::chrono::duration_cast<std::chrono::nanoseconds>(duration - secs);
        _timespec.tv_sec = secs.count();
        _timespec.tv_nsec = nsecs.count();
        return {_scheduler, &_timespec, this};
    }

    // CONTRACT: resumes the waiter inline, exactly like Event::set(), so this call
    // must come from the thread running the scheduler -- abandon() below asserts it,
    // and the timeout SQE it cancels belongs to the SINGLE_ISSUER ring.
    void set()
    {
        if (_coro)
        {
            _scheduler->abandon(_cancel_index);
            _coro.resume();
        }
        else
        {
            _pending = true;
        }
    }

private:
    std::coroutine_handle<> _coro;
    bool _pending = false;
    uint64_t _cancel_index = std::numeric_limits<uint64_t>::max();
    Scheduler* _scheduler;
    struct __kernel_timespec _timespec {};
};

struct UringAwaiter
{
public:
    explicit UringAwaiter(Scheduler* scheduler, PrepSqeClosure prepare_sqe_fn)
        : scheduler(scheduler), _prepare_sqe_fn(prepare_sqe_fn) {}
    
    UringAwaiter(const UringAwaiter&) = delete;
    UringAwaiter& operator=(const UringAwaiter&) = delete;
    UringAwaiter(UringAwaiter&&) = delete;
    UringAwaiter& operator=(UringAwaiter&&) = delete;

    // Non-virtual on purpose: an awaiter is only ever built as the co_await temporary
    // inside the awaiting coroutine's own scope and destroyed there after await_resume(),
    // so nothing deletes it through a UringAwaiter*. (Making this protected/private is not
    // an option either -- it would break every free function that returns an awaiter by
    // value, since the caller has to destroy the temporary it materializes.)
    ~UringAwaiter()
    {
        debug_coro("awaiter destroyed by coro:", _coro);
    }

    bool await_ready() const noexcept
    {
        return false;
    }
    bool await_suspend(std::coroutine_handle<> coro) noexcept
    {
        _coro = coro;
        int ret = scheduler->schedule(coro, _prepare_sqe_fn, &result);
        if (ret < 0)
        {
            result = ret;
            return false;
        }
        return true;
    }

    int await_resume() const noexcept
    {
        debug_coro("await_resume for coro:", _coro);
        return result;
    }

private:
    Scheduler* scheduler = nullptr;
    PrepSqeClosure _prepare_sqe_fn;
    std::coroutine_handle<> _coro{};
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

inline UringAwaiter sync_file_range(Scheduler* scheduler, int fd, uint64_t offset, unsigned len, int flags)
{
    PrepSqeClosure prepare_sqe_cb = [fd, offset, len, flags](io_uring_sqe* sqe) -> void {
        io_uring_prep_sync_file_range(sqe, fd, len, offset, flags);
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

inline UringAwaiter renameat(Scheduler* scheduler, int olddirfd, const char* oldpath, int newdirfd, const char* newpath, unsigned int flags = 0)
{
    PrepSqeClosure prepare_sqe_cb = [olddirfd, oldpath, newdirfd, newpath, flags](io_uring_sqe* sqe) -> void {
        io_uring_prep_renameat(sqe, olddirfd, oldpath, newdirfd, newpath, flags);
    };
    return UringAwaiter{ scheduler, prepare_sqe_cb };
}

inline UringAwaiter unlinkat(Scheduler* scheduler, int dirfd, const char* pathname, int flags = 0)
{
    PrepSqeClosure prepare_sqe_cb = [dirfd, pathname, flags](io_uring_sqe* sqe) -> void {
        io_uring_prep_unlinkat(sqe, dirfd, pathname, flags);
    };
    return UringAwaiter{ scheduler, prepare_sqe_cb };
}

// Requires: kernel >= 5.15 (IORING_OP_MKDIRAT)
inline UringAwaiter mkdirat(Scheduler* scheduler, int dirfd, const char* pathname, mode_t mode = 0755)
{
    PrepSqeClosure prepare_sqe_cb = [dirfd, pathname, mode](io_uring_sqe* sqe) -> void {
        io_uring_prep_mkdirat(sqe, dirfd, pathname, mode);
    };
    return UringAwaiter{ scheduler, prepare_sqe_cb };
}

// Requires: kernel >= 5.6 (IORING_OP_STATX).
// Both path and statxbuf are captured by pointer, not copied, so -- like the path
// handed to openat/unlinkat -- they must stay alive until the completion arrives:
// keep them in the awaiting coroutine's own frame, not in a temporary std::string.
inline UringAwaiter statx(Scheduler* scheduler, int dfd, const char* path, struct statx* statxbuf,
                          int flags = 0, unsigned mask = 0x7ff /* STATX_BASIC_STATS */)
{
    PrepSqeClosure prepare_sqe_cb = [dfd, path, flags, mask, statxbuf](io_uring_sqe* sqe) -> void {
        io_uring_prep_statx(sqe, dfd, path, flags, mask, statxbuf);
    };
    return UringAwaiter{ scheduler, prepare_sqe_cb };
}

template <typename Rep, typename Period>
class SleepAwaiter : public UringAwaiter
{
    // The prepare-SQE closure below captures `this`, and the SQE carries &_timespec, so this
    // awaiter must stay exactly where the co_await expression built it (in the coroutine
    // frame) until the operation completes. UringAwaiter's deleted copy/move ctors are what
    // guarantees that; if they ever get relaxed, the kernel would read the timespec out of a
    // stale object, so make that a compile error instead of silent UB.
    static_assert(!std::is_copy_constructible_v<UringAwaiter> && !std::is_move_constructible_v<UringAwaiter>,
                  "UringAwaiter must stay non-copyable and non-movable: SleepAwaiter hands the kernel a pointer into itself");

public:
    SleepAwaiter(Scheduler* scheduler, std::chrono::duration<Rep, Period> duration, unsigned int count, unsigned int flags)
    : UringAwaiter(scheduler, [this](io_uring_sqe* sqe) { prepare_sqe(sqe);})
    , _count(count), _flags(flags)
    {
        // A negative duration would yield a timespec the kernel rejects with -EINVAL.
        // So "sleeping into the past" just expires immediately.
        if (duration < std::chrono::duration<Rep, Period>::zero())
            duration = std::chrono::duration<Rep, Period>::zero();
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(duration);
        auto nsecs = std::chrono::duration_cast<std::chrono::nanoseconds>(duration - secs);
        _timespec.tv_sec = secs.count();
        _timespec.tv_nsec = nsecs.count();
    }

private:
    void prepare_sqe(io_uring_sqe* sqe)
    {
        io_uring_prep_timeout(sqe, &_timespec, _count, _flags);
    }
    unsigned int _count = 0;
    unsigned int _flags = 0;
    // preserve timespec data in awaiter's lifecycle, to fulfill old kernel's requirement
    struct __kernel_timespec _timespec {};
};

template <typename Rep, typename Period>
inline SleepAwaiter<Rep, Period> sleep(Scheduler* scheduler, std::chrono::duration<Rep, Period> duration)
{
    return SleepAwaiter<Rep, Period>(scheduler, duration, 0, 0);
}

template <typename T>
class ThreadAwaiter
{
    static_assert(std::is_default_constructible_v<T>, "T must be default constructible");
    static_assert(std::is_move_constructible_v<T>, "T must be move constructible");
    static_assert(std::is_move_assignable_v<T>, "T must be move assignable");

public:
    explicit ThreadAwaiter(Scheduler* scheduler, std::function<T()> func)
    : _scheduler(scheduler), _func(std::move(func)) {}

    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> coro) noexcept
    {
        std::uint64_t ticket = 0;
        ret = _scheduler->register_coro_waiting_extern(coro, &ticket);
        if (ret < 0)
        {
            return false;
        }
        _thread = std::thread([this, ticket] {
            result = _func();
            _scheduler->wakeup_coro_by_ticket(ticket);
        });
        return true;
    }

    std::tuple<int, T> await_resume() noexcept
    {
        if (_thread.joinable())
            _thread.join();
        return {ret, std::move(result)};
    }

private:
    Scheduler* _scheduler;
    T result {};
    int ret = 0;
    std::thread _thread {};
    std::function<T()> _func;
};

template <typename T, typename F>
inline ThreadAwaiter<T> run_in_thread(Scheduler* s, F&& f)
{
    return ThreadAwaiter<T>(s, std::function<T()>(std::forward<F>(f)));
}


} // namespace yyasio
