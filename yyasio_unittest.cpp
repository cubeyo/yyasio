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
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

using namespace yyasio;

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
        BOOST_REQUIRE_EQUAL(scheduler.init(16), YYASIO_OK);
        scheduler.run();
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
        BOOST_REQUIRE_EQUAL(scheduler.init(16), YYASIO_OK);
        scheduler.run();
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
        BOOST_REQUIRE_EQUAL(scheduler.init(16), YYASIO_OK);
        scheduler.run();
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
        BOOST_REQUIRE_EQUAL(scheduler.init(16), YYASIO_OK);
        scheduler.run();
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
        BOOST_REQUIRE_EQUAL(scheduler.init(16), YYASIO_OK);
        scheduler.run();
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
        BOOST_REQUIRE_EQUAL(scheduler.init(16), YYASIO_OK);
        scheduler.run();
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
        BOOST_REQUIRE_EQUAL(scheduler.init(16), YYASIO_OK);
        scheduler.run();
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
        BOOST_REQUIRE_EQUAL(scheduler.init(16), YYASIO_OK);
        scheduler.run();
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
        BOOST_REQUIRE_EQUAL(scheduler.init(16), YYASIO_OK);
        scheduler.run();
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
        BOOST_REQUIRE_EQUAL(scheduler.init(16), YYASIO_OK);
        scheduler.run();
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
        BOOST_REQUIRE_EQUAL(scheduler.init(16), YYASIO_OK);
        scheduler.run();
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
        BOOST_REQUIRE_EQUAL(scheduler.init(16), YYASIO_OK);
        scheduler.run();
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
        BOOST_REQUIRE_EQUAL(scheduler.init(16), YYASIO_OK);
        scheduler.run();
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
        BOOST_REQUIRE_EQUAL(scheduler.init(16), YYASIO_OK);
        scheduler.run();
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
        BOOST_REQUIRE_EQUAL(scheduler.init(16), YYASIO_OK);
        scheduler.run();
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
        BOOST_REQUIRE_EQUAL(scheduler.init(16), YYASIO_OK);
        scheduler.run();
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
        BOOST_REQUIRE_EQUAL(scheduler.init(16), YYASIO_OK);
        scheduler.run();
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
        BOOST_REQUIRE_EQUAL(scheduler.init(16), YYASIO_OK);
        scheduler.run();
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
        BOOST_REQUIRE_EQUAL(scheduler.init(16), YYASIO_OK);
        scheduler.run();
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

BOOST_AUTO_TEST_SUITE_END()
