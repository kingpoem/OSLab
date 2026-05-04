/* test_step1.c: 验证 my_pthread 基本 API
 * 测试内容：
 *   1) create + join 能正确取得返回值
 *   2) 多线程交替执行（通过 yield）
 *   3) 主线程 join 多个子线程
 */
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include "my_pthread_t.h"

static int counter = 0;

static void *worker(void *arg) {
	int id = *(int *)arg;
	for (int i = 0; i < 3; i++) {
		counter++;
		printf("[thread %d] iter=%d, counter=%d\n", id, i, counter);
		my_pthread_yield();
	}
	/* 返回 (id+1)*100 作为返回值 */
	return (void *)(long)((id + 1) * 100);
}

int main() {
	const int N = 3;
	pthread_t threads[N];
	int args[N];
	void *retvals[N];

	for (int i = 0; i < N; i++) {
		args[i] = i;
		pthread_create(&threads[i], NULL, worker, &args[i]);
	}

	for (int i = 0; i < N; i++) {
		pthread_join(threads[i], &retvals[i]);
		printf("[main] thread %d returned %ld\n", i, (long)retvals[i]);
	}

	printf("[main] final counter = %d (expected %d)\n", counter, N * 3);
	return 0;
}
