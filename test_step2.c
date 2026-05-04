/* test_step2.c: 验证 my_pthread mutex
 * 多个线程并发对共享变量加 N 次，使用互斥锁保护，最终值必须等于 thread_num*N。
 */
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include "my_pthread_t.h"

#define NTHREAD 4
#define NITER   100

static int shared = 0;
static pthread_mutex_t mtx;

static void *worker(void *arg) {
	int id = *(int *)arg;
	for (int i = 0; i < NITER; i++) {
		pthread_mutex_lock(&mtx);
		int tmp = shared;
		my_pthread_yield();   /* 故意 yield 制造冲突机会 */
		shared = tmp + 1;
		pthread_mutex_unlock(&mtx);
	}
	(void)id;
	return NULL;
}

int main() {
	pthread_t threads[NTHREAD];
	int args[NTHREAD];

	pthread_mutex_init(&mtx, NULL);
	for (int i = 0; i < NTHREAD; i++) {
		args[i] = i;
		pthread_create(&threads[i], NULL, worker, &args[i]);
	}
	for (int i = 0; i < NTHREAD; i++) {
		pthread_join(threads[i], NULL);
	}
	pthread_mutex_destroy(&mtx);

	int expected = NTHREAD * NITER;
	printf("[main] shared = %d (expected %d) -> %s\n",
	       shared, expected, shared == expected ? "PASS" : "FAIL");
	return shared == expected ? 0 : 1;
}
