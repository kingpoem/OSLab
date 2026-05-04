/* test_step4.c: 验证 MLFQ 调度策略
 * 创建两类线程：
 *   - "io_like"：每次 yield，模拟 IO 密集型；应当保持高优先级
 *   - "cpu_like"：紧凑循环，模拟 CPU 密集型；很快被降级
 * 同时验证混合执行下两类线程都能完成。
 */
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include "my_pthread_t.h"

#define IO_THREADS  2
#define CPU_THREADS 2

static int io_done[IO_THREADS];
static long cpu_done[CPU_THREADS];

static void *io_like(void *arg) {
	int id = *(int *)arg;
	for (int i = 0; i < 50; i++) {
		io_done[id]++;
		my_pthread_yield();   /* 频繁让出 CPU，模拟 IO 等待 */
	}
	printf("[io thread %d] done, count=%d\n", id, io_done[id]);
	return NULL;
}

static void *cpu_like(void *arg) {
	int id = *(int *)arg;
	for (long i = 0; i < 3000000L; i++) {
		cpu_done[id]++;
	}
	printf("[cpu thread %d] done, count=%ld\n", id, cpu_done[id]);
	return NULL;
}

int main() {
	pthread_t io_t[IO_THREADS], cpu_t[CPU_THREADS];
	int io_args[IO_THREADS], cpu_args[CPU_THREADS];

	for (int i = 0; i < IO_THREADS; i++) {
		io_args[i] = i;
		io_done[i] = 0;
		pthread_create(&io_t[i], NULL, io_like, &io_args[i]);
	}
	for (int i = 0; i < CPU_THREADS; i++) {
		cpu_args[i] = i;
		cpu_done[i] = 0;
		pthread_create(&cpu_t[i], NULL, cpu_like, &cpu_args[i]);
	}

	for (int i = 0; i < IO_THREADS; i++) pthread_join(io_t[i], NULL);
	for (int i = 0; i < CPU_THREADS; i++) pthread_join(cpu_t[i], NULL);

	printf("[main] all threads finished\n");
	return 0;
}
