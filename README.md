# yyasio

一个 **header-only** 的 C++20 异步 I/O 库，为 Linux `io_uring`（通过 `liburing`）提供薄封装，并内置基于 C++20 协程（coroutine）的 `Task<T>` 抽象，让你用同步风格的代码写异步逻辑。

使用时只需 `#include "yyasio.h"`，无需单独编译库文件。

## 特性

- **Header-only**：单个头文件 `yyasio.h`，拷贝即用。
- **io_uring 封装**：`read` / `write` / `sync_file_range` / `openat` / `close` / `accept` / `connect` / `listen` / `timeout` / `renameat` / `unlinkat` / `cancel_fd` 等常用操作的异步 awaiter。
- **C++20 协程支持**：`Task<T>` / `Task<void>`，支持 `co_await` 组合与 `detach()` 的 fire-and-forget 用法。
- **单线程事件循环**：`Scheduler` 以单线程驱动 io_uring，模型简单、无锁竞争。
- **协程调试能力**（编译期开关）：异常时打印硬件栈回溯与协程调用链、协程生命周期日志。
- **Event 原语**：`Event<T>` 支持在一个协程中等待、在另一个协程中唤醒。
- **Mutex 原语**：协程互斥锁，支持临界区跨 `co_await` 点挂起，等待者按 FIFO 唤醒。

## 环境要求

| 项目 | 要求 | 说明 |
|---|---|---|
| 操作系统 | Linux 内核 **5.11+** | io_uring 基础操作 |
| 内核 5.19+ | `cancel_fd` | 异步取消（`IORING_ASYNC_CANCEL_FD`） |
| 内核 **6.0+** | 可选优化 | `IORING_SETUP_SINGLE_ISSUER` / `IORING_SETUP_DEFER_TASKRUN`；低版本内核会自动回退到无 flag 模式 |
| 编译器 | GCC 10+（建议 GCC 11+） | 需支持 C++20 `std::coroutine`；GCC 11 不支持 requires 子句过滤成员函数，代码已做兼容 |
| 依赖库 | `liburing` | 异步 I/O 后端 |
| 依赖库 | `libunwind` | 仅在开启栈回溯宏时需要 |

## 构建方式

### 直接编译示例程序

> 注意：示例/测试程序除了 `-luring` 还需要链接 `unwind` 与 `unwind-x86_64`（libunwind 1.3.2+ 的架构相关符号在独立 so 中）。若纯粹在自己的工程里 include `yyasio.h` 且不开启栈回溯宏，只需 `-luring`。

```bash
g++ usage.cpp -o yyasio_usage -std=c++20 -luring -lunwind -lunwind-x86_64
./yyasio_usage
```

### 使用 CMake（推荐）

仓库根目录已提供 `CMakeLists.txt`，默认构建 `usage`、`demo_exception_stack_trace` 两个目标，并为所有目标启用 ASan/UBSan：

```bash
# 配置 + 构建全部目标
cmake -S . -B . -DBUILD_TEST=ON
cmake --build .

# 仅构建单元测试
cmake --build . --target yyasio_unittest
```

其中 `BUILD_TEST=ON` 会额外构建基于 Boost.Test 的单元测试目标 `yyasio_unittest`（需要系统安装 Boost）。

### 链接依赖速查

| 目标 | 链接库 |
|---|---|
| 你自己的程序（不开调试宏） | `uring` |
| 你自己的程序（开 `PRINT_STACK_ON_EXCEPTION`） | `uring` `unwind` `unwind-x86_64` |
| `usage` / demo / unittest | `uring` `unwind` `unwind-x86_64`（unittest 另需 `Boost::unit_test_framework`） |

## 使用方式

### 快速上手

