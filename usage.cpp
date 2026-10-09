#include <cerrno>
#include <dirent.h>
#include <exception>
#include <iostream>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include "yyasio.h"

yyasio::Task<void> handle_client(yyasio::Scheduler* scheduler, int conn_fd)
{
    char buf[1024];
    while (true)
    {
        int bytes_read = co_await yyasio::read(scheduler, conn_fd, buf, 1024);
        if (bytes_read <= 0)
        {
            std::cout << "Connection closed, fd = " << conn_fd << ", ret =" << bytes_read << "\n";
            close(conn_fd);
            co_return;
        }
        std::cout << "Received data: " << bytes_read << "--" << std::string(buf, bytes_read) << "\n";
    }
}

yyasio::Task<void> cancel_after_3s(yyasio::Scheduler* scheduler, int fd)
{
    // after cancel called, async read will return with bytes_read = 0
    struct __kernel_timespec ts = { .tv_sec = 3, .tv_nsec = 0 };
    co_await yyasio::timeout(scheduler, &ts);
    std::cout << "Canceling fd: " << fd << "\n";
    co_await yyasio::cancel_fd(scheduler, fd);
}

yyasio::Task<void> accept_connections(yyasio::Scheduler* scheduler, int listen_fd)
{
    while (true)
    {
        struct sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        int conn_fd = co_await yyasio::accept(scheduler, listen_fd, reinterpret_cast<sockaddr*>(&addr), &len);

        std::string ip_str(INET_ADDRSTRLEN, 0);
        inet_ntop(AF_INET, &(addr.sin_addr), ip_str.data(), INET_ADDRSTRLEN);
        std::cout << "Accepted connection: " << conn_fd << ", from:" << ip_str << ":" << ntohs(addr.sin_port) << "\n";

        handle_client(scheduler, conn_fd).detach();
        cancel_after_3s(scheduler, conn_fd).detach(); // just demostrate how to cancel
    }
}

yyasio::Task<int> coro_with_return_val(yyasio::Scheduler* scheduler)
{
    std::cout << "enter coro with return val...\n";
    struct __kernel_timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
    co_await yyasio::timeout(scheduler, &ts);
    
    std::cout << "exit coro with return val, calling co_return...\n";
    co_return 100;
}

// Demo of Task::is_finished(): observe the completion of a fire-and-forget coroutine.
// The worker detaches itself, and the poller checks is_finished() between timeouts.
yyasio::Task<void> background_worker(yyasio::Scheduler* scheduler)
{
    struct __kernel_timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
    co_await yyasio::timeout(scheduler, &ts); // simulate some async work
    std::cout << "background_worker: work done\n";
}

yyasio::Task<void> is_finished_demo(yyasio::Scheduler* scheduler)
{
    auto worker = background_worker(scheduler);
    std::cout << "worker is_finished = " << worker.is_finished() << " (just created)\n";

    worker.detach(); // fire-and-forget, frame is owned by itself now

    struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 200'000'000 }; // 200ms
    while (!worker.is_finished())
    {
        std::cout << "worker is_finished = false, polling again after 200ms\n";
        co_await yyasio::timeout(scheduler, &ts);
    }
    std::cout << "worker is_finished = true\n";
}

yyasio::Task<void> timeout_trigger(yyasio::Scheduler* scheduler)
{
    std::cout << "start timeout trigger...\n";
    while (true)
    {
        struct __kernel_timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        std::cout << "Waiting for timeout...\n";
        int ret = co_await yyasio::timeout(scheduler, &ts);
        std::cout << "Timeout triggered, ret = " << ret << "\n";
        // scheduler->print_sched_stats();
        std::cout << "Waiting for new coroutine...\n";
        auto val = co_await coro_with_return_val(scheduler);
        std::cout << "Return value: " << val << "\n";
    }
}

