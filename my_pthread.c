// File:	my_pthread.c
// Author:	Yujie REN, Jieming Yin
// Date:	April 2025

#include "my_pthread_t.h"
#include <errno.h>

/* ============================================================
 *                      全局变量与数据结构
 * ============================================================ */

/* 默认调度策略：编译时通过 -DMLFQ 切换为 MLFQ */
#ifdef MLFQ
static schedPolicy g_policy = POLICY_MLFQ;
#else
static schedPolicy g_policy = POLICY_PSJF;
#endif

static int           g_initialized   = 0;       // 库是否已初始化
static int           g_timer_started = 0;       // 时钟中断是否已启动
static my_pthread_t  g_next_tid      = 0;       // 下一个分配的线程ID
static tcb          *g_current       = NULL;    // 当前运行的线程
static tcb          *g_threads[MAX_THREADS] = {0}; // 线程ID -> tcb 映射

/* 调度器自己的上下文与栈 —— 信号处理器与同步切换都通过它完成 */
static ucontext_t    g_sched_ctx;
static char          g_sched_stack[STACK_SIZE];

/* PSJF / RR 使用的就绪队列 */
static threadQueue   g_ready;

/* MLFQ 多级队列：优先级 0 最高，时间片最短 */
static threadQueue   g_mlfq_queues[MLFQ_LEVELS];

/* MLFQ 各级时间片长度（毫秒）：优先级越低时间片越长 */
static const int     g_mlfq_quantum_ms[MLFQ_LEVELS] = {5, 10, 20, 40};

/* 累计运行毫秒数，用于周期性优先级提升（防饥饿） */
static int           g_elapsed_ms = 0;

/* ============================================================
 *                      信号屏蔽辅助
 * ============================================================ */

/* 屏蔽 SIGVTALRM 进入临界区 */
static void disable_preempt() {
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGVTALRM);
	sigprocmask(SIG_BLOCK, &set, NULL);
}

/* 解除 SIGVTALRM 屏蔽 */
static void enable_preempt() {
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGVTALRM);
	sigprocmask(SIG_UNBLOCK, &set, NULL);
}

/* ============================================================
 *                      队列辅助函数
 * ============================================================ */

static void queue_push(threadQueue *q, tcb *t) {
	t->next = NULL;
	if (q->tail == NULL) {
		q->head = q->tail = t;
	} else {
		q->tail->next = t;
		q->tail = t;
	}
}

static tcb *queue_pop(threadQueue *q) {
	if (q->head == NULL) return NULL;
	tcb *t = q->head;
	q->head = t->next;
	if (q->head == NULL) q->tail = NULL;
	t->next = NULL;
	return t;
}

/* ============================================================
 *                      调度策略
 * ============================================================ */

/* PSJF：从 g_ready 中取出 time_slices 最小的线程（O(n)） */
static tcb *pick_psjf() {
	if (g_ready.head == NULL) return NULL;
	tcb *prev = NULL, *p = g_ready.head;
	tcb *min_prev = NULL, *min = g_ready.head;
	while (p) {
		if (p->time_slices < min->time_slices) {
			min = p;
			min_prev = prev;
		}
		prev = p;
		p = p->next;
	}
	if (min_prev == NULL) g_ready.head = min->next;
	else min_prev->next = min->next;
	if (g_ready.tail == min) g_ready.tail = min_prev;
	min->next = NULL;
	return min;
}

/* MLFQ：从最高优先级（0 级）开始查找非空队列并弹出队首 */
static tcb *pick_mlfq() {
	for (int i = 0; i < MLFQ_LEVELS; i++) {
		if (g_mlfq_queues[i].head != NULL) {
			return queue_pop(&g_mlfq_queues[i]);
		}
	}
	return NULL;
}

/* 把线程加入合适的就绪队列（取决于策略） */
static void enqueue_ready(tcb *t) {
	if (g_policy == POLICY_MLFQ) {
		if (t->priority < 0) t->priority = 0;
		if (t->priority >= MLFQ_LEVELS) t->priority = MLFQ_LEVELS - 1;
		queue_push(&g_mlfq_queues[t->priority], t);
	} else {
		queue_push(&g_ready, t);
	}
}

/* 选择下一个要运行的线程 */
static tcb *pick_next() {
	if (g_policy == POLICY_MLFQ) return pick_mlfq();
	if (g_policy == POLICY_PSJF) return pick_psjf();
	return queue_pop(&g_ready);
}

/* MLFQ 优先级提升：把所有线程提到最高优先级队列，防止低优先级饥饿 */
static void mlfq_priority_boost() {
	if (g_policy != POLICY_MLFQ) return;
	/* 把 1..N-1 级队列中的所有线程移到 0 级 */
	for (int i = 1; i < MLFQ_LEVELS; i++) {
		while (g_mlfq_queues[i].head != NULL) {
			tcb *t = queue_pop(&g_mlfq_queues[i]);
			t->priority = 0;
			queue_push(&g_mlfq_queues[0], t);
		}
	}
	/* 把当前正在运行的线程也提升到 0 级（下一个 enqueue 时生效） */
	if (g_current) g_current->priority = 0;
}