```cpp
#include "yyasio.h"

// 1. 定义协程：返回 Task<T> 或 Task<void>，用 co_await 挂起等待 IO
yyasio::Task<int> read_file(yyasio::Scheduler* scheduler)
{
    int fd = co_await yyasio::openat(scheduler, AT_FDCWD, "/tmp/a.txt", O_RDONLY);
    char buf[1024];
    int n = co_await yyasio::read(scheduler, fd, buf, sizeof(buf));
    co_await yyasio::close(scheduler, fd);
    co_return n;
}

int main()
{
    // 2. 初始化 Scheduler（init 与 run 必须在同一线程，见「使用限制」）
    yyasio::Scheduler scheduler;
    if (scheduler.init(256) != yyasio::YYASIO_OK) return 1;

    // 3. 启动协程：detach() 表示 fire-and-forget
    read_file(&scheduler).detach();

    // 4. 进入事件循环（阻塞，直到 stop()）
    scheduler.run();
}
```

完整示例见 [`usage.cpp`](./usage.cpp)（含回声服务器、文件读写、超时轮询、协程 ID 获取等），协程与 API 的单元测试见 [`yyasio_unittest.cpp`](./yyasio_unittest.cpp)。

### 协程组合与生命周期

```cpp
// co_await 一个子 Task，拿到返回值（子协程帧在 await_resume 中被销毁）
int val = co_await child_task;

// detach()：fire-and-forget，父协程不关心结果，帧由协程自身管理
background_task.detach();

// is_finished()：配合 detach() 观察 fire-and-forget 协程是否已执行完毕
auto t = background_task(/*...*/);
t.detach();
while (!t.is_finished()) { /* 用 timeout 让出，避免忙等 */ }
```

`Task<T>` 采用**自持有帧**模式：帧在 `FinalAwaiter::await_suspend` 中自行销毁，`~Task()` 是 no-op，因此既支持 `co_await` 也支持 `detach()`。

### Event 原语

```cpp
yyasio::Event<int> ev;

yyasio::Task<void> waiter(yyasio::Event<int>& e) {
    int v = co_await e.wait();   // 挂起，直到被 set 唤醒
}
yyasio::Task<void> setter(yyasio::Event<int>& e) {
    e.set(42);                    // 唤醒等待中的协程
}
```

### Mutex 原语

保护跨 `co_await` 点的临界区（普通 `std::mutex` 无法跨越挂起点）：

```cpp
yyasio::Mutex mtx;

co_await mtx.lock();   // 锁空闲则立即返回；否则挂起
// ... 临界区，可以 co_await 其他异步操作 ...
mtx.unlock(); // 排在队首的等待者先获得锁

if (mtx.try_lock()) { ... mtx.unlock(); }  // 非阻塞尝试加锁
```

优先用 RAII 守卫 `lock_guard()`，避免在临界区里忘了 `unlock()`（或中途 `co_return`/异常路径漏解）：

```cpp
{
    auto guard = co_await mtx.lock_guard(); // RAII
    // ... 临界区，可调用其他协程或 co_await  ...
}
```

采用**所有权转移** + FIFO 等待队列：`unlock()` 不会把锁交给尚未排队的 `lock()` 调用者（防插队）。注意与 `Scheduler` 一致，`Mutex` **非线程安全**，共享同一把锁的协程必须跑在同一个调度器（同一线程）上；另外锁若在协程帧中声明，需保证帧存活期覆盖所有等待者，见 [`usage.cpp`](./usage.cpp) 的 `mutex_demo` / `lock_guard_demo`。

### 可用异步 API

均为返回 awaiter 的自由函数，`co_await` 后得到 `int` 结果（io_uring 风格：成功为返回值，失败为 `-errno`）：

| 函数 | 作用 |
|---|---|
| `openat(sched, dirfd, path, flags, mode)` | 打开文件 |
| `read(sched, fd, buf, size, offset=0)` | 读 |
| `write(sched, fd, buf, size, offset=0)` | 写 |
| `sync_file_range(sched, fd, offset, len, flags)` | 同步文件区间到存储，`flags` 为 `SYNC_FILE_RANGE_WRITE` / `SYNC_FILE_RANGE_WAIT_BEFORE` / `SYNC_FILE_RANGE_WAIT_AFTER` 的组合（需 `<fcntl.h>`） |
| `close(sched, fd)` | 关闭 fd |
| `accept(sched, listen_fd, addr, len, flags)` | 接受连接 |
| `connect(sched, fd, addr, len)` | 发起连接 |
| `listen(sched, fd, backlog=SOMAXCONN)` | 监听（io_uring 原生支持） |
| `timeout(sched, ts, count=0, flags=0)` | 定时器 |
| `renameat(sched, olddirfd, old, newdirfd, new, flags=0)` | 重命名 |
| `unlinkat(sched, dirfd, path, flags=0)` | 删除 |
| `cancel_fd(sched, fd, flags=0)` | 取消某 fd 上的在途请求 |
| `mutex.[un]lock()` | 协程互斥锁加锁（不可用则挂起，FIFO 唤醒） |
| `guard = co_await mutex.lock_guard()` | RAII 加锁，返回 `LockGuard`，离开作用域自动解锁 |
| `mutex.unlock()` / `mutex.try_lock()` | 解锁（转移所有权给队首等待者）/ 非阻塞尝试加锁 |
| `current_coro_id()` | 取当前协程地址作为 ID |

