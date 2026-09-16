#define BOOST_TEST_MODULE YyasioTest
#include <boost/test/unit_test.hpp>

#include "yyasio.h"
#include <array>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

using namespace yyasio;

BOOST_AUTO_TEST_SUITE(YYasioTests)

// Helper coroutine that waits on a promise and stores the result
Task<void> wait_for_promise(Promise<int>& promise, std::atomic<int>& result)
{
    std::cout << "Coroutine: about to wait for promise\n";
    int val = co_await promise.wait();
    std::cout << "Coroutine: got value " << val << "\n";
    result.store(val);
    std::cout << "Coroutine: stored value, about to return\n";
}

BOOST_AUTO_TEST_CASE(test_promise_basic)
{
    Promise<int> promise;
    std::atomic<int> result{-1};

    std::cout << "Test: starting coroutine\n";
    // Start the coroutine - it will suspend on co_await promise.wait()
    wait_for_promise(promise, result).detach();

    std::cout << "Test: coroutine started, result = " << result.load() << "\n";
    // At this point, the coroutine is suspended, waiting for the promise
    BOOST_CHECK_EQUAL(result.load(), -1);

    std::cout << "Test: about to resume promise\n";
    // Resume the coroutine by fulfilling the promise
    promise.resume(42);

    std::cout << "Test: promise resumed, result = " << result.load() << "\n";
    // The coroutine should have resumed and stored the value
    BOOST_CHECK_EQUAL(result.load(), 42);
}

// Helper coroutine for testing multiple resumes
Task<void> wait_for_promise_loop(Promise<int>& promise, std::vector<int>& results, int count)
{
    for (int i = 0; i < count; ++i)
    {
        int val = co_await promise.wait();
        results.push_back(val);
    }
}

