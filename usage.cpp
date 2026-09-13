#include <cerrno>
#include <exception>
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "yyasio.h"

yyasio::Task handle_client(yyasio::Scheduler* scheduler, int conn_fd)
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

yyasio::Task cancel_after_3s(yyasio::Scheduler* scheduler, int fd)
{
    // after cancel called, async read will return with bytes_read = 0
    co_await yyasio::timeout(scheduler, { .tv_sec = 3, .tv_nsec = 0 });
    std::cout << "Canceling fd: " << fd << "\n";
    co_await yyasio::cancel_fd(scheduler, fd);
}

yyasio::Task accept_connections(yyasio::Scheduler* scheduler, int listen_fd)
{
    while (true)
    {
        struct sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        int conn_fd = co_await yyasio::accept(scheduler, listen_fd, reinterpret_cast<sockaddr*>(&addr), &len);

        std::string ip_str(INET_ADDRSTRLEN, 0);
        inet_ntop(AF_INET, &(addr.sin_addr), ip_str.data(), INET_ADDRSTRLEN);
        std::cout << "Accepted connection: " << conn_fd << ", from:" << ip_str << ":" << ntohs(addr.sin_port) << "\n";

        handle_client(scheduler, conn_fd);
        cancel_after_3s(scheduler, conn_fd); // just demostrate how to cancel
    }
}

yyasio::Task timeout_trigger(yyasio::Scheduler* scheduler)
{
    while (true)
    {
        int ret = co_await yyasio::timeout(scheduler, { .tv_sec = 1, .tv_nsec = 0 });
        std::cout << "Timeout triggered, ret = " << ret << "\n";
        scheduler->print_sched_stats();
    }
}

yyasio::Task visit_regular_file(yyasio::Scheduler* scheduler)
{
    int dir_fd = co_await yyasio::openat(scheduler, AT_FDCWD, "/tmp", O_RDONLY | O_DIRECTORY);
    while (true)
    {
        co_await yyasio::timeout(scheduler, { .tv_sec = 1, .tv_nsec = 0 });
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

yyasio::Task run_simple_server(yyasio::Scheduler* scheduler, int listen_port)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8080);

    bind(listen_fd, (sockaddr*)&addr, sizeof(addr));
    listen(listen_fd, SOMAXCONN);

    return accept_connections(scheduler, listen_fd);
}

yyasio::Task infinite_loop(yyasio::Promise<int>& promise)
{
    while (true)
    {
        std::cout << "Wait for promise \n";
        int val = co_await promise.wait();
        std::cout << "Promise triggered with value: " << val << "\n";
    }
}

yyasio::Task tick_trigger(yyasio::Scheduler* scheduler, yyasio::Promise<int>& promise)
{
    int val = 0;
    while (true)
    {
        co_await yyasio::timeout(scheduler, { .tv_sec = 1, .tv_nsec = 0 });
        promise.resume(val++);
    }
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

    run_simple_server(&scheduler, 8080);

    timeout_trigger(&scheduler);

    visit_regular_file(&scheduler);

    yyasio::Promise<int> promise;
    infinite_loop(promise);
    tick_trigger(&scheduler, promise);

    // will block here
    scheduler.run();
}