yyasio::Task<void> sleep_and_report(yyasio::Scheduler* scheduler, const char* label, long long msecs)
{
    auto start = std::chrono::steady_clock::now();
    int ret = co_await yyasio::sleep(scheduler, std::chrono::milliseconds(msecs));
    long long elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << "  concurrent sleep(" << label << ") ret = " << ret
              << ", elapsed = " << elapsed_ms << " ms\n";
}

yyasio::Task<void> sleep_demo(yyasio::Scheduler* scheduler)
{
    std::cout << "=== Sleep Demo ===\n";

    // sleep() 直接接受 std::chrono::duration，并自己保管 timespec：调用方不需要像
    // timeout() 那样再准备一个活到操作完成的 struct __kernel_timespec。
    struct Case { const char* label; long long msecs; };
    const Case cases[] = {
        {"100ms", 100},
        {"1s", 1000},
        {"1500ms", 1500},
        {"-50ms (negative, clamped to 0)", -50},
    };

    for (const auto& c : cases)
    {
        auto start = std::chrono::steady_clock::now();
        int ret = co_await yyasio::sleep(scheduler, std::chrono::milliseconds(c.msecs));
        long long elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        // 定时器正常睡满时 io_uring 带回的是 -ETIME 而不是 0（与 timeout() 同一约定）
        std::cout << "sleep(" << c.label << ") ret = " << ret << " (-ETIME = " << -ETIME
                  << "), elapsed = " << elapsed_ms << " ms\n";
    }

    std::cout << "3 concurrent sleeps (50/120/190 ms), each awaiting its own duration:\n";
    sleep_and_report(scheduler, "50ms", 50).detach();
    sleep_and_report(scheduler, "120ms", 120).detach();
    sleep_and_report(scheduler, "190ms", 190).detach();
    // 自己也睡 300ms 让三个 fire-and-forget 协程有机会跑完（单线程循环里不要忙等）
    co_await yyasio::sleep(scheduler, std::chrono::milliseconds(300));

    std::cout << "=== Sleep Demo done ===\n";
}

// 带外任务完成后把计数加到 shared 计数器上，用于展示“多个任务并发”
yyasio::Task<void> rit_counted(yyasio::Scheduler* scheduler, int id, int sleep_ms,
                               std::shared_ptr<std::atomic<int>> finished)
{
    auto [rc, got] = co_await yyasio::run_in_thread<int>(scheduler, [id, sleep_ms] {
        ::usleep(static_cast<useconds_t>(sleep_ms) * 1000);   // 模拟一个必须阻塞的调用
        return id;
    });
    std::cout << "  background job id=" << got << " rc=" << rc << "\n";
    if (rc == 0 && got == id)
    {
        finished->fetch_add(1);
    }
}

