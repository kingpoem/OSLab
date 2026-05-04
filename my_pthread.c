// File:	my_pthread.c
// Author:	Yujie REN, Jieming Yin
// Date:	April 2025

#include "my_pthread_t.h"

/* ============================================================
 *                      全局变量与数据结构
 * ============================================================ */

/* 默认调度策略：编译时通过 -DMLFQ 切换为 MLFQ */
#ifdef MLFQ
static schedPolicy g_policy = POLICY_MLFQ;
#else
static schedPolicy g_policy = POLICY_PSJF;
#endif

static int           g_initialized = 0;        // 库是否已初始化
static my_pthread_t  g_next_tid    = 0;        // 下一个分配的线程ID
static tcb          *g_current     = NULL;     // 当前运行的线程
static tcb          *g_threads[MAX_THREADS] = {0}; // 线程ID -> tcb 映射
static ucontext_t    g_sched_ctx;              // 调度器上下文
static char          g_sched_stack[STACK_SIZE]; // 调度器栈
static threadQueue   g_ready;                  // 就绪队列（RR/PSJF 使用）

/* ============================================================
 *                      队列辅助函数
 * ============================================================ */

/* 将一个 tcb 节点入队到队尾 */
static void queue_push(threadQueue *q, tcb *t) {
	t->next = NULL;
	if (q->tail == NULL) {
		q->head = q->tail = t;
	} else {
		q->tail->next = t;
		q->tail = t;
	}
}

/* 从队首弹出一个 tcb，队列空则返回 NULL */
static tcb *queue_pop(threadQueue *q) {
	if (q->head == NULL) return NULL;
	tcb *t = q->head;
	q->head = t->next;
	if (q->head == NULL) q->tail = NULL;
	t->next = NULL;
	return t;
}

/* ============================================================
 *                      调度器
 * ============================================================ */

/* 选择下一个要运行的线程（FCFS / RR：直接取队首） */
static tcb *pick_next() {
	return queue_pop(&g_ready);
}

/* 调度器主循环：从队列中取出下一个线程并切换过去 */
static void schedule() {
	while (1) {
		tcb *next = pick_next();
		if (next == NULL) {
			/* 没有可运行线程，正常情况下不应发生 */
			return;
		}
		next->status = RUNNING;
		g_current = next;
		setcontext(&next->ctx);
		/* setcontext 不返回，下次回到 schedule 是通过 swapcontext */
	}
}

/* ============================================================
 *                      线程包装与初始化
 * ============================================================ */

/* 线程入口包装函数：调用真正的入口函数后自动调用 my_pthread_exit */
static void thread_wrapper() {
	tcb *self = g_current;
	void *ret = NULL;
	if (self->function) {
		ret = self->function(self->arg);
	}
	my_pthread_exit(ret);
}

/* 初始化线程库：把当前主线程注册为 tid=0 的线程，准备调度器上下文 */
static void library_init() {
	if (g_initialized) return;
	g_initialized = 1;

	/* 主线程 TCB */
	tcb *main_tcb = (tcb *)calloc(1, sizeof(tcb));
	main_tcb->tid    = g_next_tid++;
	main_tcb->status = RUNNING;
	main_tcb->stack  = NULL;          // 主线程使用进程默认栈
	g_threads[main_tcb->tid] = main_tcb;
	g_current = main_tcb;

	/* 调度器上下文 */
	getcontext(&g_sched_ctx);
	g_sched_ctx.uc_stack.ss_sp   = g_sched_stack;
	g_sched_ctx.uc_stack.ss_size = STACK_SIZE;
	g_sched_ctx.uc_link          = NULL;
	makecontext(&g_sched_ctx, schedule, 0);
}

/* ============================================================
 *                      公共 API
 * ============================================================ */