BOOST_AUTO_TEST_CASE(test_promise_multiple_resumes)
{
    Promise<int> promise;
    std::vector<int> results;

    // Start the coroutine - it will wait in a loop
    wait_for_promise_loop(promise, results, 3).detach();

    // Resume multiple times
    promise.resume(1);
    BOOST_CHECK_EQUAL(results.size(), 1);
    BOOST_CHECK_EQUAL(results[0], 1);

    promise.resume(2);
    BOOST_CHECK_EQUAL(results.size(), 2);
    BOOST_CHECK_EQUAL(results[1], 2);

    promise.resume(3);
    BOOST_CHECK_EQUAL(results.size(), 3);
    BOOST_CHECK_EQUAL(results[2], 3);
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
    BOOST_REQUIRE_EQUAL(scheduler.init(16), YYASIO_OK);

    std::atomic<int> fd{-1};
    open_file_coro(&scheduler, fd).detach();

    std::thread runner([&scheduler]() { scheduler.run(); });
    runner.detach();

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (fd.load() == -1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    BOOST_CHECK(fd.load() > 0);
    if (fd.load() > 0) {
        ::close(fd.load());
    }
    unlink(TEST_FILE_PATH);
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
    BOOST_REQUIRE_EQUAL(scheduler.init(16), YYASIO_OK);

    std::atomic<int> bytes_written{0};
    write_file_coro(&scheduler, fd, bytes_written).detach();

    std::thread runner([&scheduler]() { scheduler.run(); });
    runner.detach();

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (bytes_written.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    BOOST_CHECK_EQUAL(bytes_written.load(), (int)strlen(TEST_CONTENT));
    ::close(fd);
    unlink(TEST_FILE_PATH);
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
    BOOST_REQUIRE_EQUAL(scheduler.init(16), YYASIO_OK);

    char buffer[256] = {0};
    std::atomic<int> bytes_read{0};
    read_file_coro(&scheduler, fd, buffer, sizeof(buffer), bytes_read).detach();

    std::thread runner([&scheduler]() { scheduler.run(); });
    runner.detach();

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (bytes_read.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    BOOST_CHECK_EQUAL(bytes_read.load(), (int)strlen(TEST_CONTENT));
    BOOST_CHECK_EQUAL(std::string(buffer), std::string(TEST_CONTENT));
    ::close(fd);
    unlink(TEST_FILE_PATH);
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
    BOOST_REQUIRE_EQUAL(scheduler.init(16), YYASIO_OK);

    std::atomic<int> close_result{-1};
    close_file_coro(&scheduler, fd, close_result).detach();

    std::thread runner([&scheduler]() { scheduler.run(); });
    runner.detach();

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (close_result.load() == -1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // close() returns 0 on success
    BOOST_CHECK_EQUAL(close_result.load(), 0);
    unlink(TEST_FILE_PATH);
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
    BOOST_REQUIRE_EQUAL(scheduler.init(16), YYASIO_OK);

    std::atomic<bool> done{false};
    file_io_combined_coro(&scheduler, done).detach();

    std::thread runner([&scheduler]() { scheduler.run(); });
    runner.detach();

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    BOOST_CHECK(done.load());
    unlink(TEST_FILE_PATH);
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
    BOOST_REQUIRE_EQUAL(scheduler.init(16), YYASIO_OK);

    std::atomic<int> result{0};

    // Start the parent coroutine - it will co_await the child
    get_return_value(&scheduler, result).detach();

    // Run scheduler in background thread (run() is blocking)
    std::thread runner([&scheduler]() {
        scheduler.run();
    });
    runner.detach();

    // Wait for result with timeout
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (result.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    BOOST_CHECK_EQUAL(result.load(), MAGIC_NUMBER);

    // Note: scheduler.run() will keep running, but the test process will exit
    // For a real test framework, we'd need a way to stop the scheduler
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
    BOOST_REQUIRE_EQUAL(scheduler.init(16), YYASIO_OK);

    std::atomic<int> result{-1};
    std::atomic<long> elapsed_ns{0};
    long timeout_ns = 100'000'000; // 100ms

    timeout_coro(&scheduler, timeout_ns, result, elapsed_ns).detach();

    std::thread runner([&scheduler]() { scheduler.run(); });
    runner.detach();

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (result.load() == -1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // io_uring timeout returns -ETIME (-62) when timer expires
    BOOST_CHECK_EQUAL(result.load(), -ETIME);
    // Elapsed time should be at least 80% of the requested timeout
    BOOST_CHECK_GE(elapsed_ns.load(), timeout_ns * 8 / 10);
}

// ==================== CoroId Test ====================

Task<uint64_t> get_sub_coro_id_test(Scheduler* scheduler)
{
    uint64_t id = co_await current_coro_id();
    co_return id;
}

Task<void> coro_id_test_oro(Scheduler* scheduler, std::atomic<bool>& done,
    std::array<uint64_t, 2>& parent_ids,
    std::array<uint64_t, 2>& sub_ids)
{
    // Parent's coro_id should be the same on every call
    parent_ids[0] = co_await current_coro_id();
    parent_ids[1] = co_await current_coro_id();

    // Each sub-coroutine should have a different coro_id
    sub_ids[0] = co_await get_sub_coro_id_test(scheduler);
    sub_ids[1] = co_await get_sub_coro_id_test(scheduler);

    done.store(true);
}

BOOST_AUTO_TEST_CASE(test_coro_id)
{
    Scheduler scheduler;
    BOOST_REQUIRE_EQUAL(scheduler.init(16), YYASIO_OK);

    std::atomic<bool> done{false};
    std::array<uint64_t, 2> parent_ids{0, 0};
    std::array<uint64_t, 2> sub_ids{0, 0};

    coro_id_test_oro(&scheduler, done, parent_ids, sub_ids).detach();

    std::thread runner([&scheduler]() { scheduler.run(); });
    runner.detach();

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    BOOST_CHECK(done.load());
    // Parent's coro_id should be consistent across multiple calls
    BOOST_CHECK_EQUAL(parent_ids[0], parent_ids[1]);
    // Different sub-coroutines should have different coro_ids
    BOOST_CHECK_NE(sub_ids[0], sub_ids[1]);
    // Sub-coroutine's coro_id should differ from parent's
    BOOST_CHECK_NE(parent_ids[0], sub_ids[0]);
    BOOST_CHECK_NE(parent_ids[1], sub_ids[1]);
}

BOOST_AUTO_TEST_SUITE_END()