yyasio::Task<void> run_in_thread_demo(yyasio::Scheduler* scheduler)
{
    std::cout << "=== Run In Thread Demo ===\n";

    // 1) 最基础用法：阻塞调用在普通线程上跑，结果由 co_await 带回来。
    //    rc 是调度层状态：0 = 已执行；-ECANCELED = 调度器没接受（此时 val 是默认值）。
    auto [rc1, text] = co_await yyasio::run_in_thread<std::string>(scheduler, [] {
        ::usleep(200 * 1000);
        return std::string("hello from a worker thread");
    });
    std::cout << "job1 rc = " << rc1 << ", value = \"" << text << "\"\n";

    // 2) 结果可以是 move-only 类型（T 只需默认可构造 + 可移动构造 + 可移动赋值）
    auto [rc2, ptr] = co_await yyasio::run_in_thread<std::unique_ptr<int>>(scheduler, [] {
        return std::make_unique<int>(42);
    });
    std::cout << "job2 rc = " << rc2 << ", *ptr = " << (ptr ? *ptr : -1) << "\n";

    // 3) 与 io_uring IO 并发：三个 200ms 的带外任务 + 本协程自己的 500ms 睡眠，
    //    彼此不阻塞（三个都在 ring 线程之外等待，ring 线程全程不碰它们）
    auto finished = std::make_shared<std::atomic<int>>(0);
    for (int i = 0; i < 3; i++)
    {
        rit_counted(scheduler, i, 200, finished).detach();
    }
    int io_ret = co_await yyasio::sleep(scheduler, std::chrono::milliseconds(500));
    std::cout << "job3 io sleep rc = " << io_ret << ", background jobs done = " << finished->load()
              << "/3 (两者并发，不是串行)\n";

    // 4) 这正是需要它的典型场景：io_uring 没有 getdents 操作，列目录只能阻塞调用，
    //    于是丢给带外线程，协程照常等它的结果。
    auto [rc4, entries] = co_await yyasio::run_in_thread<std::vector<std::string>>(scheduler, [] {
        std::vector<std::string> names;
        DIR* dir = opendir("/tmp");
        if (dir)
        {
            while (dirent* de = readdir(dir))
            {
                names.emplace_back(de->d_name);
            }
            closedir(dir);
        }
        return names;
    });
    std::cout << "job4 rc = " << rc4 << ", /tmp has " << entries.size() << " entries";
    if (!entries.empty())
    {
        std::cout << ", e.g. " << entries[0];
    }
    std::cout << "\n";

    std::cout << "=== Run In Thread Demo done ===\n";
}

yyasio::Task<void> rename_file_demo(yyasio::Scheduler* scheduler)
{
    std::cout << "=== Rename File Demo ===\n";
    
    // Create a test file
    const char* old_path = "/tmp/yyasio_rename_old.txt";
    const char* new_path = "/tmp/yyasio_rename_new.txt";
    
    int fd = co_await yyasio::openat(scheduler, AT_FDCWD, old_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        std::cerr << "Failed to create file: " << old_path << "\n";
        co_return;
    }
    
    const char* content = "test content for rename";
    co_await yyasio::write(scheduler, fd, content, strlen(content));
    co_await yyasio::close(scheduler, fd);
    std::cout << "Created file: " << old_path << "\n";
    
    // Rename the file
    int ret = co_await yyasio::renameat(scheduler, AT_FDCWD, old_path, AT_FDCWD, new_path);
    if (ret < 0)
    {
        std::cerr << "Rename failed, ret = " << ret << "\n";
        co_return;
    }
    std::cout << "Renamed " << old_path << " -> " << new_path << "\n";
    
    // Clean up
    unlink(new_path);
}

yyasio::Task<void> unlink_file_demo(yyasio::Scheduler* scheduler)
{
    std::cout << "=== Unlink File Demo ===\n";
    
    const char* file_path = "/tmp/yyasio_unlink_test.txt";
    
    // Create a test file
    int fd = co_await yyasio::openat(scheduler, AT_FDCWD, file_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        std::cerr << "Failed to create file: " << file_path << "\n";
        co_return;
    }
    
    const char* content = "test content for unlink";
    co_await yyasio::write(scheduler, fd, content, strlen(content));
    co_await yyasio::close(scheduler, fd);
    std::cout << "Created file: " << file_path << "\n";
    
    // Verify file exists
    struct stat st;
    if (stat(file_path, &st) == 0)
    {
        std::cout << "File exists, size = " << st.st_size << " bytes\n";
    }
    
    // Unlink the file via io_uring
    int ret = co_await yyasio::unlinkat(scheduler, AT_FDCWD, file_path, 0);
    if (ret < 0)
    {
        std::cerr << "Unlink failed, ret = " << ret << "\n";
        co_return;
    }
    std::cout << "Unlinked file: " << file_path << "\n";
    
    // Verify file no longer exists
    if (stat(file_path, &st) != 0)
    {
        std::cout << "Confirmed: file no longer exists\n";
    }
}

