#include <cerrno>
#include <exception>
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
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

yyasio::Task<void> infinite_loop(yyasio::Promise<int>& promise)
{
    while (true)
    {
        std::cout << "Wait for promise \n";
        int val = co_await promise.wait();
        std::cout << "Promise triggered with value: " << val << "\n";
    }
}

yyasio::Task<void> tick_trigger(yyasio::Scheduler* scheduler, yyasio::Promise<int>& promise)
{
    int val = 0;
    while (true)
    {
        struct __kernel_timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        std::cout << "Wait for 1 second\n";
        co_await yyasio::timeout(scheduler, &ts);
        std::cout << "Call resume\n";
        promise.resume(val++);
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

    visit_regular_file(&scheduler).detach();

    get_coro_id(&scheduler).detach();

    yyasio::Promise<int> promise;
    infinite_loop(promise).detach();
    tick_trigger(&scheduler, promise).detach();

    connect_and_send(&scheduler).detach();

    // will block here
    scheduler.run();
}
