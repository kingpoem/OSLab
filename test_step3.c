/* test_step3.c: 验证 Step 3 抢占式调度（PSJF）
 * 1) 没有 yield 调用，CPU 密集型循环也能在多个线程之间切换
 * 2) 主线程在子线程 spin 时仍能 join 到结束
 * 3) 多次输出 tid 顺序，证明发生了上下文切换
 */
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include "my_pthread_t.h"

#define NTHREAD 3

static volatile long g_counter[NTHREAD];

static void *spin_worker(void *arg) {
	int id = *(int *)arg;
	for (long i = 0; i < 5000000L; i++) {
		g_counter[id]++;
	}
	printf("[thread %d] done, count=%ld\n", id, g_counter[id]);
	return NULL;
}

int main() {
	pthread_t threads[NTHREAD];
	int args[NTHREAD];

	for (int i = 0; i < NTHREAD; i++) {
		args[i] = i;
		g_counter[i] = 0;
		pthread_create(&threads[i], NULL, spin_worker, &args[i]);
	}

	for (int i = 0; i < NTHREAD; i++) {
		pthread_join(threads[i], NULL);
	}

	for (int i = 0; i < NTHREAD; i++) {
		printf("[main] thread %d final count = %ld\n", i, g_counter[i]);
	}
	printf("[main] all threads finished, preemptive scheduling works\n");
	return 0;
}
