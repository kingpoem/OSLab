// File:	my_pthread_t.h
// Author:	Yujie REN, Jieming Yin
// Date:	April 2025

#ifndef MY_PTHREAD_T_H
#define MY_PTHREAD_T_H

#define _GNU_SOURCE

/* To use real pthread Library in Benchmark, you have to comment the USE_MY_PTHREAD macro */
#define USE_MY_PTHREAD 1

/* include lib header files that you need here: */
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <signal.h>
#include <semaphore.h>
#include <sys/time.h>

/* defile necessary MACRO here, for example, thread upper bound,
   stack size, priority queue levels, time quantum, etc. */
#define STACK_SIZE       (64 * 1024)   // 每个线程栈大小：64KB
#define MAX_THREADS      256           // 系统最多可创建的线程数（含主线程）
#define TIME_QUANTUM_MS  10            // 时间片长度（毫秒）
#define MLFQ_LEVELS      4             // MLFQ 优先级队列级数

typedef unsigned int my_pthread_t;

// Thread Status
typedef enum threadStatus {
	NOT_STARTED = 0,  // 未启动
	RUNNING,          // 正在运行
	READY,            // 就绪（在 ready 队列）
	BLOCKED,          // 阻塞（等待 mutex 或 join）
	FINISHED,         // 已终止
} threadStatus;

// Schedule Policy
typedef enum schedPolicy {
	POLICY_RR = 0,    // 轮转（最简单的 FCFS / RR）
	POLICY_MLFQ,      // 多级反馈队列
	POLICY_PSJF       // 抢占式最短作业优先
} schedPolicy;

typedef struct threadControlBlock {
	my_pthread_t tid;                       // 线程ID
	threadStatus status;                    // 线程状态
	ucontext_t   ctx;                       // 线程上下文
	void        *stack;                     // 线程栈起始地址
	void        *retval;                    // 线程返回值
	struct threadControlBlock *joiner;      // 等待该线程结束的线程（最多一个）
	int          time_slices;               // 已运行时间片计数（用于 PSJF）
	int          priority;                  // 当前优先级（MLFQ 用，0 最高）
	void *(*function)(void *);              // 入口函数
	void        *arg;                       // 入口函数参数
	struct threadControlBlock *next;        // 队列中下一节点指针
} tcb;

/* mutex struct definition */
typedef struct my_pthread_mutex_t {
	int          locked;       // 锁状态：0 空闲，1 已占用
	int          initialized;  // 是否已初始化
	tcb         *owner;        // 当前持有者
	tcb         *wait_head;    // 等待队列头
	tcb         *wait_tail;    // 等待队列尾
} my_pthread_mutex_t;

/* simple thread queue (singly-linked list, head/tail) */
typedef struct threadQueue {
	tcb *head;  // 队列头
	tcb *tail;  // 队列尾
} threadQueue;


/* Function Declarations: */

/* create a new thread */
int my_pthread_create(my_pthread_t * thread, pthread_attr_t * attr, void *(*function)(void*), void * arg);

/* give CPU pocession to other user level threads voluntarily */
int my_pthread_yield();

/* terminate a thread */
void my_pthread_exit(void *value_ptr);

/* wait for thread termination */
int my_pthread_join(my_pthread_t thread, void **value_ptr);

/* initial the mutex lock */
int my_pthread_mutex_init(my_pthread_mutex_t *mutex, const pthread_mutexattr_t *mutexattr);

/* aquire the mutex lock */
int my_pthread_mutex_lock(my_pthread_mutex_t *mutex);

/* release the mutex lock */
int my_pthread_mutex_unlock(my_pthread_mutex_t *mutex);

/* destroy the mutex */
int my_pthread_mutex_destroy(my_pthread_mutex_t *mutex);

#ifdef USE_MY_PTHREAD
#define pthread_t my_pthread_t
#define pthread_mutex_t my_pthread_mutex_t
#define pthread_create my_pthread_create
#define pthread_exit my_pthread_exit
#define pthread_join my_pthread_join
#define pthread_mutex_init my_pthread_mutex_init
#define pthread_mutex_lock my_pthread_mutex_lock
#define pthread_mutex_unlock my_pthread_mutex_unlock
#define pthread_mutex_destroy my_pthread_mutex_destroy
#endif

#endif
