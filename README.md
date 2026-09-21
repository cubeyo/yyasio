# yyasio
This is a header only library providing simple wrap for lib uring and supports C++20 coroutine.  
Get usage from usage.cpp.  
Build usage with command g++ usage.cpp -o yyasio_usage -luring -std=c++20.  

# prerequisites
GCC 10+  
Linux 5.11+  

# macros at compile time
```cpp
/** set 1 to enable display stacktrace to stderr 
 *  when unhandled exception is caught */
#define PRINT_STACK_ON_EXCEPTION 1

/** set 1 to enable display coroutine lifetime info,
 *  including: create/switch/destroy */
#define PRINT_CORO_RUNTIMEINFO 1
```