/* 创建一个新线程：分配 TCB 与栈，初始化上下文，加入就绪队列 */
int my_pthread_create(my_pthread_t * thread, pthread_attr_t * attr,
                      void *(*function)(void*), void * arg) {
	(void)attr;
	if (!g_initialized) library_init();

	if (g_next_tid >= MAX_THREADS) return -1;

	tcb *t = (tcb *)calloc(1, sizeof(tcb));
	if (!t) return -1;

	t->tid       = g_next_tid++;
	t->status    = READY;
	t->function  = function;
	t->arg       = arg;
	t->stack     = malloc(STACK_SIZE);
	if (!t->stack) { free(t); return -1; }

	getcontext(&t->ctx);
	t->ctx.uc_stack.ss_sp   = t->stack;
	t->ctx.uc_stack.ss_size = STACK_SIZE;
	t->ctx.uc_link          = &g_sched_ctx;
	makecontext(&t->ctx, thread_wrapper, 0);

	g_threads[t->tid] = t;
	queue_push(&g_ready, t);

	if (thread) *thread = t->tid;
	return 0;
}

/* 主动让出 CPU：把当前线程放回就绪队列，切换到调度器 */
int my_pthread_yield() {
	if (!g_initialized) library_init();
	tcb *self = g_current;
	self->status = READY;
	queue_push(&g_ready, self);
	swapcontext(&self->ctx, &g_sched_ctx);
	return 0;
}

/* 终止当前线程：保存返回值，唤醒 join 者，切换到调度器 */
void my_pthread_exit(void *value_ptr) {
	tcb *self = g_current;
	self->retval = value_ptr;
	self->status = FINISHED;
	/* 若有线程在 join 此线程，把它放回就绪队列 */
	if (self->joiner) {
		self->joiner->status = READY;
		queue_push(&g_ready, self->joiner);
		self->joiner = NULL;
	}
	setcontext(&g_sched_ctx);
	/* 不返回 */
}

/* 阻塞等待目标线程结束，并取出其返回值 */
int my_pthread_join(my_pthread_t thread, void **value_ptr) {
	if (!g_initialized) library_init();
	if (thread >= MAX_THREADS) return -1;
	tcb *target = g_threads[thread];
	if (!target) return -1;
	if (target == g_current) return -1; /* 不能 join 自己 */

	if (target->status != FINISHED) {
		/* 目标线程还没结束，把自己挂到 target->joiner 上并阻塞 */
		if (target->joiner != NULL) return -1; /* 已被其他线程 join */
		target->joiner = g_current;
		g_current->status = BLOCKED;
		swapcontext(&g_current->ctx, &g_sched_ctx);
	}

	/* 此时 target 已经结束 */
	if (value_ptr) *value_ptr = target->retval;

	/* 释放被 join 线程的资源 */
	if (target->stack) free(target->stack);
	g_threads[target->tid] = NULL;
	free(target);
	return 0;
}

/* ============================================================
 *                      互斥锁（Step 2 实现）
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

/* 获取互斥锁：用 GCC 内建的 test-and-set 实现原子获取
 * 若锁已被占用，把当前线程加入到该锁的等待队列并阻塞，切换到调度器。 */
int my_pthread_mutex_lock(my_pthread_mutex_t *mutex) {
	if (!mutex || !mutex->initialized) return -1;
	if (!g_initialized) library_init();

	/* 尝试原子地把 locked 从 0 置为 1 */
	while (__sync_lock_test_and_set(&mutex->locked, 1) == 1) {
		/* 锁已被占用，把自己加入等待队列并阻塞 */
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
		/* 被 unlock 唤醒后，重新尝试获取锁 */
	}
	mutex->owner = g_current;
	return 0;
}

/* 释放互斥锁：清除占用标志，把等待队列中第一个线程移回就绪队列 */
int my_pthread_mutex_unlock(my_pthread_mutex_t *mutex) {
	if (!mutex || !mutex->initialized) return -1;
	if (mutex->owner != g_current) return -1; /* 仅持有者可以解锁 */

	mutex->owner = NULL;
	__sync_lock_release(&mutex->locked);

	/* 唤醒等待队列首部线程（如有） */
	if (mutex->wait_head != NULL) {
		tcb *t = mutex->wait_head;
		mutex->wait_head = t->next;
		if (mutex->wait_head == NULL) mutex->wait_tail = NULL;
		t->next = NULL;
		t->status = READY;
		queue_push(&g_ready, t);
	}
	return 0;
}

/* 销毁互斥锁：要求锁未被占用且无等待者 */
int my_pthread_mutex_destroy(my_pthread_mutex_t *mutex) {
	if (!mutex) return -1;
	if (mutex->locked || mutex->wait_head) return -1; /* 锁被占用或仍有等待者 */
	mutex->initialized = 0;
	return 0;
}
