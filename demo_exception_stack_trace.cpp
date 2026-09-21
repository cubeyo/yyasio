#include <iostream>
#include <assert.h>

#define PRINT_STACK_ON_EXCEPTION 1
#include "yyasio.h"

using namespace yyasio;

// Coroutine c: throws an exception
Task<void> coro_c(Scheduler* scheduler)
{
    std::cout << "coro_c: about to throw...\n";
    struct __kernel_timespec ts = {0, 1000000};
    co_await yyasio::timeout(scheduler, &ts);
    throw std::runtime_error("exception from coro_c");
}

// Coroutine b: calls coro_c
Task<void> coro_b(Scheduler* scheduler)
{
    std::cout << "coro_b: calling coro_c...\n";
    struct __kernel_timespec ts = {0, 1000000};
    co_await yyasio::timeout(scheduler, &ts);
    co_await coro_c(scheduler);
    std::cout << "coro_b: coro_c finished (should not reach here)\n";
}

// Coroutine a: calls coro_b
Task<void> coro_a(Scheduler* scheduler)
{
    std::cout << "coro_a: calling coro_b...\n";
    struct __kernel_timespec ts = {0, 1000000};
    co_await yyasio::timeout(scheduler, &ts);
    co_await coro_b(scheduler);
    std::cout << "coro_a: coro_b finished (should not reach here)\n";
}

int main()
{
    std::cout << "=== Exception stack trace demo ===\n";
    std::cout << "Chain: coro_a -> coro_b -> coro_c (throws)\n\n";

    // Fire-and-forget: coro_a will run synchronously until it hits
    // the co_await chain, then coro_c throws. The exception propagates
    // through unhandled_exception() which prints the hardware stack trace.
    Scheduler scheduler;
    auto ret = scheduler.init(256);
    assert(ret == yyasio::YYASIO_OK);
    coro_a(&scheduler).detach();
    scheduler.run();

    std::cout << "\n=== Should not reach here ===\n";
    return 0;
}