// Directory lifecycle through io_uring: mkdirat -> renameat -> unlinkat.
// Note there is no recursive remove: unlinkat(AT_REMOVEDIR) on a directory that
// still holds entries comes back with -ENOTEMPTY, so the caller has to empty it.
yyasio::Task<void> directory_ops_demo(yyasio::Scheduler* scheduler)
{
    std::cout << "=== Directory Ops Demo ===\n";

    const char* dir_path = "/tmp/yyasio_demo_dir";
    const char* renamed_dir_path = "/tmp/yyasio_demo_dir_renamed";
    const char* inner_file_path = "/tmp/yyasio_demo_dir/inner.txt";
    const char* renamed_inner_file_path = "/tmp/yyasio_demo_dir_renamed/inner.txt";

    // Drop whatever a previous run left behind
    unlink(inner_file_path);
    unlink(renamed_inner_file_path);
    rmdir(dir_path);
    rmdir(renamed_dir_path);

    int ret = co_await yyasio::mkdirat(scheduler, AT_FDCWD, dir_path, 0755);
    if (ret < 0)
    {
        std::cerr << "mkdir failed, ret = " << ret << "\n";
        co_return;
    }
    struct stat st;
    if (stat(dir_path, &st) == 0)
    {
        std::cout << "Created directory: " << dir_path << ", is_dir = " << S_ISDIR(st.st_mode)
                  << ", mode = " << std::oct << (st.st_mode & 0777) << std::dec << "\n";
    }

    // Put a file inside, so the directory is no longer empty
    int fd = co_await yyasio::openat(scheduler, AT_FDCWD, inner_file_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        std::cerr << "Failed to create file: " << inner_file_path << "\n";
        co_return;
    }
    const char* content = "content inside the directory";
    co_await yyasio::write(scheduler, fd, content, strlen(content));
    co_await yyasio::close(scheduler, fd);

    // Rename the directory - the entry inside moves along with it
    ret = co_await yyasio::renameat(scheduler, AT_FDCWD, dir_path, AT_FDCWD, renamed_dir_path);
    if (ret < 0)
    {
        std::cerr << "Rename directory failed, ret = " << ret << "\n";
        co_return;
    }
    std::cout << "Renamed " << dir_path << " -> " << renamed_dir_path << "\n";
    if (stat(renamed_inner_file_path, &st) == 0)
    {
        std::cout << "Inner file moved with the directory, size = " << st.st_size << " bytes\n";
    }

    // Removing a non-empty directory is refused
    ret = co_await yyasio::unlinkat(scheduler, AT_FDCWD, renamed_dir_path, AT_REMOVEDIR);
    std::cout << "unlinkat(AT_REMOVEDIR) on non-empty dir: ret = " << ret
              << " (-ENOTEMPTY = " << -ENOTEMPTY << ")\n";

    // Empty it first, then the same call succeeds
    ret = co_await yyasio::unlinkat(scheduler, AT_FDCWD, renamed_inner_file_path, 0);
    if (ret < 0)
    {
        std::cerr << "Failed to unlink inner file, ret = " << ret << "\n";
        co_return;
    }
    ret = co_await yyasio::unlinkat(scheduler, AT_FDCWD, renamed_dir_path, AT_REMOVEDIR);
    if (ret < 0)
    {
        std::cerr << "Failed to remove empty directory, ret = " << ret << "\n";
        co_return;
    }
    std::cout << "Removed empty directory: " << renamed_dir_path << "\n";
}

