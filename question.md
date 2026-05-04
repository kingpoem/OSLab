# 实现过程中遇到的问题与调试记录

## 1. 在信号处理器中"直接修改 ucontext"导致段错误（最大的坑）

### 问题现象
Step 3 最初实现抢占式调度时，我采用的方案是：

```c
static void timer_handler(int sig, siginfo_t *info, void *ucontext) {
    ucontext_t *uc = (ucontext_t *)ucontext;
    g_current->ctx = *uc;             // 保存当前线程
    /* ...选下一个线程 next ... */
    *uc = next->ctx;                  // 让 sigreturn 直接跳到 next
}
```

理论上 `sigreturn` 会用 `*uc` 恢复寄存器，从而切到 `next`。但实际运行 `test_step3` 时立刻 `Segmentation fault`。

GDB 现场：

```
Program received signal SIGSEGV, Segmentation fault.
thread_wrapper () at my_pthread.c:219
rsp = 0x55555558b2e8
rip = 0x555555555c42 <thread_wrapper>      ; 还没执行第一条指令
rsp+0..16 字节里居然出现 "[thread 0] done, count=5..." 字符串
```

栈上写着别的线程 printf 的字符串，说明 sigreturn 之后 RSP 落到了一个**已经被别人用过**的内存区域，根本不是 `makecontext` 设置的栈顶。

### 根因
Linux x86_64 的 `ucontext_t` 不是单纯的 POD 结构：

```c
typedef struct ucontext_t {
    ...
    mcontext_t        uc_mcontext;     // 内含 fpregs 指针
    sigset_t          uc_sigmask;
    struct _libc_fpstate __fpregs_mem; // FP 状态，由 fpregs 指针引用
} ucontext_t;
```

- 信号处理器收到的 `ucontext_t *uc` 是内核构造的 sigframe，它的 `uc_mcontext.fpregs` 指向 sigframe 内部的某个位置
- 用 `makecontext` 创建出来的 `next->ctx`，其 `uc_mcontext.fpregs` 指向 `next->ctx.__fpregs_mem`
- 我直接 `*uc = next->ctx` 整体赋值时，`fpregs` 这个**绝对地址指针**也被覆盖
- 然后 sigframe 中保存返回地址等的相对位置全乱了，sigreturn 解出来的 RSP / FP 状态都是脏的

简而言之：**`ucontext_t` 不能跨 sigframe / makecontext 整块拷贝**，结构里有自引用指针。

### 解决方案
回到经典方案：用一个**独立的调度器栈** `g_sched_stack` + `g_sched_ctx`，在信号处理器内部 `swapcontext` 出去：

```c
static void timer_handler(int sig) {
    g_current->status = READY;
    enqueue_ready(g_current);
    tcb *self = g_current;
    swapcontext(&self->ctx, &g_sched_ctx);   // 由 glibc 正确处理 fpstate
}
```

由 glibc 来管理 `ucontext_t` 的保存/恢复，不再触碰内部布局。

---

## 2. 主线程在第一次 `my_pthread_create` 后不会被抢占

### 问题现象
Step 4 改成 MLFQ 后，把 itimer 从周期模式改成"每次切换前由调度器 arm 一次"的单次模式。结果 benchmark 里：

```c
for (i = 0; i < N; i++) pthread_create(...);   // 创建子线程
for (i = 0; i < N; i++) pthread_join(...);     // 阻塞等待
```

子线程根本没拿到 CPU，主线程一路跑到 `pthread_join`，然后才在 join 内部 `swapcontext` 切走。

### 根因
- 周期 itimer 模式下，setitimer 设了之后就一直滴答；任意线程都会被定期打断
- 改单次模式后，每次 timer 由 `schedule_loop` 在 `setcontext(next)` 之前 arm
- 但第一次 `create` 完成时还在**主线程**，根本没有进入过 `schedule_loop`，itimer 始终是 0

### 解决方案
第一次启动 timer 时，立即给当前（主）线程 arm 一次：

```c
if (!g_timer_started) {
    start_timer();
    arm_timer_for(g_current);   // 让主线程也能被打断
}
```

---

## 3. 临界区保护：sigprocmask 屏蔽 SIGVTALRM

### 问题现象
没屏蔽信号时，偶发的 segfault / 死锁，特别是 mutex 相关测试。

### 根因
所有公开 API 都会读写以下共享结构：

- `g_ready` 就绪队列
- `g_mlfq_queues[]` MLFQ 各级队列
- `mutex->wait_head/tail` 等待队列
- `g_current` 全局指针

如果在 `my_pthread_yield` 里改完了 `g_ready.tail` 还没改 `g_ready.head`，恰好被时钟中断打断，进入 `timer_handler` 又访问 ready 队列 → 队列状态不一致 → 取出来的 tcb 有 NULL 指针。

