#define BOOST_TEST_MODULE YyasioTest
#include <boost/test/unit_test.hpp>

#include "yyasio.h"
#include <array>
#include <atomic>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

using namespace yyasio;

// Drives one scheduler the way every IO test needs it. init(), run() and shutdown()
// all have to happen on the loop thread (the ring is set up SINGLE_ISSUER), which is
// why the release lives here instead of after runner.join() in the test body.
static void run_scheduler(Scheduler& scheduler)
{
    BOOST_REQUIRE_EQUAL(scheduler.init(16), YYASIO_OK);
    scheduler.run();
    scheduler.shutdown();
}

BOOST_AUTO_TEST_SUITE(YYasioTests)

// Helper coroutine that waits on a event and stores the result
Task<void> wait_for_event(Event<int>& event, std::atomic<int>& result)
{
    std::cout << "Coroutine: about to wait for event\n";
    int val = co_await event.wait();
    std::cout << "Coroutine: got value " << val << "\n";
    result.store(val);
    std::cout << "Coroutine: stored value, about to return\n";
}

BOOST_AUTO_TEST_CASE(test_event_basic)
{
    Event<int> event;
    std::atomic<int> result{-1};

    std::cout << "Test: starting coroutine\n";
    // Start the coroutine - it will suspend on co_await event.wait()
    wait_for_event(event, result).detach();

    std::cout << "Test: coroutine started, result = " << result.load() << "\n";
    // At this point, the coroutine is suspended, waiting for the event
    BOOST_CHECK_EQUAL(result.load(), -1);

    std::cout << "Test: about to resume event\n";
    // Resume the coroutine by fulfilling the event
    event.set(42);

    std::cout << "Test: event resumed, result = " << result.load() << "\n";
    // The coroutine should have resumed and stored the value
    BOOST_CHECK_EQUAL(result.load(), 42);
}

// Helper coroutine for testing multiple resumes
Task<void> wait_for_event_loop(Event<int>& event, std::vector<int>& results, int count)
{
    for (int i = 0; i < count; ++i)
    {
        int val = co_await event.wait();
        results.push_back(val);
    }
}

BOOST_AUTO_TEST_CASE(test_event_multiple_resumes)
{
    Event<int> event;
    std::vector<int> results;

    // Start the coroutine - it will wait in a loop
    wait_for_event_loop(event, results, 3).detach();

    // Resume multiple times
    event.set(1);
    BOOST_CHECK_EQUAL(results.size(), 1);
    BOOST_CHECK_EQUAL(results[0], 1);

    event.set(2);
    BOOST_CHECK_EQUAL(results.size(), 2);
    BOOST_CHECK_EQUAL(results[1], 2);

    event.set(3);
    BOOST_CHECK_EQUAL(results.size(), 3);
    BOOST_CHECK_EQUAL(results[2], 3);
}

// 复现问题：协程1 set 某个 Event 时，协程2 还没有 co_await 在这个 Event 上
// （它正 co_await 在另一个 Event 上），这个值就被直接丢弃了；
// 协程2 随后 co_await 这个 Event 时会永久挂起。
// 期望语义：set 的值应当被记住，后到的 waiter 立刻拿到它，事件不丢。
Task<void> waiter_on_other_event_then_target(Event<int>& gate, Event<int>& target,
                                             std::atomic<int>& result)
{
    // 先挂在另一个 Event 上，此刻 target 上一个 waiter 都没有
    co_await gate.wait();
    // 被唤醒之后才开始等 target，而 target 早就被 set 过了
    int val = co_await target.wait();
    result.store(val);
}

BOOST_AUTO_TEST_CASE(test_event_set_before_waiter_not_lost)
{
    Event<int> gate;
    Event<int> target;
    std::atomic<int> result{-1};

    // detach() 会同步执行到第一个 co_await 才挂起，所以返回时 gate 已经登记了 waiter。
    // 整个用例不使用 scheduler/线程，时序完全确定，可稳定复现。
    waiter_on_other_event_then_target(gate, target, result).detach();
    BOOST_REQUIRE_EQUAL(result.load(), -1);

    // 协程2 还阻塞在 gate 上，target 没有任何 waiter，此时 set 不能被丢弃
    target.set(42);
    BOOST_REQUIRE_EQUAL(result.load(), -1); // 还没轮到协程2 运行

    // 唤醒协程2，它接着才 co_await target
    gate.set(0);

    // 如果事件没有丢，协程2 应当立刻拿到 42 并跑完
    BOOST_CHECK_EQUAL(result.load(), 42);

    /* 清理：若事件被丢弃（当前实现），协程2 会永久挂起在 target 上，
     * 协程帧不析构，ASan 会报 leak。这里再 set 一次让它跑完并自毁帧。
     */
    target.set(42);
}

BOOST_AUTO_TEST_CASE(placeholder)
{
    // TODO: add more test cases
    BOOST_CHECK(true);
}

// ==================== File IO Tests ====================

static const char* TEST_FILE_PATH = "/tmp/yyasio_test_file.txt";
static const char* TEST_CONTENT = "Hello, yyasio!";