yyasio::Task<void> sync_file_range_demo(yyasio::Scheduler* scheduler)
{
    std::cout << "=== Sync File Range Demo ===\n";

    const char* file_path = "/tmp/yyasio_sync_test.txt";

    // Create a test file and write some data into the page cache
    int fd = co_await yyasio::openat(scheduler, AT_FDCWD, file_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        std::cerr << "Failed to create file: " << file_path << "\n";
        co_return;
    }

    const char* content = "test content for sync_file_range";
    co_await yyasio::write(scheduler, fd, content, strlen(content));

    // Start writeback of the dirty range without waiting for completion
    int ret = co_await yyasio::sync_file_range(scheduler, fd, 0, strlen(content), SYNC_FILE_RANGE_WRITE);
    if (ret < 0)
    {
        std::cerr << "sync_file_range failed, ret = " << ret << "\n";
        co_await yyasio::close(scheduler, fd);
        co_return;
    }
    std::cout << "sync_file_range(SYNC_FILE_RANGE_WRITE) ok\n";

    // Flush and wait until the data is stable on storage
    ret = co_await yyasio::sync_file_range(scheduler, fd, 0, strlen(content),
                                           SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER);
    if (ret < 0)
    {
        std::cerr << "sync_file_range(wait) failed, ret = " << ret << "\n";
    }
    else
    {
        std::cout << "sync_file_range(WAIT_BEFORE|WRITE|WAIT_AFTER) ok, data flushed\n";
    }

    co_await yyasio::close(scheduler, fd);
    unlink(file_path);
}

// statx() through io_uring (kernel >= 5.6): the metadata is written into the caller's
// struct statx, while await_resume() gives back the statx(2) return value itself --
// 0 on success, -errno otherwise. struct statx and the AT_/STATX_ constants come from
// <sys/stat.h>, which the caller has to include; yyasio.h only forward declares the
// struct because the wrapper merely carries the pointer.
yyasio::Task<void> statx_demo(yyasio::Scheduler* scheduler)
{
    std::cout << "=== Statx Demo ===\n";

    const char* file_path = "/tmp/yyasio_statx_demo.txt";
    const char* link_path = "/tmp/yyasio_statx_demo_link";

    int fd = co_await yyasio::openat(scheduler, AT_FDCWD, file_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        std::cerr << "Failed to create file: " << file_path << "\n";
        co_return;
    }
    const char* content = "some bytes for statx";
    co_await yyasio::write(scheduler, fd, content, strlen(content));
    co_await yyasio::close(scheduler, fd);

    // The buffer is captured by pointer, so it has to outlive the completion: keeping
    // it as a local of this coroutine is enough, the frame stays alive while suspended.
    struct statx stx {};
    int ret = co_await yyasio::statx(scheduler, AT_FDCWD, file_path, &stx);
    if (ret < 0)
    {
        std::cerr << "statx failed, ret = " << ret << "\n";
        unlink(file_path);
        co_return;
    }
    std::cout << file_path << ": size = " << stx.stx_size
              << ", mode = " << std::oct << (stx.stx_mode & 0777) << std::dec
              << ", nlink = " << stx.stx_nlink
              << ", is_reg = " << S_ISREG(stx.stx_mode) << "\n";

    // flags = AT_SYMLINK_NOFOLLOW reports the link itself instead of its target
    if (symlink(file_path, link_path) == 0)
    {
        struct statx target {};
        co_await yyasio::statx(scheduler, AT_FDCWD, link_path, &target);
        struct statx link_self {};
        co_await yyasio::statx(scheduler, AT_FDCWD, link_path, &link_self, AT_SYMLINK_NOFOLLOW);
        std::cout << "via symlink: target size = " << target.stx_size
                  << ", link itself: is_link = " << S_ISLNK(link_self.stx_mode)
                  << ", size = " << link_self.stx_size << " (length of the stored path)\n";
        unlink(link_path);
    }

    // Same file addressed as dir_fd + relative name. A dfd that gets ignored on the way
    // into the SQE would resolve against the working directory instead.
    int dir_fd = co_await yyasio::openat(scheduler, AT_FDCWD, "/tmp", O_RDONLY | O_DIRECTORY);
    if (dir_fd >= 0)
    {
        struct statx relative {};
        ret = co_await yyasio::statx(scheduler, dir_fd, "yyasio_statx_demo.txt", &relative);
        std::cout << "through dir_fd + relative name: ret = " << ret
                  << ", size = " << relative.stx_size
                  << ", ino matches = " << (relative.stx_ino == stx.stx_ino) << "\n";
        co_await yyasio::close(scheduler, dir_fd);
    }

    // Missing path: the error arrives as -errno, no exception involved
    struct statx ghost {};
    ret = co_await yyasio::statx(scheduler, AT_FDCWD, "/tmp/yyasio_statx_demo_missing", &ghost);
    std::cout << "statx on a missing path: ret = " << ret << " (-ENOENT = " << -ENOENT << ")\n";

    unlink(file_path);
}

