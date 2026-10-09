# yyasio

一个 **header-only** 的 C++20 异步 I/O 库，为 Linux `io_uring`（通过 `liburing`）提供薄封装，并内置基于 C++20 协程（coroutine）的 `Task<T>` 抽象，让你用同步风格的代码写异步逻辑。

使用时只需 `#include "yyasio.h"`，无需单独编译库文件。

## 特性

- **Header-only**：单个头文件 `yyasio.h`，拷贝即用。
- **io_uring 封装**：`read` / `write` / `sync_file_range` / `openat` / `close` / `accept` / `connect` / `listen` / `timeout` / `sleep` / `renameat` / `unlinkat` / `mkdirat` / `statx` / `cancel_fd` 等常用操作的异步 awaiter。
- **C++20 协程支持**：`Task<T>` / `Task<void>`，支持 `co_await` 组合与 `detach()` 的 fire-and-forget 用法。
- **单线程事件循环**：`Scheduler` 以单线程驱动 io_uring，模型简单、无锁竞争。
- **带外工作线程**：`run_in_thread` 把无法交给 io_uring 的阻塞调用丢到普通线程执行，协程 `co_await` 等结果；结果经控制管道回到 ring 线程，**协程只可能在 ring 线程上恢复**，因此不会破坏 `Event` / `Mutex` 的同线程契约。
- **可控停机**：`stop()` 从任意线程调用都安全，`run()` 退出前会排在途 IO 与带外任务排空，并在返回前自行释放 ring 与控制管道（**没有也不需要单独的 `shutdown()`**）。
- **协程调试能力**（编译期开关）：异常时打印硬件栈回溯与协程调用链、协程生命周期日志。
- **Event 原语**：`Event<T>` 支持在一个协程中等待、在另一个协程中唤醒。
- **TimeoutEvent 原语**：可等待的定时器。`co_await ev.wait_until(500ms)` 与 `ev.set()` 谁先到都只唤醒一次；被 `set()` 抢先时会自动 `abandon()` 掉未触发的定时器，迟到的 CQE 不会二次唤醒协程。
- **Mutex 原语**：协程互斥锁，支持临界区跨 `co_await` 点挂起，等待者按 FIFO 唤醒。

## 环境要求

| 项目 | 要求 | 说明 |
|---|---|---|
| 操作系统 | Linux 内核 **5.11+** | io_uring 基础操作 |
| 内核 5.6+ | `statx` | 依赖 `IORING_OP_STATX`；早于本库 5.11 基线，故无额外要求 |
| 内核 5.4+ | `timeout` / `sleep` | 依赖 `IORING_OP_TIMEOUT`；早于本库 5.11 基线，故无额外要求 |
| 内核 **5.13+** | `run_in_thread` / `stop()` | 依赖 `IORING_OP_POLL_ADD` + `IORING_POLL_ADD_MULTI` 常驻监听控制管道，**高于本库 5.11 基线**；不使用这两个能力时仍是 5.11 |
| 内核 **5.15+** | `mkdirat` | 依赖 `IORING_OP_MKDIRAT`（`renameat` / `unlinkat` 只需 5.11） |
| 内核 5.19+ | `cancel_fd` | 异步取消（`IORING_ASYNC_CANCEL_FD`） |
| 内核 **5.5+** | `TimeoutEvent` / `abandon()` | 依赖 `IORING_OP_ASYNC_CANCEL` 按 `user_data` 取消（`flags = 0`）；`IORING_ASYNC_CANCEL_*` 匹配标志需更新内核，本库当前未使用 |
| 内核 **6.0+** | 可选优化 | `IORING_SETUP_SINGLE_ISSUER` / `IORING_SETUP_DEFER_TASKRUN`；低版本内核会自动回退到无 flag 模式 |
| 编译器 | GCC 10+（建议 GCC 11+） | 需支持 C++20 `std::coroutine`；GCC 11 不支持 requires 子句过滤成员函数，代码已做兼容 |
| 依赖库 | `liburing` | 异步 I/O 后端。`abandon()` 用到 `io_uring_prep_cancel64()`：若你的 liburing 没有这个内联函数，可等价地手填 SQE（`opcode = IORING_OP_ASYNC_CANCEL` + `sqe->addr = user_data`），同 `cancel_fd` 在 liburing < 2.3 下的兼容路径。`run_in_thread` / `stop()` 用到 `io_uring_prep_poll_multishot()`，旧版 liburing 下也可手填：`opcode = IORING_OP_POLL_ADD`、`fd` 为管道读端、`len` 先置 `POLLIN` 再叠加 `IORING_POLL_ADD_MULTI`、`addr` 置 0 |
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
    // 2. 初始化 Scheduler（init()、detach()、run() 三者必须在同一线程，见「使用限制」）
    yyasio::Scheduler scheduler;
    if (scheduler.init(256) != yyasio::YYASIO_OK) return 1;

    // 3. 启动协程：detach() 表示 fire-and-forget
    read_file(&scheduler).detach();

    // 4. 进入事件循环（阻塞，直到 stop()；返回时 ring 已释放）
    scheduler.run();
}
```

完整示例见 [`usage.cpp`](./usage.cpp)（含回声服务器、文件读写、目录增删改、`statx` 元信息、`sleep` 定时睡眠、超时轮询、`TimeoutEvent`、协程 ID 获取等），协程与 API 的单元测试见 [`yyasio_unittest.cpp`](./yyasio_unittest.cpp)。

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

### TimeoutEvent 原语

一个可等待的定时器，语义上是 `Event`（auto-reset 型，同 cppcoro 的 `async_auto_reset_event`）加上超时：等 `set()` 或等超时，**谁先到都只唤醒一次**。

```cpp
yyasio::TimeoutEvent ev(&scheduler);