// Test openat: open a file and return the fd
Task<void> open_file_coro(Scheduler* scheduler, std::atomic<int>& result)
{
    int fd = co_await openat(scheduler, AT_FDCWD, TEST_FILE_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    std::cout << "test_open_file: fd = " << fd << "\n";
    result.store(fd);
}

BOOST_AUTO_TEST_CASE(test_openat)
{
    // Clean up any existing file
    unlink(TEST_FILE_PATH);

    Scheduler scheduler;

    std::atomic<int> fd{-1};
    open_file_coro(&scheduler, fd).detach();

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (fd.load() == -1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    BOOST_CHECK(fd.load() > 0);
    if (fd.load() > 0) {
        ::close(fd.load());
    }
    unlink(TEST_FILE_PATH);
    scheduler.stop();
    runner.join();
}

// Test write: write content to a file
Task<void> write_file_coro(Scheduler* scheduler, int fd, std::atomic<int>& bytes_written)
{
    int ret = co_await write(scheduler, fd, TEST_CONTENT, strlen(TEST_CONTENT));
    std::cout << "test_write_file: wrote " << ret << " bytes\n";
    bytes_written.store(ret);
}

BOOST_AUTO_TEST_CASE(test_write)
{
    // Create a file first
    int fd = ::open(TEST_FILE_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    BOOST_REQUIRE(fd > 0);

    Scheduler scheduler;

    std::atomic<int> bytes_written{0};
    write_file_coro(&scheduler, fd, bytes_written).detach();

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (bytes_written.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    BOOST_CHECK_EQUAL(bytes_written.load(), (int)strlen(TEST_CONTENT));
    ::close(fd);
    unlink(TEST_FILE_PATH);
    scheduler.stop();
    runner.join();
}

// Test read: read content from a file
Task<void> read_file_coro(Scheduler* scheduler, int fd, char* buffer, size_t buf_size, std::atomic<int>& bytes_read)
{
    int ret = co_await read(scheduler, fd, buffer, buf_size);
    std::cout << "test_read_file: read " << ret << " bytes\n";
    bytes_read.store(ret);
}

BOOST_AUTO_TEST_CASE(test_read)
{
    // Create a file with content
    int fd = ::open(TEST_FILE_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    BOOST_REQUIRE(fd > 0);
    ::write(fd, TEST_CONTENT, strlen(TEST_CONTENT));
    ::lseek(fd, 0, SEEK_SET);  // Reset to beginning

    Scheduler scheduler;

    char buffer[256] = {0};
    std::atomic<int> bytes_read{0};
    read_file_coro(&scheduler, fd, buffer, sizeof(buffer), bytes_read).detach();

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (bytes_read.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    BOOST_CHECK_EQUAL(bytes_read.load(), (int)strlen(TEST_CONTENT));
    BOOST_CHECK_EQUAL(std::string(buffer), std::string(TEST_CONTENT));
    ::close(fd);
    unlink(TEST_FILE_PATH);
    scheduler.stop();
    runner.join();
}

// Test close: close a file descriptor
Task<void> close_file_coro(Scheduler* scheduler, int fd, std::atomic<int>& result)
{
    int ret = co_await close(scheduler, fd);
    std::cout << "test_close_file: ret = " << ret << "\n";
    result.store(ret);
}

BOOST_AUTO_TEST_CASE(test_close)
{
    // Create a file
    int fd = ::open(TEST_FILE_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    BOOST_REQUIRE(fd > 0);

    Scheduler scheduler;

    std::atomic<int> close_result{-1};
    close_file_coro(&scheduler, fd, close_result).detach();

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (close_result.load() == -1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // close() returns 0 on success
    BOOST_CHECK_EQUAL(close_result.load(), 0);
    unlink(TEST_FILE_PATH);
    scheduler.stop();
    runner.join();
}

// Combined test: open, write, read, close in sequence
Task<void> file_io_combined_coro(Scheduler* scheduler, std::atomic<bool>& done)
{
    // Open file
    int fd = co_await openat(scheduler, AT_FDCWD, TEST_FILE_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    std::cout << "combined: opened fd = " << fd << "\n";
    BOOST_REQUIRE(fd > 0);

    // Write
    int written = co_await write(scheduler, fd, TEST_CONTENT, strlen(TEST_CONTENT));
    std::cout << "combined: wrote " << written << " bytes\n";
    BOOST_CHECK_EQUAL(written, (int)strlen(TEST_CONTENT));

    // Seek back to beginning (using read with offset=0)
    char buffer[256] = {0};
    int read_bytes = co_await read(scheduler, fd, buffer, sizeof(buffer), 0);
    std::cout << "combined: read " << read_bytes << " bytes: " << buffer << "\n";
    BOOST_CHECK_EQUAL(read_bytes, (int)strlen(TEST_CONTENT));
    BOOST_CHECK_EQUAL(std::string(buffer), std::string(TEST_CONTENT));

    // Close
    int close_ret = co_await close(scheduler, fd);
    std::cout << "combined: closed, ret = " << close_ret << "\n";
    BOOST_CHECK_EQUAL(close_ret, 0);

    done.store(true);
}

BOOST_AUTO_TEST_CASE(test_file_io_combined)
{
    unlink(TEST_FILE_PATH);

    Scheduler scheduler;

    std::atomic<bool> done{false};
    file_io_combined_coro(&scheduler, done).detach();

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    BOOST_CHECK(done.load());
    unlink(TEST_FILE_PATH);
    scheduler.stop();
    runner.join();
}

// Magic number returned by the child coroutine
static constexpr int MAGIC_NUMBER = 0x5A686F75;

// Child coroutine: sleep for a short time, then return a magic number
Task<int> return_after_sleep(Scheduler* scheduler)
{
    struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 }; // 100ms
    co_await timeout(scheduler, &ts);
    std::cout << "Child: returning magic number " << std::hex << MAGIC_NUMBER << std::dec << "\n";
    co_return MAGIC_NUMBER;
}

// Parent coroutine: co_await the child and store the result
Task<void> get_return_value(Scheduler* scheduler, std::atomic<int>& result)
{
    std::cout << "Parent: awaiting child coroutine\n";
    int val = co_await return_after_sleep(scheduler);
    std::cout << "Parent: got value " << std::hex << val << std::dec << "\n";
    result.store(val);
}

BOOST_AUTO_TEST_CASE(test_coroutine_return_value)
{
    Scheduler scheduler;

    std::atomic<int> result{0};

    // Start the parent coroutine - it will co_await the child
    get_return_value(&scheduler, result).detach();

    // Run scheduler in background thread (run() is blocking)
    // init() and run() must be on the same thread (IORING_SETUP_SINGLE_ISSUER)
    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    // Wait for result with timeout
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (result.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    BOOST_CHECK_EQUAL(result.load(), MAGIC_NUMBER);
    scheduler.stop();
    runner.join();
}

// ==================== Timeout Test ====================

Task<void> timeout_coro(Scheduler* scheduler, long nsec, std::atomic<int>& result, std::atomic<long>& elapsed_ns)
{
    struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = nsec };
    auto start = std::chrono::steady_clock::now();
    int ret = co_await timeout(scheduler, &ts);
    auto end = std::chrono::steady_clock::now();
    elapsed_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    std::cout << "timeout_coro: ret = " << ret << ", elapsed = " << elapsed_ns.load() << " ns\n";
    result.store(ret);
}

BOOST_AUTO_TEST_CASE(test_timeout)
{
    Scheduler scheduler;

    std::atomic<int> result{-1};
    std::atomic<long> elapsed_ns{0};
    long timeout_ns = 100'000'000; // 100ms

    timeout_coro(&scheduler, timeout_ns, result, elapsed_ns).detach();

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (result.load() == -1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // io_uring timeout returns -ETIME (-62) when timer expires
    BOOST_CHECK_EQUAL(result.load(), -ETIME);
    // Elapsed time should be at least 80% of the requested timeout
    BOOST_CHECK_GE(elapsed_ns.load(), timeout_ns * 8 / 10);
    scheduler.stop();
    runner.join();
}

// ==================== Sleep Test ====================

// sleep() is the self-owning-timespec flavour of timeout(): the timer CQE still carries
// -ETIME on a full sleep (same convention test_timeout asserts on), never 0.
static constexpr long SLEEP_UNIT_NS = 1000000L; // 1ms, for the 80%-of-requested assertions

Task<void> sleep_once(Scheduler* scheduler, long msecs, std::atomic<int>& result, std::atomic<long>& elapsed_ns)
{
    auto start = std::chrono::steady_clock::now();
    int ret = co_await yyasio::sleep(scheduler, std::chrono::milliseconds(msecs));
    auto end = std::chrono::steady_clock::now();
    elapsed_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    std::cout << "sleep_once(" << msecs << "ms): ret = " << ret
              << ", elapsed = " << elapsed_ns.load() << " ns\n";
    result.store(ret);
}

BOOST_AUTO_TEST_CASE(test_sleep_expiry)
{
    Scheduler scheduler;

    std::atomic<int> result{0};
    std::atomic<long> elapsed_ns{0};
    long want_ns = 200 * SLEEP_UNIT_NS;

    sleep_once(&scheduler, 200, result, elapsed_ns).detach();

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (result.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    BOOST_CHECK_EQUAL(result.load(), -ETIME);
    BOOST_CHECK_GE(elapsed_ns.load(), want_ns * 8 / 10);
    // Upper bound too: each SleepAwaiter owns its own timespec, so no timer may borrow
    // another one's duration (a shared timespec lands here).
    BOOST_CHECK_LE(elapsed_ns.load(), want_ns + 80 * SLEEP_UNIT_NS);
    scheduler.stop();
    runner.join();
}

// A sub-second duration must not collapse to "no wait": the seconds/nanoseconds split in
// SleepAwaiter is the whole point of accepting a std::chrono::duration.
BOOST_AUTO_TEST_CASE(test_sleep_sub_second_not_truncated)
{
    Scheduler scheduler;

    std::atomic<int> result{0};
    std::atomic<long> elapsed_ns{0};
    long want_ns = 50 * SLEEP_UNIT_NS;

    sleep_once(&scheduler, 50, result, elapsed_ns).detach();

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (result.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    BOOST_CHECK_EQUAL(result.load(), -ETIME);
    BOOST_CHECK_GE(elapsed_ns.load(), want_ns * 8 / 10);
    BOOST_CHECK_LE(elapsed_ns.load(), want_ns + 80 * SLEEP_UNIT_NS);
    scheduler.stop();
    runner.join();
}

// A negative duration is clamped to zero instead of being handed to the kernel as an
// invalid timespec: the waiter must still see the normal -ETIME, completing immediately
// (without the clamp it comes back as -EINVAL).
BOOST_AUTO_TEST_CASE(test_sleep_negative_clamped)
{
    Scheduler scheduler;

    std::atomic<int> result{0};
    std::atomic<long> elapsed_ns{0};

    sleep_once(&scheduler, -50, result, elapsed_ns).detach();

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (result.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    BOOST_CHECK_EQUAL(result.load(), -ETIME);
    BOOST_CHECK_LE(elapsed_ns.load(), 30 * SLEEP_UNIT_NS);
    scheduler.stop();
    runner.join();
}

struct SleepRecord
{
    std::atomic<int> ret{0};
    std::atomic<long> elapsed_ns{0};
};

Task<void> sleep_recorded(Scheduler* scheduler, long msecs, SleepRecord* rec, std::atomic<int>& done_count)
{
    auto start = std::chrono::steady_clock::now();
    int ret = co_await yyasio::sleep(scheduler, std::chrono::milliseconds(msecs));
    auto end = std::chrono::steady_clock::now();
    rec->ret.store(ret);
    rec->elapsed_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    std::cout << "sleep_recorded(" << msecs << "ms): ret = " << ret
              << ", elapsed = " << rec->elapsed_ns.load() << " ns\n";
    done_count.fetch_add(1);
}

// An IO submitted next to the timers must not be stuck behind them.
Task<void> open_close_recorded(Scheduler* scheduler, SleepRecord* rec, std::atomic<int>& done_count)
{
    auto start = std::chrono::steady_clock::now();
    int fd = co_await yyasio::openat(scheduler, AT_FDCWD, "/dev/null", O_RDONLY, 0);
    rec->ret.store(fd);
    if (fd >= 0)
    {
        co_await yyasio::close(scheduler, fd);
    }
    auto end = std::chrono::steady_clock::now();
    rec->elapsed_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    std::cout << "open_close_recorded: fd = " << fd << "\n";
    done_count.fetch_add(1);
}

// Three live SleepAwaiters at once: each carries its own timespec inside its own awaiter,
// so every timer must honour its own duration, and the wall clock must not add up.
BOOST_AUTO_TEST_CASE(test_sleep_concurrent_with_io)
{
    Scheduler scheduler;

    std::array<SleepRecord, 4> recs;
    std::atomic<int> done_count{0};
    const long msecs[3] = {50, 120, 190};

    auto wall_start = std::chrono::steady_clock::now();
    for (int i = 0; i < 3; i++)
    {
        sleep_recorded(&scheduler, msecs[i], &recs[i], done_count).detach();
    }
    open_close_recorded(&scheduler, &recs[3], done_count).detach();

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (done_count.load() < 4 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    long long wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - wall_start).count();

    BOOST_CHECK_EQUAL(done_count.load(), 4);
    for (int i = 0; i < 3; i++)
    {
        BOOST_CHECK_EQUAL(recs[i].ret.load(), -ETIME);
        BOOST_CHECK_GE(recs[i].elapsed_ns.load(), msecs[i] * SLEEP_UNIT_NS * 8 / 10);
        BOOST_CHECK_LE(recs[i].elapsed_ns.load(), (msecs[i] + 80) * SLEEP_UNIT_NS);
    }
    BOOST_CHECK_GE(recs[3].ret.load(), 0);   // the openat fd, i.e. IO did go through
    // Concurrent timers finish with the longest one (~190ms), not after their sum (~360ms).
    BOOST_CHECK_LT(wall_ns, 310 * SLEEP_UNIT_NS);
    scheduler.stop();
    runner.join();
}

// ==================== CoroId Test ====================

Task<uint64_t> get_sub_coro_id_test(Scheduler* scheduler)
{
    uint64_t id = co_await current_coro_id();
    co_return id;
}

Task<void> coro_id_test_oro(std::atomic<bool>& done,
    std::array<uint64_t, 2>& parent_ids,
    std::array<uint64_t, 2>& sub_ids)
{
    // Parent's coro_id should be the same on every call
    parent_ids[0] = co_await current_coro_id();
    parent_ids[1] = co_await current_coro_id();

    // Each sub-coroutine should have a different coro_id
    sub_ids[0] = co_await get_sub_coro_id_test(nullptr);
    sub_ids[1] = co_await get_sub_coro_id_test(nullptr);

    done.store(true);
}

BOOST_AUTO_TEST_CASE(test_coro_id)
{
    // current_coro_id() does not use io_uring, so no scheduler is needed.
    // The coroutine runs synchronously via detach() (all await_suspend return false).
    std::atomic<bool> done{false};
    std::array<uint64_t, 2> parent_ids{0, 0};
    std::array<uint64_t, 2> sub_ids{0, 0};

    coro_id_test_oro(done, parent_ids, sub_ids).detach();

    BOOST_CHECK(done.load());
    // Parent's coro_id should be consistent across multiple calls
    BOOST_CHECK_EQUAL(parent_ids[0], parent_ids[1]);
    // Different sub-coroutines should have different coro_ids
    BOOST_CHECK_NE(sub_ids[0], sub_ids[1]);
    // Sub-coroutine's coro_id should differ from parent's
    BOOST_CHECK_NE(parent_ids[0], sub_ids[0]);
    BOOST_CHECK_NE(parent_ids[1], sub_ids[1]);
}

// ==================== IsFinished Test ====================

// Helper coroutines for is_finished tests
Task<void> wait_event_then_done(Event<int>& event, std::atomic<bool>& done)
{
    co_await event.wait();
    done.store(true);
}

Task<int> sync_return_value() { co_return 42; }

// Await a child Task and report its is_finished() from within the parent coroutine
Task<void> check_child_is_finished(std::atomic<bool>& child_finished_in_parent, std::atomic<bool>& parent_done)
{
    auto child = sync_return_value();
    BOOST_CHECK(!child.is_finished()); // not started yet
    int val = co_await child;
    BOOST_CHECK_EQUAL(val, 42);
    // Child frame was destroyed in await_resume, but our Task object is still alive
    child_finished_in_parent.store(child.is_finished());
    parent_done.store(true);
}

BOOST_AUTO_TEST_CASE(test_is_finished_sync_detach)
{
    // Coroutine runs to completion synchronously inside detach(),
    // frame (and ~promise_type) already executed before detach() returns.
    auto task = sync_return_value();
    BOOST_CHECK(!task.is_finished());

    task.detach();

    BOOST_CHECK(task.is_finished());
}

BOOST_AUTO_TEST_CASE(test_is_finished_async_detach)
{
    // is_finished() must stay false while the coroutine is suspended,
    // and flip to true once it runs to completion.
    Event<int> event;
    std::atomic<bool> done{false};

    auto task = wait_event_then_done(event, done);
    BOOST_CHECK(!task.is_finished()); // created but not started

    task.detach(); // resumes until co_await event.wait()
    BOOST_CHECK(!task.is_finished()); // suspended, frame still alive
    BOOST_CHECK(!done.load());

    event.set(1); // resumes and finishes the coroutine synchronously
    BOOST_CHECK(done.load());
    BOOST_CHECK(task.is_finished());
}

BOOST_AUTO_TEST_CASE(test_is_finished_after_move)
{
    // The move transfers the done_guard to the new Task, so completion is only
    // observable through the moved-to object; the moved-from one has no guard.
    Event<int> event;
    std::atomic<bool> done{false};

    auto task = wait_event_then_done(event, done);
    auto moved = std::move(task);

    // moved-from has no guard -> always false
    BOOST_CHECK(!task.is_finished());
    BOOST_CHECK(!moved.is_finished());

    moved.detach();
    event.set(1);

    BOOST_CHECK(moved.is_finished());
    BOOST_CHECK(!task.is_finished()); // guard was moved away
}

BOOST_AUTO_TEST_CASE(test_is_finished_with_co_await)
{
    // After `co_await child`, the child's frame is destroyed in await_resume,
    // so a named Task object can observe is_finished() == true from the parent.
    std::atomic<bool> child_finished_in_parent{false};
    std::atomic<bool> parent_done{false};

    check_child_is_finished(child_finished_in_parent, parent_done).detach();

    BOOST_CHECK(parent_done.load());
    BOOST_CHECK(child_finished_in_parent.load());
}

// ==================== Connect Test ====================

// Server coroutine: accept one connection and read data
Task<void> accept_one_connection(Scheduler* scheduler, int listen_fd, std::atomic<bool>& accepted, char* buf, size_t buf_size, std::atomic<int>& bytes_read)
{
    struct sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    int conn_fd = co_await accept(scheduler, listen_fd, reinterpret_cast<sockaddr*>(&addr), &len);
    std::cout << "accept_one_connection: accepted conn_fd = " << conn_fd << "\n";
    accepted.store(true);

    int ret = co_await read(scheduler, conn_fd, buf, buf_size);
    std::cout << "accept_one_connection: read " << ret << " bytes\n";
    bytes_read.store(ret);
    ::close(conn_fd);
}

// Client coroutine: connect to server and send data
Task<void> connect_and_send_test(Scheduler* scheduler, int port, std::atomic<int>& connect_result, std::atomic<int>& write_result)
{
    // Wait for server to be ready
    struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 100'000'000 }; // 100ms
    co_await timeout(scheduler, &ts);

    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
    {
        std::cerr << "connect_and_send_test: socket() failed\n";
        connect_result.store(-1);
        co_return;
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    std::cout << "connect_and_send_test: connecting to 127.0.0.1:" << port << "\n";
    int ret = co_await connect(scheduler, fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    std::cout << "connect_and_send_test: connect ret = " << ret << "\n";
    connect_result.store(ret);

    if (ret < 0)
    {
        ::close(fd);
        co_return;
    }

    const char* msg = "hello connect test";
    int written = co_await write(scheduler, fd, msg, strlen(msg));
    std::cout << "connect_and_send_test: wrote " << written << " bytes\n";
    write_result.store(written);
    ::close(fd);
}

BOOST_AUTO_TEST_CASE(test_connect)
{
    // Create listening socket
    int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    BOOST_REQUIRE(listen_fd > 0);

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(18080); // Use a different port to avoid conflict

    BOOST_REQUIRE_EQUAL(bind(listen_fd, (sockaddr*)&server_addr, sizeof(server_addr)), 0);
    BOOST_REQUIRE_EQUAL(listen(listen_fd, SOMAXCONN), 0);

    Scheduler scheduler;

    std::atomic<bool> accepted{false};
    std::atomic<int> bytes_read{0};
    char read_buf[256] = {0};

    std::atomic<int> connect_result{-1};
    std::atomic<int> write_result{0};

    // Start server coroutine
    accept_one_connection(&scheduler, listen_fd, accepted, read_buf, sizeof(read_buf), bytes_read).detach();

    // Start client coroutine
    connect_and_send_test(&scheduler, 18080, connect_result, write_result).detach();

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    // Wait for both to complete (including read operation)
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((!accepted.load() || connect_result.load() == -1 || bytes_read.load() == 0) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Verify connection succeeded
    BOOST_CHECK(accepted.load());
    BOOST_CHECK_EQUAL(connect_result.load(), 0);
    BOOST_CHECK_EQUAL(write_result.load(), (int)strlen("hello connect test"));
    BOOST_CHECK_EQUAL(bytes_read.load(), (int)strlen("hello connect test"));
    BOOST_CHECK_EQUAL(std::string(read_buf, bytes_read.load()), "hello connect test");

    ::close(listen_fd);
    scheduler.stop();
    runner.join();
}

// ==================== Listen Test ====================

// Helper coroutine: async listen and verify
Task<void> listen_coro(Scheduler* scheduler, int fd, std::atomic<int>& result)
{
    int ret = co_await listen(scheduler, fd);
    std::cout << "listen_coro: ret = " << ret << "\n";
    result.store(ret);
}

BOOST_AUTO_TEST_CASE(test_listen)
{
    // Create and bind socket first
    int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    BOOST_REQUIRE(listen_fd > 0);

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(18081);

    BOOST_REQUIRE_EQUAL(bind(listen_fd, (sockaddr*)&server_addr, sizeof(server_addr)), 0);

    Scheduler scheduler;

    std::atomic<int> listen_result{-1};
    listen_coro(&scheduler, listen_fd, listen_result).detach();

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (listen_result.load() == -1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // listen() returns 0 on success
    BOOST_CHECK_EQUAL(listen_result.load(), 0);

    // Verify we can connect to it
    if (listen_result.load() == 0)
    {
        int client_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        BOOST_REQUIRE(client_fd > 0);

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(18081);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        int ret = ::connect(client_fd, (struct sockaddr*)&addr, sizeof(addr));
        // Non-blocking connect returns -1 with EINPROGRESS or 0 on success
        BOOST_CHECK(ret == 0 || (ret == -1 && errno == EINPROGRESS));

        ::close(client_fd);
    }
    ::close(listen_fd);
    scheduler.stop();
    runner.join();
}

// ==================== Renameat Test ====================

static const char* TEST_OLD_FILE_PATH = "/tmp/yyasio_test_old.txt";
static const char* TEST_NEW_FILE_PATH = "/tmp/yyasio_test_new.txt";

// Helper coroutine: async renameat and verify
Task<void> renameat_coro(Scheduler* scheduler, int olddirfd, const char* oldpath, int newdirfd, const char* newpath, std::atomic<int>& result)
{
    int ret = co_await renameat(scheduler, olddirfd, oldpath, newdirfd, newpath);
    std::cout << "renameat_coro: ret = " << ret << "\n";
    result.store(ret);
}

BOOST_AUTO_TEST_CASE(test_renameat)
{
    // Create the old file first
    int fd = ::open(TEST_OLD_FILE_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    BOOST_REQUIRE(fd > 0);
    ::write(fd, TEST_CONTENT, strlen(TEST_CONTENT));
    ::close(fd);

    // Clean up new file if exists
    unlink(TEST_NEW_FILE_PATH);

    Scheduler scheduler;

    std::atomic<int> rename_result{-1};
    renameat_coro(&scheduler, AT_FDCWD, TEST_OLD_FILE_PATH, AT_FDCWD, TEST_NEW_FILE_PATH, rename_result).detach();

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (rename_result.load() == -1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // renameat() returns 0 on success
    BOOST_CHECK_EQUAL(rename_result.load(), 0);

    // Verify old file no longer exists
    BOOST_CHECK(access(TEST_OLD_FILE_PATH, F_OK) != 0);

    // Verify new file exists and has correct content
    fd = ::open(TEST_NEW_FILE_PATH, O_RDONLY);
    BOOST_REQUIRE(fd > 0);
    char buffer[256] = {0};
    int bytes_read = ::read(fd, buffer, sizeof(buffer));
    BOOST_CHECK_EQUAL(bytes_read, (int)strlen(TEST_CONTENT));
    BOOST_CHECK_EQUAL(std::string(buffer), std::string(TEST_CONTENT));
    ::close(fd);

    // Clean up
    unlink(TEST_NEW_FILE_PATH);
    scheduler.stop();
    runner.join();
}

// ==================== Unlinkat Test ====================

static const char* TEST_UNLINK_FILE_PATH = "/tmp/yyasio_test_unlink.txt";

// Helper coroutine: async unlinkat and verify
Task<void> unlinkat_coro(Scheduler* scheduler, int dirfd, const char* pathname, int flags, std::atomic<int>& result)
{
    int ret = co_await unlinkat(scheduler, dirfd, pathname, flags);
    std::cout << "unlinkat_coro: ret = " << ret << "\n";
    result.store(ret);
}

BOOST_AUTO_TEST_CASE(test_unlinkat)
{
    // Create the file first
    int fd = ::open(TEST_UNLINK_FILE_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    BOOST_REQUIRE(fd > 0);
    ::write(fd, TEST_CONTENT, strlen(TEST_CONTENT));
    ::close(fd);

    // Verify file exists
    BOOST_CHECK_EQUAL(access(TEST_UNLINK_FILE_PATH, F_OK), 0);

    Scheduler scheduler;

    std::atomic<int> unlink_result{-1};
    unlinkat_coro(&scheduler, AT_FDCWD, TEST_UNLINK_FILE_PATH, 0, unlink_result).detach();

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (unlink_result.load() == -1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // unlinkat() returns 0 on success
    BOOST_CHECK_EQUAL(unlink_result.load(), 0);

    // Verify file no longer exists
    BOOST_CHECK(access(TEST_UNLINK_FILE_PATH, F_OK) != 0);
    scheduler.stop();
    runner.join();
}

// Test unlinkat on non-existent file: should return negative errno
Task<void> unlinkat_nonexist_coro(Scheduler* scheduler, std::atomic<int>& result)
{
    int ret = co_await unlinkat(scheduler, AT_FDCWD, "/tmp/yyasio_nonexistent_file_xyz.txt", 0);
    std::cout << "unlinkat_nonexist_coro: ret = " << ret << "\n";
    result.store(ret);
}

BOOST_AUTO_TEST_CASE(test_unlinkat_nonexistent)
{
    Scheduler scheduler;

    std::atomic<int> unlink_result{0};
    unlinkat_nonexist_coro(&scheduler, unlink_result).detach();

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (unlink_result.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // unlinkat on non-existent file should return -ENOENT
    BOOST_CHECK_LT(unlink_result.load(), 0);
    BOOST_CHECK_EQUAL(unlink_result.load(), -ENOENT);
    scheduler.stop();
    runner.join();
}

// ==================== Directory Operations Test (mkdirat / renameat / unlinkat) ====================

static const char* TEST_DIR_PATH = "/tmp/yyasio_test_dir";
static const char* TEST_DIR_RENAMED_PATH = "/tmp/yyasio_test_dir_renamed";
static const char* TEST_DIR_CHILD_PATH = "/tmp/yyasio_test_dir/child";
static const char* TEST_DIR_RENAMED_CHILD_PATH = "/tmp/yyasio_test_dir_renamed/child";
static const char* TEST_DIR_NONEMPTY_PATH = "/tmp/yyasio_test_dir_nonempty";
static const char* TEST_DIR_NONEMPTY_FILE_PATH = "/tmp/yyasio_test_dir_nonempty/inner.txt";

// Remove the fixed test paths so every case starts from a clean slate and leftovers
// from a failing run do not leak into the next one. Children first, then the dirs.
static void cleanup_dir_test_paths()
{
    unlink(TEST_DIR_NONEMPTY_FILE_PATH);
    unlink(TEST_DIR_CHILD_PATH);
    unlink(TEST_DIR_RENAMED_CHILD_PATH);
    rmdir(TEST_DIR_CHILD_PATH);
    rmdir(TEST_DIR_NONEMPTY_PATH);
    rmdir(TEST_DIR_PATH);
    rmdir(TEST_DIR_RENAMED_PATH);
}

// Create TEST_DIR_PATH twice, then create a relative subdirectory through the dirfd.
// The duplicate must fail with -EEXIST (proof the request really is a mkdir, not a
// no-op) and the relative one must succeed (proof dirfd is plumbed through the SQE).
Task<void> mkdirat_coro(Scheduler* scheduler, std::atomic<int>& root_ret,
                        std::atomic<int>& dup_ret, std::atomic<int>& child_ret)
{
    root_ret.store(co_await mkdirat(scheduler, AT_FDCWD, TEST_DIR_PATH, 0755));
    std::cout << "mkdirat_coro: root ret = " << root_ret.load() << "\n";
    BOOST_REQUIRE_EQUAL(root_ret.load(), 0);

    dup_ret.store(co_await mkdirat(scheduler, AT_FDCWD, TEST_DIR_PATH, 0755));
    std::cout << "mkdirat_coro: duplicate ret = " << dup_ret.load() << "\n";

    int dir_fd = co_await openat(scheduler, AT_FDCWD, TEST_DIR_PATH, O_RDONLY | O_DIRECTORY);
    BOOST_REQUIRE(dir_fd > 0);
    child_ret.store(co_await mkdirat(scheduler, dir_fd, "child", 0700));
    std::cout << "mkdirat_coro: child ret = " << child_ret.load() << "\n";
    BOOST_CHECK_EQUAL(co_await close(scheduler, dir_fd), 0);
}

BOOST_AUTO_TEST_CASE(test_mkdirat)
{
    cleanup_dir_test_paths();

    // Snapshot the umask while nothing else runs: mkdirat() applies it to `mode`.
    mode_t cur_umask = ::umask(0);
    ::umask(cur_umask);

    Scheduler scheduler;

    std::atomic<int> root_ret{-1};
    std::atomic<int> dup_ret{-1};
    std::atomic<int> child_ret{-1};
    mkdirat_coro(&scheduler, root_ret, dup_ret, child_ret).detach();

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (child_ret.load() == -1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // mkdirat() returns 0 on success
    BOOST_CHECK_EQUAL(root_ret.load(), 0);
    BOOST_CHECK_EQUAL(dup_ret.load(), -EEXIST);
    BOOST_CHECK_EQUAL(child_ret.load(), 0);

    // Both paths became directories with the requested mode masked by the umask
    struct stat st;
    BOOST_REQUIRE_EQUAL(stat(TEST_DIR_PATH, &st), 0);
    BOOST_CHECK(S_ISDIR(st.st_mode));
    BOOST_CHECK_EQUAL(st.st_mode & 0777, 0755 & ~cur_umask);

    BOOST_REQUIRE_EQUAL(stat(TEST_DIR_CHILD_PATH, &st), 0);
    BOOST_CHECK(S_ISDIR(st.st_mode));
    BOOST_CHECK_EQUAL(st.st_mode & 0777, 0700 & ~cur_umask);

    cleanup_dir_test_paths();
    scheduler.stop();
    runner.join();
}

// Rename a directory that holds a file: the entry inside has to move along with it.
Task<void> rename_dir_coro(Scheduler* scheduler, std::atomic<int>& rename_ret)
{
    BOOST_REQUIRE_EQUAL(co_await mkdirat(scheduler, AT_FDCWD, TEST_DIR_PATH, 0755), 0);

    int fd = co_await openat(scheduler, AT_FDCWD, TEST_DIR_CHILD_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    BOOST_REQUIRE(fd > 0);
    BOOST_CHECK_EQUAL(co_await write(scheduler, fd, TEST_CONTENT, strlen(TEST_CONTENT)),
                      (int)strlen(TEST_CONTENT));
    BOOST_CHECK_EQUAL(co_await close(scheduler, fd), 0);

    rename_ret.store(co_await renameat(scheduler, AT_FDCWD, TEST_DIR_PATH, AT_FDCWD, TEST_DIR_RENAMED_PATH));
    std::cout << "rename_dir_coro: ret = " << rename_ret.load() << "\n";
}

BOOST_AUTO_TEST_CASE(test_renameat_directory)
{
    cleanup_dir_test_paths();

    Scheduler scheduler;

    std::atomic<int> rename_ret{-1};
    rename_dir_coro(&scheduler, rename_ret).detach();

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (rename_ret.load() == -1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // renameat() returns 0 on success
    BOOST_CHECK_EQUAL(rename_ret.load(), 0);

    // The old name is gone, the new one is a directory
    struct stat st;
    BOOST_CHECK(access(TEST_DIR_PATH, F_OK) != 0);
    BOOST_REQUIRE_EQUAL(stat(TEST_DIR_RENAMED_PATH, &st), 0);
    BOOST_CHECK(S_ISDIR(st.st_mode));

    // The entry inside moved with the directory and kept its content
    int fd = ::open(TEST_DIR_RENAMED_CHILD_PATH, O_RDONLY);
    BOOST_REQUIRE(fd > 0);
    char buffer[256] = {0};
    int bytes_read = ::read(fd, buffer, sizeof(buffer));
    BOOST_CHECK_EQUAL(bytes_read, (int)strlen(TEST_CONTENT));
    BOOST_CHECK_EQUAL(std::string(buffer), std::string(TEST_CONTENT));
    ::close(fd);

    cleanup_dir_test_paths();
    scheduler.stop();
    runner.join();
}

// Remove an empty directory: unlinkat() without AT_REMOVEDIR must refuse, with the
// flag the directory is gone.
Task<void> remove_empty_dir_coro(Scheduler* scheduler, std::atomic<int>& plain_ret,
                                 std::atomic<int>& removedir_ret)
{
    BOOST_REQUIRE_EQUAL(co_await mkdirat(scheduler, AT_FDCWD, TEST_DIR_PATH, 0755), 0);

    plain_ret.store(co_await unlinkat(scheduler, AT_FDCWD, TEST_DIR_PATH, 0));
    std::cout << "remove_empty_dir_coro: unlink without flag ret = " << plain_ret.load() << "\n";

    removedir_ret.store(co_await unlinkat(scheduler, AT_FDCWD, TEST_DIR_PATH, AT_REMOVEDIR));
    std::cout << "remove_empty_dir_coro: unlink with AT_REMOVEDIR ret = " << removedir_ret.load() << "\n";
}

BOOST_AUTO_TEST_CASE(test_unlinkat_empty_directory)
{
    cleanup_dir_test_paths();

    Scheduler scheduler;

    std::atomic<int> plain_ret{-1};
    std::atomic<int> removedir_ret{-1};
    remove_empty_dir_coro(&scheduler, plain_ret, removedir_ret).detach();

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (removedir_ret.load() == -1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Directories are not removable through the plain unlink path (EISDIR on recent
    // kernels, EPERM historically) - the flag is what distinguishes the two.
    BOOST_CHECK_LT(plain_ret.load(), 0);

    // unlinkat(dir, AT_REMOVEDIR) returns 0 and the directory is really gone
    BOOST_CHECK_EQUAL(removedir_ret.load(), 0);
    BOOST_CHECK(access(TEST_DIR_PATH, F_OK) != 0);
    scheduler.stop();
    runner.join();
}

// A directory that still holds an entry cannot be removed: the kernel answers
// -ENOTEMPTY. yyasio has no recursive delete, so the caller has to empty the
// directory first; the same call then succeeds.
Task<void> remove_nonempty_dir_coro(Scheduler* scheduler, std::atomic<int>& busy_ret,
                                    std::atomic<int>& empty_ret)
{
    BOOST_REQUIRE_EQUAL(co_await mkdirat(scheduler, AT_FDCWD, TEST_DIR_NONEMPTY_PATH, 0755), 0);

    int fd = co_await openat(scheduler, AT_FDCWD, TEST_DIR_NONEMPTY_FILE_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    BOOST_REQUIRE(fd > 0);
    BOOST_CHECK_EQUAL(co_await close(scheduler, fd), 0);

    busy_ret.store(co_await unlinkat(scheduler, AT_FDCWD, TEST_DIR_NONEMPTY_PATH, AT_REMOVEDIR));
    std::cout << "remove_nonempty_dir_coro: non-empty ret = " << busy_ret.load() << "\n";

    BOOST_REQUIRE_EQUAL(co_await unlinkat(scheduler, AT_FDCWD, TEST_DIR_NONEMPTY_FILE_PATH, 0), 0);
    empty_ret.store(co_await unlinkat(scheduler, AT_FDCWD, TEST_DIR_NONEMPTY_PATH, AT_REMOVEDIR));
    std::cout << "remove_nonempty_dir_coro: after-emptying ret = " << empty_ret.load() << "\n";
}

BOOST_AUTO_TEST_CASE(test_unlinkat_nonempty_directory)
{
    cleanup_dir_test_paths();

    Scheduler scheduler;

    std::atomic<int> busy_ret{-1};
    std::atomic<int> empty_ret{-1};
    remove_nonempty_dir_coro(&scheduler, busy_ret, empty_ret).detach();

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (empty_ret.load() == -1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    BOOST_CHECK_EQUAL(busy_ret.load(), -ENOTEMPTY);
    BOOST_CHECK_EQUAL(empty_ret.load(), 0);

    // Nothing left behind under either name
    BOOST_CHECK(access(TEST_DIR_NONEMPTY_PATH, F_OK) != 0);
    BOOST_CHECK(access(TEST_DIR_NONEMPTY_FILE_PATH, F_OK) != 0);
    scheduler.stop();
    runner.join();
}

// ==================== Statx Test ====================

static const char* TEST_STATX_DIR_PATH     = "/tmp/yyasio_test_statx_dir";
static const char* TEST_STATX_FILE_NAME    = "file.txt";
static const char* TEST_STATX_FILE_PATH    = "/tmp/yyasio_test_statx_dir/file.txt";
static const char* TEST_STATX_LINK_PATH    = "/tmp/yyasio_test_statx_dir/link.txt";
static const char* TEST_STATX_MISSING_PATH = "/tmp/yyasio_statx_nonexistent_xyz.txt";

static void cleanup_statx_test_paths()
{
    unlink(TEST_STATX_LINK_PATH);
    unlink(TEST_STATX_FILE_PATH);
    rmdir(TEST_STATX_DIR_PATH);
}

// Buffers and return codes of one async run. The loop thread writes every field and
// the test thread only reads them once last_ret has published those writes, which is
// the same handoff the read() cases already rely on for their buffers.
struct StatxProbe
{
    struct statx absolute {};
    struct statx relative {};
    struct statx followed {};
    struct statx not_followed {};
    int absolute_ret = -1;
    int relative_ret = -1;
    int bogus_dirfd_ret = -1;
    int follow_ret = -1;
    int nofollow_ret = -1;
    int missing_ret = -1;
};

// Async answer must match what the synchronous syscall reports for the same arguments.
static void check_statx_like_sync(const struct statx& got, const struct statx& ref)
{
    BOOST_CHECK_EQUAL(got.stx_mask, ref.stx_mask);
    BOOST_CHECK_EQUAL(got.stx_mode, ref.stx_mode);
    BOOST_CHECK_EQUAL(got.stx_nlink, ref.stx_nlink);
    BOOST_CHECK_EQUAL(got.stx_ino, ref.stx_ino);
    BOOST_CHECK_EQUAL(got.stx_uid, ref.stx_uid);
    BOOST_CHECK_EQUAL(got.stx_gid, ref.stx_gid);
    BOOST_CHECK_EQUAL(got.stx_size, ref.stx_size);
    BOOST_CHECK_EQUAL(got.stx_attributes, ref.stx_attributes);
}

// Six requests in one coroutine: the file by absolute path, the same file through a
// dirfd with a relative name, that relative name again with a bogus dirfd, a symlink
// followed and not followed, and a path that does not exist.
//
// The arguments are covered by comparing each async answer with a synchronous call
// made using the same ones, which is what catches a dropped parameter: measured with
// this case, passing 0 instead of flags loses the symlink check, AT_FDCWD instead of
// dfd loses the relative name, and 0 instead of the mask shows up as stx_mask
// 0x173f instead of 0x17ff. No case pins a *narrow* mask though -- statx(2) backfills
// the basic set here whatever subset you ask for, so stx_mask cannot tell STATX_SIZE
// from a hardcoded default.
Task<void> statx_coro(Scheduler* scheduler, int dir_fd, StatxProbe& probe, std::atomic<int>& last_ret)
{
    probe.absolute_ret = co_await statx(scheduler, AT_FDCWD, TEST_STATX_FILE_PATH, &probe.absolute);
    probe.relative_ret = co_await statx(scheduler, dir_fd, TEST_STATX_FILE_NAME, &probe.relative);

    struct statx unused {};
    // -EBADF only if the dfd really comes from the SQE: a wrapper that ignored it and
    // fell back to AT_FDCWD would answer -ENOENT instead (no ./file.txt in the CWD).
    probe.bogus_dirfd_ret = co_await statx(scheduler, -1, TEST_STATX_FILE_NAME, &unused);

    probe.follow_ret = co_await statx(scheduler, AT_FDCWD, TEST_STATX_LINK_PATH, &probe.followed);
    probe.nofollow_ret = co_await statx(scheduler, AT_FDCWD, TEST_STATX_LINK_PATH, &probe.not_followed,
                                       AT_SYMLINK_NOFOLLOW);

    struct statx ghost {};
    // io_uring reports failures as -errno, where the libc wrapper returns -1 + errno
    probe.missing_ret = co_await statx(scheduler, AT_FDCWD, TEST_STATX_MISSING_PATH, &ghost);

    std::cout << "statx_coro: absolute = " << probe.absolute_ret
              << ", relative = " << probe.relative_ret
              << ", bogus_dirfd = " << probe.bogus_dirfd_ret
              << ", follow = " << probe.follow_ret
              << ", nofollow = " << probe.nofollow_ret
              << ", missing = " << probe.missing_ret << "\n";
    last_ret.store(probe.missing_ret, std::memory_order_release);
}

BOOST_AUTO_TEST_CASE(test_statx)
{
    cleanup_statx_test_paths();
    BOOST_REQUIRE_EQUAL(::mkdir(TEST_STATX_DIR_PATH, 0755), 0);

    int fd = ::open(TEST_STATX_FILE_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    BOOST_REQUIRE(fd > 0);
    BOOST_CHECK_EQUAL(::write(fd, TEST_CONTENT, strlen(TEST_CONTENT)), (long)strlen(TEST_CONTENT));
    BOOST_REQUIRE_EQUAL(::close(fd), 0);
    BOOST_REQUIRE_EQUAL(::symlink(TEST_STATX_FILE_PATH, TEST_STATX_LINK_PATH), 0);

    int dir_fd = ::open(TEST_STATX_DIR_PATH, O_RDONLY | O_DIRECTORY);
    BOOST_REQUIRE(dir_fd > 0);

    // Reference answers, taken while nothing else runs
    struct statx ref_absolute {};
    struct statx ref_relative {};
    struct statx ref_followed {};
    struct statx ref_not_followed {};
    BOOST_REQUIRE_EQUAL(::statx(AT_FDCWD, TEST_STATX_FILE_PATH, 0, STATX_BASIC_STATS, &ref_absolute), 0);
    BOOST_REQUIRE_EQUAL(::statx(dir_fd, TEST_STATX_FILE_NAME, 0, STATX_BASIC_STATS, &ref_relative), 0);
    BOOST_REQUIRE_EQUAL(::statx(AT_FDCWD, TEST_STATX_LINK_PATH, 0, STATX_BASIC_STATS, &ref_followed), 0);
    BOOST_REQUIRE_EQUAL(::statx(AT_FDCWD, TEST_STATX_LINK_PATH, AT_SYMLINK_NOFOLLOW,
                                STATX_BASIC_STATS, &ref_not_followed), 0);

    Scheduler scheduler;

    StatxProbe probe;
    std::atomic<int> last_ret{-1};
    statx_coro(&scheduler, dir_fd, probe, last_ret).detach();

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (last_ret.load(std::memory_order_acquire) == -1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // the error path doubles as the completion signal
    BOOST_CHECK_EQUAL(last_ret.load(), -ENOENT);
    BOOST_CHECK_EQUAL(probe.absolute_ret, 0);
    BOOST_CHECK_EQUAL(probe.relative_ret, 0);
    BOOST_CHECK_EQUAL(probe.bogus_dirfd_ret, -EBADF);
    BOOST_CHECK_EQUAL(probe.follow_ret, 0);
    BOOST_CHECK_EQUAL(probe.nofollow_ret, 0);

    // each async answer equals the synchronous one taken with the same arguments
    check_statx_like_sync(probe.absolute, ref_absolute);
    check_statx_like_sync(probe.relative, ref_relative);
    check_statx_like_sync(probe.followed, ref_followed);
    check_statx_like_sync(probe.not_followed, ref_not_followed);

    // and the values are the file written above, not whatever the buffer held
    BOOST_CHECK(S_ISREG(probe.absolute.stx_mode));
    BOOST_CHECK_EQUAL(probe.absolute.stx_size, (unsigned long long)strlen(TEST_CONTENT));
    BOOST_CHECK_EQUAL(probe.absolute.stx_nlink, 1);

    // the only argument whose effect is directly observable: which inode we got
    BOOST_CHECK(S_ISREG(probe.followed.stx_mode));
    BOOST_CHECK(S_ISLNK(probe.not_followed.stx_mode));
    BOOST_CHECK_EQUAL(probe.not_followed.stx_size,
                      (unsigned long long)strlen(TEST_STATX_FILE_PATH));

    BOOST_REQUIRE_EQUAL(::close(dir_fd), 0);
    cleanup_statx_test_paths();
    scheduler.stop();
    runner.join();
}

// ==================== SyncFileRange Test ====================

static const char* TEST_SYNC_FILE_PATH = "/tmp/yyasio_test_sync_range.txt";

// Helper coroutine: async sync_file_range and verify
Task<void> sync_file_range_coro(Scheduler* scheduler, int fd, uint64_t offset, unsigned len, int flags, std::atomic<int>& result)
{
    int ret = co_await sync_file_range(scheduler, fd, offset, len, flags);
    std::cout << "sync_file_range_coro: ret = " << ret << "\n";
    result.store(ret);
}

BOOST_AUTO_TEST_CASE(test_sync_file_range)
{
    // Create a file with dirty data in page cache
    int fd = ::open(TEST_SYNC_FILE_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    BOOST_REQUIRE(fd > 0);
    ::write(fd, TEST_CONTENT, strlen(TEST_CONTENT));

    Scheduler scheduler;

    std::atomic<int> sync_result{-1};
    // WAIT_BEFORE|WRITE|WAIT_AFTER: flush the range and wait for stable storage
    sync_file_range_coro(&scheduler, fd, 0, strlen(TEST_CONTENT),
                         SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER,
                         sync_result).detach();

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (sync_result.load() == -1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // sync_file_range() returns 0 on success
    BOOST_CHECK_EQUAL(sync_result.load(), 0);

    // Content should still be readable after sync
    char buffer[256] = {0};
    BOOST_CHECK_EQUAL(::pread(fd, buffer, sizeof(buffer), 0), (long)strlen(TEST_CONTENT));
    BOOST_CHECK_EQUAL(std::string(buffer), std::string(TEST_CONTENT));

    ::close(fd);
    unlink(TEST_SYNC_FILE_PATH);
    scheduler.stop();
    runner.join();
}

// Test sync_file_range on an invalid fd: should return -EBADF
Task<void> sync_file_range_badfd_coro(Scheduler* scheduler, std::atomic<int>& result)
{
    int ret = co_await sync_file_range(scheduler, -1, 0, 0, SYNC_FILE_RANGE_WRITE);
    std::cout << "sync_file_range_badfd_coro: ret = " << ret << "\n";
    result.store(ret);
}

BOOST_AUTO_TEST_CASE(test_sync_file_range_badfd)
{
    Scheduler scheduler;

    std::atomic<int> sync_result{0};
    sync_file_range_badfd_coro(&scheduler, sync_result).detach();

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (sync_result.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    BOOST_CHECK_EQUAL(sync_result.load(), -EBADF);
    scheduler.stop();
    runner.join();
}

// ==================== Mutex Test ====================

// Helper coroutine: lock, hold for hold_ns, record its tag, unlock
Task<void> mutex_locked_incr(Scheduler* scheduler, Mutex& mtx, long hold_ns,
                             int tag, std::vector<int>& order, std::atomic<int>& done)
{
    co_await mtx.lock();
    if (hold_ns > 0)
    {
        struct __kernel_timespec ts = { .tv_sec = hold_ns / 1000000000L, .tv_nsec = hold_ns % 1000000000L };
        co_await timeout(scheduler, &ts);
    }
    order.push_back(tag);
    mtx.unlock();
    done.fetch_add(1);
}

BOOST_AUTO_TEST_CASE(test_mutex_uncontended)
{
    // Nobody else holds the lock: lock() completes without suspending
    Scheduler scheduler;

    Mutex mtx;
    std::vector<int> order;
    std::atomic<int> done{0};

    // Same pattern as other IO tests: drive the coroutine before init(),
    // it stops at the first IO and is resumed once run() submits it.
    mutex_locked_incr(&scheduler, mtx, 10'000'000, 1, order, done).detach();

    BOOST_CHECK(mtx.try_lock() == false); // still held inside the critical section
    BOOST_CHECK_EQUAL(mtx.waiter_count(), 0u);

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (done.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    BOOST_CHECK_EQUAL(done.load(), 1);
    BOOST_CHECK_EQUAL(order.size(), 1u);
    BOOST_CHECK_EQUAL(order[0], 1);
    BOOST_CHECK(mtx.try_lock() == true); // released after unlock() with no waiters
    mtx.unlock();
    scheduler.stop();
    runner.join();
}

BOOST_AUTO_TEST_CASE(test_mutex_fifo_order)
{
    // Coroutines launched in order 0 -> 1 -> 2 -> 3:
    // holder 0 keeps the lock for 100ms, so 1..3 suspend in lock() and must
    // be woken in strict FIFO order by the ownership transfers in unlock().
    Scheduler scheduler;

    Mutex mtx;
    std::vector<int> order;
    std::atomic<int> done{0};

    long hold_ns = 100'000'000; // 100ms, holder 0 only
    mutex_locked_incr(&scheduler, mtx, hold_ns, 0, order, done).detach();
    BOOST_CHECK(mtx.try_lock() == false);
    mutex_locked_incr(&scheduler, mtx, 0, 1, order, done).detach();
    mutex_locked_incr(&scheduler, mtx, 0, 2, order, done).detach();
    mutex_locked_incr(&scheduler, mtx, 0, 3, order, done).detach();
    BOOST_CHECK_EQUAL(mtx.waiter_count(), 3u);

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (done.load() < 4 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    BOOST_CHECK_EQUAL(done.load(), 4);
    BOOST_CHECK_EQUAL(order.size(), 4u);
    for (int i = 0; i < 4; ++i)
    {
        BOOST_CHECK_EQUAL(order[i], i);
    }
    BOOST_CHECK_EQUAL(mtx.waiter_count(), 0u);
    scheduler.stop();
    runner.join();
}

BOOST_AUTO_TEST_CASE(test_mutex_try_lock)
{
    // try_lock fails while a coroutine holds the lock across a co_await point
    Scheduler scheduler;

    Mutex mtx;
    std::vector<int> order;
    std::atomic<int> done{0};

    mutex_locked_incr(&scheduler, mtx, 50'000'000, 0, order, done).detach();

    BOOST_CHECK(mtx.try_lock() == false);
    BOOST_CHECK(mtx.try_lock() == false); // must stay false, no stealing

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (done.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // After the holder unlocked, try_lock succeeds
    BOOST_CHECK(mtx.try_lock() == true);
    mtx.unlock();
    scheduler.stop();
    runner.join();
}

// Helper coroutine: acquire via lock_guard(), hold for hold_ns, record tag.
// The guard is scoped so its destructor calls unlock() at the closing brace.
Task<void> guard_locked_incr(Scheduler* scheduler, Mutex& mtx, long hold_ns,
                             int tag, std::vector<int>& order, std::atomic<int>& done)
{
    {
        auto guard = co_await mtx.lock_guard();
        if (hold_ns > 0)
        {
            struct __kernel_timespec ts = { .tv_sec = hold_ns / 1000000000L, .tv_nsec = hold_ns % 1000000000L };
            co_await timeout(scheduler, &ts);
        }
        order.push_back(tag);
    } // ~LockGuard unlocks here, before done is bumped
    done.fetch_add(1);
}

BOOST_AUTO_TEST_CASE(test_mutex_lock_guard_scoped)
{
    // Single coroutine: lock_guard() acquires without contending, and the
    // destructor releases the lock automatically at scope exit (no manual unlock).
    Scheduler scheduler;

    Mutex mtx;
    std::vector<int> order;
    std::atomic<int> done{0};

    guard_locked_incr(&scheduler, mtx, 10'000'000, 1, order, done).detach();
    BOOST_CHECK(mtx.try_lock() == false); // held inside the scope

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (done.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    BOOST_CHECK_EQUAL(done.load(), 1);
    BOOST_CHECK_EQUAL(order.size(), 1u);
    BOOST_CHECK_EQUAL(order[0], 1);
    BOOST_CHECK(mtx.try_lock() == true); // released by ~LockGuard
    mtx.unlock();
    scheduler.stop();
    runner.join();
}

BOOST_AUTO_TEST_CASE(test_mutex_lock_guard_fifo)
{
    // Four coroutines guarded by lock_guard: holder 0 keeps the lock for 100ms,
    // so 1..3 suspend in lock_guard() and are handed ownership in strict FIFO
    // order as each ~LockGuard fires unlock() at scope exit.
    Scheduler scheduler;

    Mutex mtx;
    std::vector<int> order;
    std::atomic<int> done{0};

    long hold_ns = 100'000'000; // 100ms, holder 0 only
    guard_locked_incr(&scheduler, mtx, hold_ns, 0, order, done).detach();
    guard_locked_incr(&scheduler, mtx, 0, 1, order, done).detach();
    guard_locked_incr(&scheduler, mtx, 0, 2, order, done).detach();
    guard_locked_incr(&scheduler, mtx, 0, 3, order, done).detach();
    BOOST_CHECK_EQUAL(mtx.waiter_count(), 3u);

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (done.load() < 4 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    BOOST_CHECK_EQUAL(done.load(), 4);
    BOOST_CHECK_EQUAL(order.size(), 4u);
    for (int i = 0; i < 4; ++i)
    {
        BOOST_CHECK_EQUAL(order[i], i);
    }
    BOOST_CHECK_EQUAL(mtx.waiter_count(), 0u);
    BOOST_CHECK(mtx.try_lock() == true); // fully released after last scope exit
    mtx.unlock();
    scheduler.stop();
    runner.join();
}

// ==================== Move-only Task Result Tests ====================
//
// Task<T> must also work when T cannot be copied (unique_ptr and friends): the
// value has to travel from `co_return` through the child's promise result into
// the awaiting parent using moves only.

static const int MOVE_ONLY_MAGIC = 0x5AA5;

// A non-copyable type counting its live owners: an accidental copy in the
// result path does not even compile, and an accidental drop/duplicate shows up
// as owners() != 1.
class MoveOnlyToken
{
public:
    MoveOnlyToken() = default; // Task<T>'s promise default-constructs `result`
    explicit MoveOnlyToken(int v): _value(v), _owner(true) { owners() += 1; }
    MoveOnlyToken(const MoveOnlyToken&) = delete;
    MoveOnlyToken& operator=(const MoveOnlyToken&) = delete;
    MoveOnlyToken(MoveOnlyToken&& other) noexcept: _value(other._value), _owner(other._owner)
    { other._owner = false; } // ownership handed over, total unchanged
    MoveOnlyToken& operator=(MoveOnlyToken&& other) noexcept
    {
        if (_owner) owners() -= 1; // release what we held before taking over
        _value = other._value;
        _owner = other._owner;
        other._owner = false;
        return *this;
    }
    ~MoveOnlyToken() { if (_owner) owners() -= 1; }
    int value() const { return _value; }
    static int& owners() { static int n = 0; return n; }
private:
    int _value = 0;
    bool _owner = false;
};

// Counts live heap objects so we can check a unique_ptr result is destroyed
// exactly once (no leak, no double free).
class ReleaseProbe
{
public:
    ReleaseProbe() { ++live(); }
    ~ReleaseProbe() { --live(); }
    ReleaseProbe(const ReleaseProbe&) = delete;
    ReleaseProbe& operator=(const ReleaseProbe&) = delete;
    static int& live() { static int n = 0; return n; }
};

Task<std::unique_ptr<int>> make_int_ptr_sync()
{
    co_return std::make_unique<int>(MOVE_ONLY_MAGIC);
}

Task<void> take_int_ptr_sync(std::unique_ptr<int>& out, std::atomic<bool>& done)
{
    out = co_await make_int_ptr_sync();
    done.store(true);
}

BOOST_AUTO_TEST_CASE(test_task_move_only_result_sync)
{
    // No scheduler: the child has no IO point, so the whole
    // parent -> child -> parent handoff runs synchronously inside detach().
    std::unique_ptr<int> out;
    std::atomic<bool> done{false};

    take_int_ptr_sync(out, done).detach();

    BOOST_REQUIRE(done.load());
    BOOST_REQUIRE(out != nullptr); // ownership reached the awaiting parent
    BOOST_CHECK_EQUAL(*out, MOVE_ONLY_MAGIC);
}

Task<std::unique_ptr<int>> maybe_int_ptr_sync(bool produce)
{
    if (!produce) co_return nullptr; // an empty move-only result must survive too
    co_return std::make_unique<int>(MOVE_ONLY_MAGIC);
}

Task<void> take_optional_ptr_sync(std::unique_ptr<int>& out, bool produce, std::atomic<bool>& done)
{
    out = co_await maybe_int_ptr_sync(produce);
    done.store(true);
}

BOOST_AUTO_TEST_CASE(test_task_move_only_result_null_sync)
{
    // co_return nullptr goes through return_value(T value) as well
    std::unique_ptr<int> out = std::make_unique<int>(-1);
    std::atomic<bool> done{false};

    take_optional_ptr_sync(out, false, done).detach();

    BOOST_REQUIRE(done.load());
    BOOST_CHECK(out == nullptr);

    // and the non-empty branch still works
    done.store(false);
    take_optional_ptr_sync(out, true, done).detach();
    BOOST_REQUIRE(done.load());
    BOOST_REQUIRE(out != nullptr);
    BOOST_CHECK_EQUAL(*out, MOVE_ONLY_MAGIC);
}

Task<std::unique_ptr<int>> make_int_ptr_after_io(Scheduler* scheduler, int value)
{
    struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 10'000'000 }; // 10ms
    co_await timeout(scheduler, &ts);
    // the value is only produced after the coroutine was resumed by the loop
    co_return std::make_unique<int>(value);
}

Task<void> take_int_ptr_after_io(Scheduler* scheduler, std::unique_ptr<int>& out, std::atomic<bool>& done)
{
    out = co_await make_int_ptr_after_io(scheduler, MOVE_ONLY_MAGIC);
    done.store(true);
}

BOOST_AUTO_TEST_CASE(test_task_move_only_result_async)
{
    // The move-only result is produced after an io_uring round trip: the value
    // must survive suspension in the child frame and be moved out in await_resume.
    Scheduler scheduler;

    std::unique_ptr<int> out;
    std::atomic<bool> done{false};
    take_int_ptr_after_io(&scheduler, out, done).detach();

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    BOOST_REQUIRE(done.load());
    BOOST_REQUIRE(out != nullptr);
    BOOST_CHECK_EQUAL(*out, MOVE_ONLY_MAGIC);
    scheduler.stop();
    runner.join();
}

Task<MoveOnlyToken> make_token_after_io(Scheduler* scheduler, int value)
{
    struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 10'000'000 }; // 10ms
    co_await timeout(scheduler, &ts);
    co_return MoveOnlyToken(value);
}

Task<void> take_token_after_io(Scheduler* scheduler, int base,
                               std::atomic<int>& held_owners,
                               std::atomic<int>& released_owners,
                               std::atomic<bool>& done)
{
    {
        auto token = co_await make_token_after_io(scheduler, MOVE_ONLY_MAGIC);
        BOOST_CHECK_EQUAL(token.value(), MOVE_ONLY_MAGIC);
        // exactly one instance holds the resource: the child frame was already
        // destroyed in await_resume, nothing was duplicated on the way out
        held_owners.store(MoveOnlyToken::owners() - base);
    } // the parent's local dies here and releases the resource
    released_owners.store(MoveOnlyToken::owners() - base);
    done.store(true);
}

BOOST_AUTO_TEST_CASE(test_task_move_only_token_ownership)
{
    Scheduler scheduler;

    const int base = MoveOnlyToken::owners();
    std::atomic<int> held_owners{-1};
    std::atomic<int> released_owners{-1};
    std::atomic<bool> done{false};
    take_token_after_io(&scheduler, base, held_owners, released_owners, done).detach();

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    BOOST_REQUIRE(done.load());
    BOOST_CHECK_EQUAL(held_owners.load(), 1);
    BOOST_CHECK_EQUAL(released_owners.load(), 0);
    BOOST_CHECK_EQUAL(MoveOnlyToken::owners(), base); // the coroutine frames left nothing behind
    scheduler.stop();
    runner.join();
}

Task<std::unique_ptr<std::string>> inner_string_ptr_coro(const char* text)
{
    co_return std::make_unique<std::string>(text);
}

// Middle coroutine: takes the inner move-only result and forwards it later.
// Note `co_return p;` would be ill-formed for a move-only T (return_value takes
// its argument by value), the forward has to move explicitly.
Task<std::unique_ptr<std::string>> forward_string_ptr_coro(Scheduler* scheduler, const char* text,
                                                           std::atomic<bool>& inner_checked)
{
    auto p = co_await inner_string_ptr_coro(text);
    BOOST_REQUIRE(p != nullptr);
    BOOST_CHECK_EQUAL(*p, text);
    inner_checked.store(true);

    // keep the move-only result alive across another suspension point: it sits
    // in this coroutine's frame while the timeout is in flight
    struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 10'000'000 }; // 10ms
    co_await timeout(scheduler, &ts);
    BOOST_REQUIRE(p != nullptr);
    BOOST_CHECK_EQUAL(*p, text);

    co_return std::move(p);
}

Task<void> take_forwarded_ptr_coro(Scheduler* scheduler, std::unique_ptr<std::string>& out,
                                   std::atomic<bool>& inner_checked, std::atomic<bool>& done)
{
    out = co_await forward_string_ptr_coro(scheduler, "move-only chain", inner_checked);
    done.store(true);
}

BOOST_AUTO_TEST_CASE(test_task_move_only_forward_nested)
{
    // grandchild -> child -> parent: the value is moved through two frames and
    // held across a suspension point before being returned again.
    Scheduler scheduler;

    std::unique_ptr<std::string> out;
    std::atomic<bool> inner_checked{false};
    std::atomic<bool> done{false};
    take_forwarded_ptr_coro(&scheduler, out, inner_checked, done).detach();

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    BOOST_REQUIRE(done.load());
    BOOST_CHECK(inner_checked.load());
    BOOST_REQUIRE(out != nullptr);
    BOOST_CHECK_EQUAL(*out, "move-only chain");
    scheduler.stop();
    runner.join();
}

Task<std::unique_ptr<ReleaseProbe>> make_probe_ptr_coro(Event<int>& gate, std::atomic<bool>& produced)
{
    co_await gate.wait(); // suspend until somebody sets the event
    produced.store(true);
    co_return std::make_unique<ReleaseProbe>();
}

BOOST_AUTO_TEST_CASE(test_task_move_only_detached_result_released)
{
    // Fire-and-forget: nobody co_awaits the Task, so the move-only result is
    // produced in the frame and destroyed together with it, exactly once.
    const int base = ReleaseProbe::live();
    Event<int> gate;
    std::atomic<bool> produced{false};

    auto task = make_probe_ptr_coro(gate, produced);
    task.detach(); // runs until co_await gate.wait()
    BOOST_CHECK(!produced.load());
    BOOST_CHECK_EQUAL(ReleaseProbe::live(), base);

    gate.set(1); // runs to completion, the frame self-destroys in FinalAwaiter
    BOOST_CHECK(produced.load());
    BOOST_CHECK_EQUAL(ReleaseProbe::live(), base);
}

// ==================== Move-only Event Value Tests ====================
//
// Event<T> publishes its value with moves only (set() moves the parameter in,
// await_resume() moves it out), so a non-copyable T works while every copyable
// T keeps behaving as before. The helpers MoveOnlyToken/ReleaseProbe above are
// reused here.

// Waits for the probe and reports whether it was alive at hand-off time.
Task<void> consume_probe_ptr(Event<std::unique_ptr<ReleaseProbe>>& event,
                             std::atomic<ReleaseProbe*>& seen,
                             std::atomic<bool>& alive_at_handoff)
{
    auto p = co_await event.wait();
    seen.store(p.get());
    alive_at_handoff.store(ReleaseProbe::live() > 0);
} // p dies together with the coroutine frame: the probe is released here

BOOST_AUTO_TEST_CASE(test_event_move_only_ptr_handoff)
{
    // A move-only value published to a coroutine that is suspended on the Event
    const int base = ReleaseProbe::live();
    Event<std::unique_ptr<ReleaseProbe>> event;
    std::atomic<ReleaseProbe*> seen{nullptr};
    std::atomic<bool> alive_at_handoff{false};

    consume_probe_ptr(event, seen, alive_at_handoff).detach(); // suspends inside wait()
    BOOST_CHECK(seen.load() == nullptr);
    BOOST_CHECK_EQUAL(ReleaseProbe::live(), base);

    auto probe = std::make_unique<ReleaseProbe>();
    ReleaseProbe* expected = probe.get();
    event.set(std::move(probe)); // resumes the waiter, which releases the probe before returning

    BOOST_CHECK(seen.load() == expected); // ownership reached the waiter
    BOOST_CHECK(alive_at_handoff.load());
    BOOST_CHECK(probe == nullptr);        // the publisher gave it up
    BOOST_CHECK_EQUAL(ReleaseProbe::live(), base); // released exactly once: no leak, no double free
}

Task<void> take_ptr_after_gate(Event<int>& gate, Event<std::unique_ptr<int>>& source, std::atomic<int>& seen)
{
    co_await gate.wait();
    auto p = co_await source.wait();
    seen.store(p ? *p : -1);
}

BOOST_AUTO_TEST_CASE(test_event_move_only_set_before_wait)
{
    // The value is published while no coroutine is waiting yet: it must stay
    // pending inside the Event and be moved out by the waiter that arrives later.
    Event<int> gate;
    Event<std::unique_ptr<int>> source;
    std::atomic<int> seen{0};

    take_ptr_after_gate(gate, source, seen).detach(); // suspended on gate

    auto value = std::make_unique<int>(MOVE_ONLY_MAGIC);
    source.set(std::move(value));
    BOOST_CHECK(value == nullptr);   // moved into the Event
    BOOST_CHECK_EQUAL(seen.load(), 0); // the waiter has not run yet

    gate.set(1);                     // now it reaches source.wait() and takes the pending value
    BOOST_CHECK_EQUAL(seen.load(), MOVE_ONLY_MAGIC);
}

BOOST_AUTO_TEST_CASE(test_event_move_only_overwrite_releases_previous)
{
    // Event holds a single pending value: overwriting it must destroy the
    // previous one exactly once and hand over the last published value.
    const int base = ReleaseProbe::live();
    Event<std::unique_ptr<ReleaseProbe>> event;

    auto first = std::make_unique<ReleaseProbe>();
    ReleaseProbe* first_addr = first.get();
    event.set(std::move(first)); // nobody waiting yet -> stored as pending
    BOOST_CHECK_EQUAL(ReleaseProbe::live(), base + 1);

    auto second = std::make_unique<ReleaseProbe>();
    ReleaseProbe* second_addr = second.get();
    event.set(std::move(second)); // replaces the pending value
    BOOST_CHECK_EQUAL(ReleaseProbe::live(), base + 1); // the first was released, not leaked

    std::atomic<ReleaseProbe*> seen{nullptr};
    std::atomic<bool> alive_at_handoff{false};
    consume_probe_ptr(event, seen, alive_at_handoff).detach(); // pending -> no suspension

    BOOST_CHECK(seen.load() == second_addr);
    BOOST_CHECK(seen.load() != first_addr);
    BOOST_CHECK(alive_at_handoff.load());
    BOOST_CHECK_EQUAL(ReleaseProbe::live(), base);
}

Task<void> take_token_from_event(Event<MoveOnlyToken>& event, int base,
                                 std::atomic<int>& held_owners,
                                 std::atomic<int>& seen_value,
                                 std::atomic<bool>& done)
{
    auto token = co_await event.wait();
    seen_value.store(token.value());
    // the Event still owns a moved-from shell: nobody duplicated the resource
    held_owners.store(MoveOnlyToken::owners() - base);
    done.store(true);
} // token released here, back to base

BOOST_AUTO_TEST_CASE(test_event_move_only_token_ownership)
{
    Event<MoveOnlyToken> event;
    const int base = MoveOnlyToken::owners();
    std::atomic<int> held_owners{-1};
    std::atomic<int> seen_value{0};
    std::atomic<bool> done{false};

    take_token_from_event(event, base, held_owners, seen_value, done).detach();
    BOOST_CHECK_EQUAL(MoveOnlyToken::owners(), base); // still suspended, nothing published

    event.set(MoveOnlyToken(MOVE_ONLY_MAGIC));

    BOOST_REQUIRE(done.load());
    BOOST_CHECK_EQUAL(seen_value.load(), MOVE_ONLY_MAGIC);
    BOOST_CHECK_EQUAL(held_owners.load(), 1); // exactly one owner during hand-off
    BOOST_CHECK_EQUAL(MoveOnlyToken::owners(), base);
}

static const char* EVENT_STRING_VALUE = "copyable but not trivially copyable"; // > 15 chars: heap buffer, so a move really steals

Task<void> take_string_from_event(Event<std::string>& event, std::string& out, std::atomic<bool>& done)
{
    out = co_await event.wait();
    done.store(true);
}

BOOST_AUTO_TEST_CASE(test_event_copyable_nontrivial_still_works)
{
    // Regression guard for the move-based set()/await_resume(): std::string is
    // copyable but NOT trivially copyable, it must still work, and publishing an
    // lvalue must still leave the publisher untouched (copy in, not move).
    Event<std::string> event;
    std::string published = EVENT_STRING_VALUE;
    std::string out;
    std::atomic<bool> done{false};

    take_string_from_event(event, out, done).detach();
    event.set(published); // lvalue

    BOOST_REQUIRE(done.load());
    BOOST_CHECK_EQUAL(out, EVENT_STRING_VALUE);
    BOOST_CHECK(!published.empty()); // the publisher kept its own copy

    // an rvalue publisher hands the value over instead of copying it
    std::string moved_out;
    Event<std::string> event2;
    std::atomic<bool> done2{false};
    std::string source = EVENT_STRING_VALUE;
    take_string_from_event(event2, moved_out, done2).detach();
    event2.set(std::move(source));

    BOOST_REQUIRE(done2.load());
    BOOST_CHECK_EQUAL(moved_out, EVENT_STRING_VALUE);
    BOOST_CHECK(source.empty()); // moved out of the publisher
}

Task<void> publish_ptr_after_io(Scheduler* scheduler, Event<std::unique_ptr<int>>& event,
                                int value, std::atomic<bool>& published)
{
    struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 10'000'000 }; // 10ms
    co_await timeout(scheduler, &ts);
    published.store(true);
    event.set(std::make_unique<int>(value)); // resumes the waiting consumer
}

Task<void> consume_ptr_in_loop(Scheduler* scheduler, Event<std::unique_ptr<int>>& event,
                               std::atomic<int>& seen, std::atomic<bool>& done)
{
    struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 10'000'000 }; // 10ms
    co_await timeout(scheduler, &ts);
    auto p = co_await event.wait();
    seen.store(p ? *p : -1);
    done.store(true);
}

BOOST_AUTO_TEST_CASE(test_event_move_only_across_scheduler_coroutines)
{
    // Producer and consumer both driven by the io_uring loop: the move-only value
    // is created in one coroutine and destroyed in another.
    Scheduler scheduler;

    Event<std::unique_ptr<int>> event;
    std::atomic<bool> published{false};
    std::atomic<int> seen{0};
    std::atomic<bool> done{false};

    consume_ptr_in_loop(&scheduler, event, seen, done).detach();
    publish_ptr_after_io(&scheduler, event, MOVE_ONLY_MAGIC, published).detach();

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    BOOST_CHECK(published.load());
    BOOST_CHECK_EQUAL(seen.load(), MOVE_ONLY_MAGIC);
    scheduler.stop();
    runner.join();
}

// ==================== TimeoutEvent / Scheduler::abandon() Tests ====================
//
// TimeoutEvent::wait_until() 会向 scheduler 提交一个 IORING_OP_TIMEOUT，并把它的
// index 记在 _cancel_index 上。若协程是被 set() 提前唤醒的，set() 先 abandon 这个
// index，保证那个定时器 CQE 迟到时既不会 resume 已经推进/销毁的协程，也不会再写
// 它的结果槽。因此下面每个用例都在盯 times：一旦超过预期次数，就是发生了二次唤醒
// （协程帧此时已被销毁，开了 ASan 会直接报 heap-use-after-free）。

struct TimeoutWakeLog {
    std::atomic<int> times{0};
    std::atomic<long long> elapsed_ms[4]{};   // 每次唤醒距该轮 co_await 开始的毫秒数
};

static long long ms_since(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start).count();
}

// 记录一次唤醒，返回它在 log 里的槽位
static int record_wakeup(TimeoutWakeLog& log, std::chrono::steady_clock::time_point start)
{
    int slot = log.times.fetch_add(1);
    if (slot < 4) log.elapsed_ms[slot].store(ms_since(start));
    return slot;
}

// ---------- 1. 只有超时，没人 set ----------

Task<void> wait_timeout_once(TimeoutEvent* ev, TimeoutWakeLog& log, std::atomic<bool>& done)
{
    auto start = std::chrono::steady_clock::now();
    co_await ev->wait_until(std::chrono::milliseconds(1500));
    record_wakeup(log, start);
    done.store(true);
}

BOOST_AUTO_TEST_CASE(test_timeout_event_fires_on_timeout)
{
    Scheduler scheduler;
    TimeoutEvent ev(&scheduler);
    TimeoutWakeLog log;
    std::atomic<bool> done{false};

    // 沿用本文件的约定：detach() 在启动 runner 线程之前完成，
    // 于是 timer SQE 先停在 pending_queue 里，init/run 与协程同线程。
    wait_timeout_once(&ev, log, done).detach();

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    BOOST_CHECK(done.load());
    BOOST_CHECK_EQUAL(log.times.load(), 1);
    // 1500ms 的亚秒部分不能丢：若只保留 tv_sec 会退化成整 1s 就触发
    BOOST_CHECK_GE(log.elapsed_ms[0].load(), 1400);
    BOOST_CHECK_LE(log.elapsed_ms[0].load(), 3000);

    scheduler.stop();
    runner.join();
}

// ---------- 2. set() 抢先，未到期定时器被 abandon ----------

Task<void> wait_until_set_arrives(TimeoutEvent* ev, TimeoutWakeLog& log, std::atomic<bool>& done)
{
    auto start = std::chrono::steady_clock::now();
    co_await ev->wait_until(std::chrono::milliseconds(1200));
    record_wakeup(log, start);
    done.store(true);
}

Task<void> set_after_50ms(Scheduler* scheduler, TimeoutEvent* ev)
{
    struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 50'000'000 };
    co_await timeout(scheduler, &ts);
    ev->set();
}

BOOST_AUTO_TEST_CASE(test_timeout_event_set_wins_and_timer_is_abandoned)
{
    Scheduler scheduler;
    TimeoutEvent ev(&scheduler);
    TimeoutWakeLog log;
    std::atomic<bool> done{false};

    // 两个协程都在 run() 之前挂起：waiter 已登记 _coro，setter 的 50ms 定时器与
    // waiter 的 1200ms 定时器一起进 pending_queue，时序确定、没有竞态。
    wait_until_set_arrives(&ev, log, done).detach();
    set_after_50ms(&scheduler, &ev).detach();

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    BOOST_CHECK(done.load());
    BOOST_CHECK_EQUAL(log.times.load(), 1);
    BOOST_CHECK_GE(log.elapsed_ms[0].load(), 30);   // 不是秒级抖动
    BOOST_CHECK_LE(log.elapsed_ms[0].load(), 800);  // 远早于 1200ms，是被 set 唤醒的

    // 跨过原来的超时期限，确认迟到的定时器 CQE 没有第二次唤醒协程
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    BOOST_CHECK_EQUAL(log.times.load(), 1);

    scheduler.stop();
    runner.join();
    BOOST_CHECK_EQUAL(log.times.load(), 1);
}

// ---------- 3. 同一 event 复用：超时路径 ----------

Task<void> wait_timeout_three_times(TimeoutEvent* ev, TimeoutWakeLog& log, std::atomic<bool>& done)
{
    for (int i = 0; i < 3; ++i)
    {
        auto start = std::chrono::steady_clock::now();
        co_await ev->wait_until(std::chrono::milliseconds(30));
        record_wakeup(log, start);
    }
    done.store(true);
}

BOOST_AUTO_TEST_CASE(test_timeout_event_reusable_after_timeout)
{
    // 回归用例：正常超时路径以前不复位 _cancel_index，第二次 co_await 会撞在
    // await_suspend 的 assert(_cancel_index == max) 上直接 abort。
    Scheduler scheduler;
    TimeoutEvent ev(&scheduler);
    TimeoutWakeLog log;
    std::atomic<bool> done{false};

    wait_timeout_three_times(&ev, log, done).detach();

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    BOOST_CHECK(done.load());
    BOOST_CHECK_EQUAL(log.times.load(), 3);
    for (int i = 0; i < 3; ++i) {
        BOOST_CHECK_GE(log.elapsed_ms[i].load(), 20);   // 每轮都真的等满了
        BOOST_CHECK_LE(log.elapsed_ms[i].load(), 1000);
    }

    scheduler.stop();
    runner.join();
}

// ---------- 4. 同一 event 复用：abandon 之后还能重新武装 ----------

Task<void> wait_two_rounds_after_set(TimeoutEvent* ev, TimeoutWakeLog& log, std::atomic<bool>& done)
{
    // 第 1 轮：1000ms 定时器，会被 set() 提前打断并 abandon
    auto t0 = std::chrono::steady_clock::now();
    co_await ev->wait_until(std::chrono::milliseconds(1000));
    record_wakeup(log, t0);

    // 第 2 轮：上一轮的 cancel 不能影响这一轮，正常靠超时唤醒
    auto t1 = std::chrono::steady_clock::now();
    co_await ev->wait_until(std::chrono::milliseconds(40));
    record_wakeup(log, t1);

    done.store(true);
}

BOOST_AUTO_TEST_CASE(test_timeout_event_rearmable_after_abandon)
{
    Scheduler scheduler;
    TimeoutEvent ev(&scheduler);
    TimeoutWakeLog log;
    std::atomic<bool> done{false};

    wait_two_rounds_after_set(&ev, log, done).detach();
    set_after_50ms(&scheduler, &ev).detach();

    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    BOOST_CHECK(done.load());
    BOOST_CHECK_EQUAL(log.times.load(), 2);
    BOOST_CHECK_LE(log.elapsed_ms[0].load(), 800);   // 第 1 轮由 set 提前唤醒
    BOOST_CHECK_LE(log.elapsed_ms[1].load(), 1000);  // 第 2 轮由自己的超时唤醒

    // 等过第 1 轮那个 1000ms 定时器的原始期限，确认它没有再来一次
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    BOOST_CHECK_EQUAL(log.times.load(), 2);

    scheduler.stop();
    runner.join();
}

// ---------- 5. Scheduler 层：abandon 命中尚未提交的 pending item ----------

BOOST_AUTO_TEST_CASE(test_abandon_drops_pending_item_and_caller_resumes)
{
    Scheduler scheduler;
    TimeoutEvent ev(&scheduler);
    TimeoutWakeLog log;
    std::atomic<bool> done{false};

    // 注意此时还没有 init()/run()：协程挂起后它的 timer SQE 只停在 pending_queue 里，
    // 于是 abandon() 走的是 pending 分支（丢弃 SQE、不发 cancel），resume 由调用方负责。
    wait_until_set_arrives(&ev, log, done).detach();
    BOOST_REQUIRE(!done.load());

    ev.set();
    BOOST_CHECK_EQUAL(log.times.load(), 1);
    BOOST_CHECK(done.load());   // 协程被调用方 resume 后一路跑到销毁

    // 现在才把循环启动起来：那个被丢弃的 item 不该被提交，也不该有任何 CQE 二次唤醒
    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    BOOST_CHECK_EQUAL(log.times.load(), 1);

    scheduler.stop();
    runner.join();
    BOOST_CHECK_EQUAL(log.times.load(), 1);
}

// ---------- 6. Scheduler 层：abandon 未知 index 是 no-op ----------

BOOST_AUTO_TEST_CASE(test_abandon_unknown_index_is_noop)
{
    Scheduler scheduler;

    // TimeoutEvent 用 ULLONG_MAX 当「没有定时器」的哨兵，set() 会在哨兵状态下也调
    // abandon；从未 schedule 过的 index 同样不该有任何副作用（幂等）。
    scheduler.abandon(std::numeric_limits<uint64_t>::max());
    scheduler.abandon(12345);
    scheduler.abandon(0);

    // 队列/在飞表都是空的，循环靠 idle timeout 兜底，应能干净启停
    std::thread runner([&scheduler]() {
        run_scheduler(scheduler);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    scheduler.stop();
    runner.join();

    BOOST_CHECK(true);
}

BOOST_AUTO_TEST_SUITE_END()