yyasio::Task<void> visit_regular_file(yyasio::Scheduler* scheduler)
{
    int dir_fd = co_await yyasio::openat(scheduler, AT_FDCWD, "/tmp", O_RDONLY | O_DIRECTORY);
    while (true)
    {
        struct __kernel_timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        co_await yyasio::timeout(scheduler, &ts);
        int fd = co_await yyasio::openat(scheduler, dir_fd, "test.txt", O_RDWR | O_CREAT, 0644);
        if (fd < 0)
        {
            std::cerr << "Failed to open file";
            continue;
        }
        char buf[1024];
        int bytes_read = co_await yyasio::read(scheduler, fd, buf, 1024);
        std::cout << "Read " << bytes_read << " bytes from file: " << std::string(buf, bytes_read) << "\n";
        std::string new_data = "this is new data";
        off_t offset = lseek(fd, 0, SEEK_END);
        std::cout << "Offset: " << offset << std::endl;
        int bytes_write = co_await yyasio::write(scheduler, fd, new_data.data(), new_data.size(), offset);
        std::cout << "Wrote " << bytes_write << " bytes to file: " << new_data << "\n";
        co_await yyasio::close(scheduler, fd);
    }
}

yyasio::Task<void> run_simple_server(yyasio::Scheduler* scheduler, int listen_port)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listen_fd < 0)
    {
        std::cerr << "Create socket failed\n";
        co_return;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(listen_port);

    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        std::cerr << "Bind failed\n";
        close(listen_fd);
        co_return;
    }

    int ret = co_await yyasio::listen(scheduler, listen_fd);
    if (ret < 0)
    {
        std::cerr << "Listen on port " << listen_port << " failed, ret = " << ret << "\n";
        close(listen_fd);
        co_return;
    }
    std::cout << "Server listening on port " << listen_port << ", fd = " << listen_fd << "\n";

    co_await accept_connections(scheduler, listen_fd);
}

yyasio::Task<uint64_t> get_sub_coro_id(yyasio::Scheduler* scheduler)
{
    std::cout << "enter sub coro\n";
    uint64_t coro_id = co_await yyasio::current_coro_id();
    std::cout << "sub coro id: " << coro_id << "\n";
    co_return coro_id;
}

yyasio::Task<void> get_coro_id(yyasio::Scheduler* scheduler)
{
    while (true)
    {
        uint64_t coro_id = co_await yyasio::current_coro_id();
        std::cout << "Parent coro ID: " << coro_id << "\n";
        uint64_t sub_coro_id = co_await get_sub_coro_id(scheduler);
        std::cout << "Sub coro ID: " << sub_coro_id << "\n";
        struct __kernel_timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        co_await yyasio::timeout(scheduler, &ts);
    }
}

yyasio::Task<void> infinite_loop(yyasio::Event<int>& event)
{
    while (true)
    {
        std::cout << "Wait for event \n";
        int val = co_await event.wait();
        std::cout << "Event triggered with value: " << val << "\n";
    }
}

yyasio::Task<void> tick_trigger(yyasio::Scheduler* scheduler, yyasio::Event<int>& event)
{
    int val = 0;
    while (true)
    {
        struct __kernel_timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        std::cout << "Wait for 1 second\n";
        co_await yyasio::timeout(scheduler, &ts);
        std::cout << "Call resume\n";
        event.set(val++);
    }
}