### 解决方案
封装一对辅助函数：

```c
static void disable_preempt() { sigprocmask(SIG_BLOCK,   ...SIGVTALRM..., NULL); }
static void enable_preempt()  { sigprocmask(SIG_UNBLOCK, ...SIGVTALRM..., NULL); }
```

**所有公开 API 入口立即 `disable_preempt`，离开前 `enable_preempt`**。线程刚被 `makecontext` 创建出来时还在屏蔽态，所以 `thread_wrapper` 第一行也调用 `enable_preempt` 才进入用户函数。

`swapcontext` 会把当前 sigmask 保存到 ctx，下次恢复 ctx 时 sigmask 也跟着恢复——这个细节使得跨切换的临界区保护是自洽的。

---

## 4. mutex_lock 里"先 TAS 再排队"的竞态

### 第一版（错的）

```c
int my_pthread_mutex_lock(...) {
    while (__sync_lock_test_and_set(&m->locked, 1) == 1) {
        // 入等待队列
        // swapcontext 出去
    }
}
```

中间没有禁用抢占。如果两步之间被时钟中断（在 enqueue 一半的时刻），互斥锁的等待队列会损坏。

### 修正
把 TAS、入队、阻塞作为一个原子段：

```c
while (1) {
    disable_preempt();
    if (__sync_lock_test_and_set(&m->locked, 1) == 0) {
        m->owner = g_current;
        enable_preempt();
        return 0;
    }
    /* 入等待队列 → 阻塞 → 切换 */
    swapcontext(&self->ctx, &g_sched_ctx);
    enable_preempt();
}
```

---

## 5. Makefile 的"歧义写法"

`Makefile` 里这段：

```makefile
my_pthread.o: my_pthread_t.h


ifeq ($(SCHED), PSJF)
	$(CC) -pthread $(CFLAGS) my_pthread.c
...
endif
```

刚看到时以为 `ifeq...endif` 是顶层条件指令，里面带 tab 的命令在顶层不合法。实际 GNU make 会把它视作 `my_pthread.o` 这个目标的 recipe（即使中间隔了一个空行）。`make` 实测能正常生成 .o 和 .a。

**结论：能跑就别动**，符合实验"不修改测试和脚本"的要求。

---

## 6. `make clean` 会清掉 `record/`

`Benchmark/Makefile`：

```makefile
clean:
	rm -rf testcase parallelCal vectorMultiply externalCal *.o ./record/
```

每次切换 PSJF/MLFQ/std pthread 都要 make clean → record 没了 → externalCal 直接失败。

测试脚本 `run_all_tests.sh` 里加了 `ensure_record()`：每次跑 externalCal 前检查 `record/` 存在，否则调用 `genRecord.sh` 重生成。

---

## 7. 线程资源回收（半截泄漏）

`my_pthread_join` 里才 `free(target->stack)` + `free(target)`。如果用户 `pthread_create` 后 **没 join**，对应栈和 tcb 永久泄漏。

实验任务书里没要求 `pthread_detach`，benchmark 也都正确 join，所以暂时不补。但作为一个待办事项写在这里。

---

## 8. PSJF 是 O(n) 选择

`pick_psjf` 每次扫描整个就绪队列找 `time_slices` 最小的线程，时间复杂度 O(n)。线程数大时（如 256 上限）调度开销不小，可以改用最小堆 O(log n)。本次实验线程数有限，没做这个优化。

---

## 9. 编译时切换 `USE_MY_PTHREAD` 宏

测试脚本 `run_all_tests.sh` 既要测自定义库，又要测系统 pthread。两者的差别只在 `my_pthread_t.h` 里的：

```c
#define USE_MY_PTHREAD 1
```

是否启用。脚本通过 `sed` 临时改头文件，`trap EXIT` 保证异常退出时也能恢复成"启用"状态，避免破坏开发环境。

---

## 10. signal handler 中 `printf` 不安全

调试时为了观察上下文切换在 `timer_handler` 里加 `printf`。表面看也能工作，但 `printf` 不是 async-signal-safe，且 `stdout` 自带的锁可能与被抢占线程持有的锁形成死锁。最终全部改用全局计数器 + 主线程定期打印的方式做调试观察。

> 教训：**信号处理器内只做最少的必要事**：保存上下文 / 入队 / `swapcontext` 出去，其他全在调度器栈上做。

---

## 总结
最难定位的是 **#1（ucontext 自引用指针）** 和 **#2（首次抢占缺失）**，都属于"看起来逻辑对，运行就崩"的类型，只能依赖 GDB 看寄存器和栈。

剩下的几乎全是并发同步问题，"哪些数据结构是共享的、哪些操作必须原子" 想清楚之后基本一次写对。