yyasio::Task<void> worker(yyasio::TimeoutEvent& e) {
    co_await e.wait_until(std::chrono::milliseconds(500)); // 最多等 500ms
    // ... 被 set() 叫起或被超时叫起，都会走到这里，且只走一次 ...
}

yyasio::Task<void> setter(yyasio::TimeoutEvent& e) {
    // ... 做一些异步工作 ...
    e.set();   // 提前唤醒 waiter；同时取消那个还没到期的定时器
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

### 目录操作

`mkdirat` / `renameat` / `unlinkat` 直接对应同名系统调用，`dirfd` 传 `AT_FDCWD` 则路径按当前工作目录解析，传目录 fd 则按该目录相对解析（打开目录用 `openat(..., O_RDONLY | O_DIRECTORY)`）：

```cpp
yyasio::Task<void> dir_demo(yyasio::Scheduler* scheduler) {
    int ret = co_await yyasio::mkdirat(scheduler, AT_FDCWD, "/tmp/a_dir");        // 默认 0755
    ret = co_await yyasio::renameat(scheduler, AT_FDCWD, "/tmp/a_dir", AT_FDCWD, "/tmp/b_dir");
    ret = co_await yyasio::unlinkat(scheduler, AT_FDCWD, "/tmp/b_dir", AT_REMOVEDIR); // 删目录
}
```

删目录靠 `unlinkat` 加 `AT_REMOVEDIR` 标志（定义在 `<fcntl.h>`，由调用方 include）。**没有递归删除**：目录里还有条目时返回 `-ENOTEMPTY`，需要调用方先清空；同理，不带该标志删目录返回 `-EISDIR`。完整流程见 [`usage.cpp`](./usage.cpp) 的 `directory_ops_demo`。

### 文件元信息（statx）

`statx` 对应系统调用 `statx(2)`：元信息写进调用方提供的 `struct statx`，`co_await` 的返回值是 `statx(2)` 的结果本身（成功 `0`，失败 `-errno`）。`struct statx`、`AT_*` 标志与 `STATX_*` mask 均来自 `<sys/stat.h>`，需调用方自行 include——`yyasio.h` 只前向声明该 struct，因为 wrapper 仅转传指针：

```cpp
yyasio::Task<void> statx_demo(yyasio::Scheduler* scheduler) {
    struct statx stx {};
    // 默认 flags = 0（跟随符号链接），mask = STATX_BASIC_STATS
    int ret = co_await yyasio::statx(scheduler, AT_FDCWD, "/tmp/a.txt", &stx);
    std::cout << "size = " << stx.stx_size << ", is_reg = " << S_ISREG(stx.stx_mode) << "\n";

    // AT_SYMLINK_NOFOLLOW：报告链接本身，而不是它的目标
    struct statx link_self {};
    co_await yyasio::statx(scheduler, AT_FDCWD, "/tmp/a_link", &link_self, AT_SYMLINK_NOFOLLOW);

    // dirfd + 相对名同样可用
    int dir_fd = co_await yyasio::openat(scheduler, AT_FDCWD, "/tmp", O_RDONLY | O_DIRECTORY);
    struct statx relative {};
    ret = co_await yyasio::statx(scheduler, dir_fd, "a.txt", &relative);
    co_await yyasio::close(scheduler, dir_fd);
}
```

`statxbuf` 与 `path` 都是指针捕获，内核要到 CQE 回来时才写缓冲，所以两者必须存活到操作完成——放在协程帧内的局部变量即可。完整流程（含不存在路径的 `-ENOENT`）见 [`usage.cpp`](./usage.cpp) 的 `statx_demo`。

### 定时睡眠（sleep）

`co_await yyasio::sleep(scheduler, 1500ms)` 挂起当前协程、到点再回来，接受任意 `std::chrono::duration`：

```cpp
yyasio::Task<void> nap(yyasio::Scheduler* scheduler) {
    co_await yyasio::sleep(scheduler, std::chrono::milliseconds(100));
    co_await yyasio::sleep(scheduler, std::chrono::seconds(2));
    co_await yyasio::sleep(scheduler, std::chrono::milliseconds(-50)); // 负值钳制成 0，立即到期
}
```

与 `timeout(sched, ts, count, flags)` 的区别在于 timespec 归属：`timeout` 只转传你给的指针，该内存必须活到操作完成；`sleep` 把 timespec 放在自己的 awaiter 内（awaiter 分配在协程帧里，活到 `await_resume()` 之后），调用方不需要操心。

两条语义约定：

- **睡满返回 `-ETIME`，不是 `0`**。这是 io_uring 定时器的报法（到期 `-ETIME`，被取消 `-ECANCELED`，只有靠完成事件凑够 `count` 才返回 `0`），与下文第 8 条一致，不要把 `< 0` 当失败分支。
- **计时用 `CLOCK_MONOTONIC`**，不含系统挂起时间；需要 `BOOTTIME` / `REALTIME` / 绝对时刻 / 事件计数时请直接用底层 `timeout`。

完整演示（各时长实测耗时 + 三路并发睡眠）见 [`usage.cpp`](./usage.cpp) 的 `sleep_demo`，回归用例见 [`yyasio_unittest.cpp`](./yyasio_unittest.cpp) 的 `test_sleep_*`。

### 带外工作线程（run_in_thread）

有些阻塞调用无法交给 io_uring（典型如 `getdents64` 列目录、第三方同步 SDK、CPU 密集换算）。`run_in_thread` 把它们丢到普通线程执行，协程 `co_await` 等结果，而**协程仍然只在 ring 线程上恢复**：

```cpp
yyasio::Task<void> work(yyasio::Scheduler* scheduler) {
    // callable 签名是 T()，返回值就是结果
    auto [rc, text] = co_await yyasio::run_in_thread<std::string>(
        scheduler, [] { return heavy_sync_call(); });

    if (rc == 0)         { /* text 有效 */ }
    else if (rc == -ECANCELED) { /* 调度器没接受（未 init / 已进入停机），text 是默认值 */ }
}
```

机制：`Scheduler` 内部有一对 `pipe2(O_CLOEXEC | O_NONBLOCK)` 控制管道，`init()` 时在 ring 线程上挂一条常驻的 `IORING_OP_POLL_ADD` + `IORING_POLL_ADD_MULTI` 监听读端。等待结果的协程在 ring 线程上登记一个 ticket 后挂起；工作线程算完只往写端 `write()` 那 8 字节 ticket，CQE 由 ring 线程消费、按 ticket 找回对应协程并恢复。

过内核的只有那 8 字节“门铃”，**结果对象本身走用户态内存，不需要序列化**：`T` 可以是 `std::string`、`std::vector`、move-only 类型等（`T` 需默认可构造 + 可移动构造 + 可移动赋值）。工作侧的内存可见性由 `await_resume()` 里的 `thread::join()` 保证，所以 `join` 不能改成 `detach`；`Event` / `Mutex` 那些“必须在 ring 线程上操作”的契约因此继续成立。

使用上要记住：

- `rc` 只表示**调度层**结果（`0` / `-ECANCELED`），工作线程内部的失败请放进 `T`（如 `std::optional<X>` 或 `std::pair<int,X>`）。
- **一次调用一个线程**，实测单程往返 260~380μs（线程创建占大头）。高频路径请改成线程池，控制管道机制可直接复用。
- 任务在途时 `run()` 不会退出（退出条件包含“没有等待带外结果的协程”），所以 **`Scheduler` 对象必须活得比工作线程久**。
- **工作线程内抛出的异常不会被协程侧的 `try/catch` 捕获**（它在另一个线程），会直接逸出导致 `std::terminate()`；请在 callable 内部自行 `try/catch`。
- 等待带外结果与等待 IO 一样，可以在 `co_await` 之前被 `Mutex`、`TimeoutEvent` 等包围，但它们仍必须在同一个 ring 线程上使用。

底层入口（需要自己支配 ticket 时可用）：`register_coro_waiting_extern(coro, &ticket)` 在 ring 线程登记等待，外部线程调 `wakeup_coro_by_ticket(ticket)` 唤醒——`run_in_thread` 就是这对接口的封装。

完整演示（阻塞调用取值、move-only 结果、与 io_uring IO 并发、`opendir/readdir` 列目录）见 [`usage.cpp`](./usage.cpp) 的 `run_in_thread_demo`，1000 个带外任务并发的回归用例见 [`yyasio_unittest.cpp`](./yyasio_unittest.cpp) 的 `test_run_in_thread_many_concurrent`。

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
| `timeout(sched, ts, count=0, flags=0)` | 定时器；`ts` 为指针捕获，需存活到操作完成 |
| `sleep(sched, duration)` | 定时睡眠，接受任意 `std::chrono::duration`；timespec 由 awaiter 自持；睡满返回 `-ETIME`，负值按 `0` 处理 |
| `renameat(sched, olddirfd, old, newdirfd, new, flags=0)` | 重命名（文件与目录同用，对应 `renameat2`） |
| `unlinkat(sched, dirfd, path, flags=0)` | 删除；删目录传 `flags = AT_REMOVEDIR`，且目录必须已空 |
| `mkdirat(sched, dirfd, path, mode=0755)` | 创建目录，`mode` 受进程 umask 过滤（需内核 5.15+） |
| `statx(sched, dirfd, path, buf, flags=0, mask=0x7ff)` | 文件元信息，结果写入 `*buf`，返回 `0` / `-errno`；`mask` 即 `STATX_BASIC_STATS`，`flags` 可传 `AT_SYMLINK_NOFOLLOW`、`AT_EMPTY_PATH`（需 `<sys/stat.h>`，内核 5.6+） |
| `cancel_fd(sched, fd, flags=0)` | 取消某 fd 上的在途请求 |
| `ev.wait_until(duration)` | `TimeoutEvent`：最多等待 `duration`，可被 `set()` 提前唤醒，返回 `void` |
| `ev.set()` | 唤醒等待中的协程，并 `abandon()` 掉未触发的定时器 |
| `scheduler.abandon(index)` | 放弃 `schedule()` 返回的 index 对应的在途 IO（详见上文） |
| `mutex.[un]lock()` | 协程互斥锁加锁（不可用则挂起，FIFO 唤醒） |
| `guard = co_await mutex.lock_guard()` | RAII 加锁，返回 `LockGuard`，离开作用域自动解锁 |
| `mutex.unlock()` / `mutex.try_lock()` | 解锁（转移所有权给队首等待者）/ 非阻塞尝试加锁 |
| `current_coro_id()` | 取当前协程地址作为 ID |
| `run_in_thread<T>(sched, callable)` | 带外工作线程：在普通线程上跑 `T()`，`co_await` 得到 `std::tuple<int, T>`（`0` 已执行 / `-ECANCELED` 未被接受且 `T` 为默认值）；需内核 5.13+，详见上文 |

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
   `Scheduler::init()`、协程的 `detach()` / `co_await` 发起、`Scheduler::run()` **必须都在同一个线程**上（称为 ring 线程）。启用 `IORING_SETUP_SINGLE_ISSUER` 后，在其他线程提交 ring 会报 `-EEXIST`；`schedule()` / `abandon()` 等入口都有 `assert(is_on_ring_thread())`。特别注意：**在 `init()` 之前（或在别的线程上）`detach()` 协程是无效的**——awaiter 会直接拿到 `-ECANCELED` 而根本不发起 IO。所有协程也都在 ring 线程上运行，`Scheduler` 本身不是线程安全的。

2. **`run()` 是阻塞的**
   `run()` 进入事件循环后阻塞，直到 `stop()` 被处理且**已无待提交项、无在途 IO、无等待带外结果的协程**；返回前它会自行释放 ring 与控制管道（**没有 `shutdown()`，也不需要配对调用**）。`stop()` 可以从任意线程调用（它只往控制管道写一个 ticket，因而能立即唤醒阻塞中的循环）。多线程程序里常见做法是把 `init()+detach()+run()` 都放在专用线程，主线程用 `stop()` + `join()` 收尾。

3. **协程内不要长时间占用**
   单线程事件循环，协程内执行 CPU 密集或阻塞系统调用会拖慢整个调度器；这类工作请交给 `run_in_thread`（见上文）。对 fire-and-forget 协程轮询 `is_finished()` 时也要用 `co_await timeout(...)` / `co_await sleep(...)` 主动让出，避免忙等独占。

4. **Task 生命周期与 `is_finished()` 语义**
   - `is_finished()` 实际含义是「协程帧已销毁」（由 `~promise_type()` 置位），而非「执行到 `co_return`」。
   - 对 `co_await` 的临时 Task，Task 对象与帧同生共死，观察不到 `is_finished()` 为 true；它主要配合 `detach()` + 具名 Task 轮询使用。
   - `Task` 的 `operator=(Task&&)` 被删除；移动后原对象的 `done_guard` 被移走，`is_finished()` 恒为 false。

5. **异常处理策略是 terminate**
   协程体内抛出未捕获异常时，`unhandled_exception()` 会打印栈回溯（若开启宏）后调用 `std::terminate()`，**不做异常跨协程传播**。请在协程内自行 `try/catch`。`run_in_thread` 的 callable 跑在**另一个线程**，协程里的 `try/catch` 拦不到它，异常会直接逸出导致 `std::terminate()`——必须在 callable 内部处理。

6. **awaiter 参数与缓冲区生命周期**
   awaiter 以指针/引用捕获参数（如 `timeout` 的 `__kernel_timespec*`、`read/write` 的 `buffer`、`statx` 的 `struct statx*` 与 `path`）。在 kernel ≤ 5.9 上 `timeout` 的时间结构体须在操作完成前保持有效；缓冲区须在 IO 完成前存活。建议放在协程帧内的局部变量，跨 `co_await` 保持。例外：`sleep` 自己持有 timespec，无需调用方保留。

7. **offset 语义**
   `read` / `write` 默认 `offset = 0`，即以文件起始位置作为绝对偏移（io_uring `prep_read/write` 语义），并非从当前游标追加。需要追加写时显式传入 `offset`。

8. **返回值错误码**
   异步 API 返回 io_uring 风格结果：`>=0` 为成功，`<0` 为 `-errno`（如 `timeout` / `sleep` 到期返回 `-ETIME`）。两个例外：调度器**未接受**该操作时统一得到 `-ECANCELED`（此时协程不挂起、立即恢复）；`run_in_thread` 返回的是 `std::tuple<int, T>`。

9. **依赖工具链**
   header-only 但依赖 `liburing`（及开启调试宏时的 `libunwind`）；单元测试依赖 Boost.Test。低版本 liburing（< 2.3）下 `cancel_fd` 走手工填充 SQE 的兼容路径，需 kernel ≥ 5.19。

10. **`run_in_thread` / `stop()` 的内核与生命周期要求**
    两者都建立在控制管道上的常驻 `IORING_POLL_ADD_MULTI` 上，需 kernel **5.13+**（高于其余 API 的 5.11 基线）。另外：`Scheduler` 对象必须活得比所有工作线程久（`run()` 会等在途任务排空，但对象提前销毁会让工作线程回调悬空）；不要在 `run()` 运行期间关闭那对管道 fd，否则 `stop()` 与带外唤醒会一起失效。