yyasio::Task<void> connect_and_send(yyasio::Scheduler* scheduler)
{
    // wait for server to start
    __kernel_timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
    co_await yyasio::timeout(scheduler, &ts);

    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
    {
        std::cerr << "Create socket failed\n";
        co_return;
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    std::cout << "Connecting to 127.0.0.1:8080...\n";
    int ret = co_await yyasio::connect(scheduler, fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (ret < 0)
    {
        std::cerr << "Connect failed, ret = " << ret << "\n";
        close(fd);
        co_return;
    }
    std::cout << "Connected successfully, fd = " << fd << "\n";

    std::string msg = "hello from connect_and_send";
    int bytes_written = co_await yyasio::write(scheduler, fd, msg.data(), msg.size(), 0);
    std::cout << "Sent " << bytes_written << " bytes: " << msg << "\n";

    close(fd);
}

// Two coroutines share one counter. The critical section spans a co_await
// (a 200ms timeout), so without the mutex the read-modify-write would be
// interleaved; with it, each increment runs atomically w.r.t. the other coro.
yyasio::Task<void> mutex_writer(yyasio::Scheduler* scheduler, yyasio::Mutex& mtx, int* counter, int id,
                                std::atomic<int>& finished)
{
    co_await mtx.lock();
    std::cout << "writer " << id << ": entered critical section, counter = " << *counter << "\n";
    struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 200'000'000 }; // hold across an await point
    co_await yyasio::timeout(scheduler, &ts);
    *counter = *counter + 1;
    std::cout << "writer " << id << ": incremented to " << *counter << ", unlocking\n";
    mtx.unlock();
    finished.fetch_add(1);
}

yyasio::Task<void> mutex_demo(yyasio::Scheduler* scheduler)
{
    yyasio::Mutex mtx;
    int counter = 0;
    std::atomic<int> finished{0};
    mutex_writer(scheduler, mtx, &counter, 1, finished).detach();
    mutex_writer(scheduler, mtx, &counter, 2, finished).detach();
    // writer 2 is suspended in mtx.lock() until writer 1 calls unlock()

    // mtx and counter live in this coroutine's frame; wait until both writers
    // are done before the frame is destroyed, so the waiters' references stay valid
    struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 50'000'000 }; // 50ms
    while (finished.load() < 2)
    {
        co_await yyasio::timeout(scheduler, &ts);
    }
    std::cout << "mutex_demo: final counter = " << counter << " (expected 2)\n";
}

// Same shared-counter pattern, but the critical section is delimited by an
// RAII guard: co_await mtx.lock_guard() returns a LockGuard whose destructor
// unlocks at scope exit, so there is no manual unlock() to forget.
yyasio::Task<void> guarded_writer(yyasio::Scheduler* scheduler, yyasio::Mutex& mtx, int* counter, int id,
                                  std::atomic<int>& finished)
{
    auto guard = co_await mtx.lock_guard();
    std::cout << "guarded_writer " << id << ": locked via RAII, counter = " << *counter << "\n";
    struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 150'000'000 }; // hold across an await point
    co_await yyasio::timeout(scheduler, &ts);
    *counter = *counter + 1;
    std::cout << "guarded_writer " << id << ": incremented to " << *counter << ", leaving scope\n";
    // guard destructs here -> unlock()
    finished.fetch_add(1);
}

yyasio::Task<void> lock_guard_demo(yyasio::Scheduler* scheduler)
{
    yyasio::Mutex mtx;
    int counter = 0;
    std::atomic<int> finished{0};
    guarded_writer(scheduler, mtx, &counter, 1, finished).detach();
    guarded_writer(scheduler, mtx, &counter, 2, finished).detach();

    struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 50'000'000 }; // 50ms
    while (finished.load() < 2)
    {
        co_await yyasio::timeout(scheduler, &ts);
    }
    std::cout << "lock_guard_demo: final counter = " << counter << " (expected 2)\n";
}