### 编译期宏

在 `#include "yyasio.h"` **之前**定义以下宏（值设为 1 开启）：

```cpp
/** 置 1：协程捕获到未处理异常时，向 stderr 打印硬件栈回溯 + 协程调用链
 *  需链接 -lunwind -lunwind-x86_64 */
#define PRINT_STACK_ON_EXCEPTION 1

/** 置 1：打印协程生命周期日志（create / suspend / switch / destroy），
 *  带时间戳与 [文件名:行号] */
#define PRINT_CORO_RUNTIMEINFO 1
```

## 使用限制

1. **单线程调度模型**
   `Scheduler::init()` 与 `Scheduler::run()` **必须在同一线程**调用。启用 `IORING_SETUP_SINGLE_ISSUER` 后，若在其他线程提交/触发 ring 会报 `-EEXIST`。所有协程也都在这个线程上运行，`Scheduler` 本身不是线程安全的。

2. **`run()` 是阻塞的**
   `run()` 进入事件循环后阻塞直到 `stop()`。多线程程序中通常让主线程跑 `run()`，或将 `init()+run()` 放在专用线程；协程不应在 `run()` 所在线程之外驱动。

3. **协程内不要长时间占用**
   单线程事件循环，协程内执行 CPU 密集或阻塞系统调用会拖慢整个调度器；对 fire-and-forget 协程轮询 `is_finished()` 时也要用 `co_await timeout(...)` 主动让出，避免忙等独占。

4. **Task 生命周期与 `is_finished()` 语义**
   - `is_finished()` 实际含义是「协程帧已销毁」（由 `~promise_type()` 置位），而非「执行到 `co_return`」。
   - 对 `co_await` 的临时 Task，Task 对象与帧同生共死，观察不到 `is_finished()` 为 true；它主要配合 `detach()` + 具名 Task 轮询使用。
   - `Task` 的 `operator=(Task&&)` 被删除；移动后原对象的 `done_guard` 被移走，`is_finished()` 恒为 false。

5. **异常处理策略是 terminate**
   协程体内抛出未捕获异常时，`unhandled_exception()` 会打印栈回溯（若开启宏）后调用 `std::terminate()`，**不做异常跨协程传播**。请在协程内自行 `try/catch`。

6. **awaiter 参数与缓冲区生命周期**
   awaiter 以指针/引用捕获参数（如 `timeout` 的 `__kernel_timespec*`、`read/write` 的 `buffer`）。在 kernel ≤ 5.9 上 `timeout` 的时间结构体须在操作完成前保持有效；缓冲区须在 IO 完成前存活。建议放在协程帧内的局部变量，跨 `co_await` 保持。

7. **offset 语义**
   `read` / `write` 默认 `offset = 0`，即以文件起始位置作为绝对偏移（io_uring `prep_read/write` 语义），并非从当前游标追加。需要追加写时显式传入 `offset`。

8. **返回值错误码**
   异步 API 返回 io_uring 风格结果：`>=0` 为成功，`<0` 为 `-errno`（如 `timeout` 到期返回 `-ETIME`）。

9. **依赖工具链**
   header-only 但依赖 `liburing`（及开启调试宏时的 `libunwind`）；单元测试依赖 Boost.Test。低版本 liburing（< 2.3）下 `cancel_fd` 走手工填充 SQE 的兼容路径，需 kernel ≥ 5.19。