/* 根据当前线程的优先级（MLFQ）或固定值（PSJF）启动一次性时钟 */
static void arm_timer_for(tcb *t) {
	int ms = TIME_QUANTUM_MS;
	if (g_policy == POLICY_MLFQ) {
		int p = t->priority;
		if (p < 0) p = 0;
		if (p >= MLFQ_LEVELS) p = MLFQ_LEVELS - 1;
		ms = g_mlfq_quantum_ms[p];
	}
	struct itimerval it;
	it.it_interval.tv_sec  = 0;
	it.it_interval.tv_usec = 0;        /* 单次模式，由调度器每次重置 */
	it.it_value.tv_sec     = 0;
	it.it_value.tv_usec    = ms * 1000;
	setitimer(ITIMER_VIRTUAL, &it, NULL);
}

/* ============================================================
 *                      调度器主体
 * ============================================================ */

/* 调度器入口：每次进入时选择下一个线程并切到它的上下文。
 * 调度器以 makecontext 创建在独立栈 g_sched_stack 上。 */
static void schedule_loop() {
	while (1) {
		/* MLFQ：检查是否需要周期性提升优先级 */
		if (g_policy == POLICY_MLFQ && g_elapsed_ms >= MLFQ_BOOST_PERIOD) {
			g_elapsed_ms = 0;
			mlfq_priority_boost();
		}

		tcb *next = pick_next();
		if (next == NULL) {
			/* 没有可调度线程：所有线程都已结束 */
			exit(0);
		}
		next->status = RUNNING;
		g_current = next;
		arm_timer_for(next);
		setcontext(&next->ctx);
		/* setcontext 不返回；下次回到 schedule_loop 通过 swapcontext */
	}
}

/* ============================================================
 *                      时钟中断处理
 * ============================================================ */

/* SIGVTALRM 处理器：当前线程的时间片用完时触发
 * 把当前线程放回就绪队列（MLFQ 下降级），切到调度器栈让它选下一个。 */
static void timer_handler(int sig) {
	(void)sig;
	if (g_current == NULL) return;

	g_current->time_slices++;

	/* MLFQ：用完一个时间片后降级；同时累计经过时间用于优先级提升 */
	if (g_policy == POLICY_MLFQ) {
		g_elapsed_ms += g_mlfq_quantum_ms[g_current->priority];
		if (g_current->priority < MLFQ_LEVELS - 1) {
			g_current->priority++;
		}
	}

	g_current->status = READY;
	enqueue_ready(g_current);

	tcb *self = g_current;
	swapcontext(&self->ctx, &g_sched_ctx);
}

/* 启动时钟中断（仅注册 sigaction，具体的 itimer 由调度器逐次设置） */
static void start_timer() {
	if (g_timer_started) return;
	g_timer_started = 1;

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = timer_handler;
	sa.sa_flags   = SA_RESTART;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGVTALRM, &sa, NULL);

	/* 由 schedule_loop 在每次切换时通过 arm_timer_for 设置具体值 */
}

/* ============================================================
 *                      库初始化与线程包装
 * ============================================================ */

/* 库初始化：注册主线程为 tid=0 的 tcb，准备调度器上下文 */
static void library_init() {
	if (g_initialized) return;
	g_initialized = 1;

	tcb *main_tcb = (tcb *)calloc(1, sizeof(tcb));
	main_tcb->tid    = g_next_tid++;
	main_tcb->status = RUNNING;
	main_tcb->stack  = NULL;
	main_tcb->priority = 0;
	g_threads[main_tcb->tid] = main_tcb;
	g_current = main_tcb;

	getcontext(&g_sched_ctx);
	g_sched_ctx.uc_stack.ss_sp   = g_sched_stack;
	g_sched_ctx.uc_stack.ss_size = STACK_SIZE;
	g_sched_ctx.uc_link          = NULL;
	makecontext(&g_sched_ctx, schedule_loop, 0);
}

/* 线程入口包装：调用真实入口函数后自动 my_pthread_exit */
static void thread_wrapper() {
	enable_preempt(); /* 用户代码运行时允许抢占 */
	tcb *self = g_current;
	void *ret = NULL;
	if (self->function) {
		ret = self->function(self->arg);
	}
	my_pthread_exit(ret);
}

/* ============================================================
 *                      公共 API
 * ============================================================ */