// Demo of TimeoutEvent: 一个可等待的定时器，「超时」和「被 set 唤醒」谁先到都行。
yyasio::Task<void> timeout_event_worker(yyasio::Scheduler* scheduler, yyasio::TimeoutEvent& ev,
                                        std::atomic<int>& rounds)
{
    constexpr auto kTimeout = std::chrono::milliseconds(500);
    for (int i = 0; i < 4; ++i)
    {
        auto start = std::chrono::steady_clock::now();
        std::cout << "timeout_event demo: round " << i << ", waiting up to 500ms\n";
        co_await ev.wait_until(kTimeout);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start).count();
        // 没到 500ms 就是被 set() 提前叫起的（那一轮的定时器已被 abandon）
        std::cout << "timeout_event demo: round " << i << " woke after " << elapsed
                  << " ms (" << (elapsed < 500 ? "set() won" : "timeout won") << ")\n";
        rounds.fetch_add(1);
    }
    std::cout << "timeout_event demo: worker done\n";
}

// 每 200ms set 一次，共 3 次；worker 的最后一轮（第 4 轮）没人 set，走自然超时。
// 单线程调度下 worker 被唤醒后会在同一条调用链里重新 co_await，所以每次 set 都能
// 命中一个已经挂好的 waiter，时序是确定的。
yyasio::Task<void> timeout_event_setter(yyasio::Scheduler* scheduler, yyasio::TimeoutEvent& ev)
{
    for (int i = 0; i < 3; ++i)
    {
        struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 200'000'000 }; // 200ms
        co_await yyasio::timeout(scheduler, &ts);
        std::cout << "timeout_event demo: calling set() #" << i << "\n";
        ev.set();
    }
}

yyasio::Task<void> timeout_event_demo(yyasio::Scheduler* scheduler)
{
    yyasio::TimeoutEvent ev(scheduler);
    std::atomic<int> rounds{0};

    timeout_event_worker(scheduler, ev, rounds).detach();
    timeout_event_setter(scheduler, ev).detach();

    // ev 活在本协程帧里，必须等两个 detached 协程都结束才能销毁，
    // 否则它们持有的 TimeoutEvent 引用/正在飞的定时器 CQE 会落在已释放的帧上。
    struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 100'000'000 }; // 100ms
    while (rounds.load() < 4)
    {
        co_await yyasio::timeout(scheduler, &ts);
    }
    std::cout << "timeout_event_demo: final rounds = " << rounds.load() << " (expected 4)\n";
}

int main()
{
    yyasio::Scheduler scheduler;
    auto ret = scheduler.init(256);
    if (ret != yyasio::YYASIO_OK)
    {
        std::cerr << "Init scheduler failed\n";
        std::terminate();
    }

    run_simple_server(&scheduler, 8080).detach();

    timeout_trigger(&scheduler).detach();

    sleep_demo(&scheduler).detach();

    run_in_thread_demo(&scheduler).detach();

    visit_regular_file(&scheduler).detach();

    get_coro_id(&scheduler).detach();

    yyasio::Event<int> event;
    infinite_loop(event).detach();
    tick_trigger(&scheduler, event).detach();

    connect_and_send(&scheduler).detach();

    rename_file_demo(&scheduler).detach();

    unlink_file_demo(&scheduler).detach();

    directory_ops_demo(&scheduler).detach();

    sync_file_range_demo(&scheduler).detach();

    statx_demo(&scheduler).detach();

    mutex_demo(&scheduler).detach();

    lock_guard_demo(&scheduler).detach();

    is_finished_demo(&scheduler).detach();

    timeout_event_demo(&scheduler).detach();

    // will block here
    scheduler.run();

    // run() returns after stop() is handled (or on a fatal submit error) and has already
    // released the ring and the control pipe on this thread, so no extra cleanup call here.
}