/* 创建线程：分配 TCB 与栈，初始化上下文，加入就绪队列 */
int my_pthread_create(my_pthread_t * thread, pthread_attr_t * attr,
                      void *(*function)(void*), void * arg) {
	(void)attr;
	if (!g_initialized) library_init();
	disable_preempt();

	if (g_next_tid >= MAX_THREADS) { enable_preempt(); return -1; }

	tcb *t = (tcb *)calloc(1, sizeof(tcb));
	if (!t) { enable_preempt(); return -1; }

	t->tid       = g_next_tid++;
	t->status    = READY;
	t->function  = function;
	t->arg       = arg;
	t->priority  = 0;
	t->stack     = malloc(STACK_SIZE);
	if (!t->stack) { free(t); enable_preempt(); return -1; }

	getcontext(&t->ctx);
	t->ctx.uc_stack.ss_sp   = t->stack;
	t->ctx.uc_stack.ss_size = STACK_SIZE;
	t->ctx.uc_link          = &g_sched_ctx;
	makecontext(&t->ctx, thread_wrapper, 0);

	g_threads[t->tid] = t;
	enqueue_ready(t);

	if (thread) *thread = t->tid;

	if (!g_timer_started) {
		start_timer();
		/* 给当前（主）线程也 arm 一次时钟，否则它会一直霸占 CPU */
		arm_timer_for(g_current);
	}
	enable_preempt();
	return 0;
}

/* 主动让出 CPU：把自己放回就绪队列，切到调度器 */
int my_pthread_yield() {
	if (!g_initialized) library_init();
	disable_preempt();
	tcb *self = g_current;
	self->status = READY;
	enqueue_ready(self);
	swapcontext(&self->ctx, &g_sched_ctx);
	enable_preempt();
	return 0;
}

/* 终止当前线程：保存返回值，唤醒 join 者，跳到调度器 */
void my_pthread_exit(void *value_ptr) {
	disable_preempt();
	tcb *self = g_current;
	self->retval = value_ptr;
	self->status = FINISHED;
	if (self->joiner) {
		self->joiner->status = READY;
		enqueue_ready(self->joiner);
		self->joiner = NULL;
	}
	setcontext(&g_sched_ctx);
	/* 不返回 */
}

/* 阻塞等待目标线程结束 */
int my_pthread_join(my_pthread_t thread, void **value_ptr) {
	if (!g_initialized) library_init();
	disable_preempt();

	if (thread >= MAX_THREADS) { enable_preempt(); return -1; }
	tcb *target = g_threads[thread];
	if (!target || target == g_current) { enable_preempt(); return -1; }

	if (target->status != FINISHED) {
		if (target->joiner != NULL) { enable_preempt(); return -1; }
		target->joiner = g_current;
		tcb *self = g_current;
		self->status = BLOCKED;
		swapcontext(&self->ctx, &g_sched_ctx);
		/* 被唤醒后此处继续 */
	}

	if (value_ptr) *value_ptr = target->retval;
	if (target->stack) free(target->stack);
	g_threads[target->tid] = NULL;
	free(target);
	enable_preempt();
	return 0;
}

/* ============================================================
 *                      互斥锁
 * ============================================================ */

int my_pthread_mutex_init(my_pthread_mutex_t *mutex,
                          const pthread_mutexattr_t *mutexattr) {
	(void)mutexattr;
	if (!mutex) return -1;
	mutex->locked      = 0;
	mutex->initialized = 1;
	mutex->owner       = NULL;
	mutex->wait_head   = NULL;
	mutex->wait_tail   = NULL;
	return 0;
}

/* 获取互斥锁：使用 GCC 内建 test-and-set 原子获取
 * 若锁被占用，把当前线程加入互斥锁等待队列并阻塞 */
int my_pthread_mutex_lock(my_pthread_mutex_t *mutex) {
	if (!mutex || !mutex->initialized) return -1;
	if (!g_initialized) library_init();

	while (1) {
		disable_preempt();
		if (__sync_lock_test_and_set(&mutex->locked, 1) == 0) {
			mutex->owner = g_current;
			enable_preempt();
			return 0;
		}
		/* 锁已占用：把自己挂到等待队列并阻塞，切到调度器 */
		tcb *self = g_current;
		self->next = NULL;
		if (mutex->wait_tail == NULL) {
			mutex->wait_head = mutex->wait_tail = self;
		} else {
			mutex->wait_tail->next = self;
			mutex->wait_tail = self;
		}
		self->status = BLOCKED;
		swapcontext(&self->ctx, &g_sched_ctx);
		enable_preempt();
		/* 被 unlock 唤醒后回到循环首部，重新尝试获取 */
	}
}

/* 释放互斥锁：清 locked，唤醒等待队列首部线程 */
int my_pthread_mutex_unlock(my_pthread_mutex_t *mutex) {
	if (!mutex || !mutex->initialized) return -1;
	disable_preempt();
	if (mutex->owner != g_current) { enable_preempt(); return -1; }

	mutex->owner = NULL;
	__sync_lock_release(&mutex->locked);

	if (mutex->wait_head != NULL) {
		tcb *t = mutex->wait_head;
		mutex->wait_head = t->next;
		if (mutex->wait_head == NULL) mutex->wait_tail = NULL;
		t->next = NULL;
		t->status = READY;
		enqueue_ready(t);
	}
	enable_preempt();
	return 0;
}

/* 销毁互斥锁：要求锁未被占用且无等待者 */
int my_pthread_mutex_destroy(my_pthread_mutex_t *mutex) {
	if (!mutex) return -1;
	if (mutex->locked || mutex->wait_head) return -1;
	mutex->initialized = 0;
	return 0;
}
