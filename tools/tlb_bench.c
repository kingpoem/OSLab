/*
 * tlb_bench.c
 *
 * 自定义基准程序：在 my_vm 上构造若干典型访问模式，统计耗时与 TLB 命中情况。
 * 与 benchmark/ 下的官方测试不同，本程序专门用于评估 TLB 行为，并通过
 * printTLBStats() 输出累计访问次数与缺失率。
 *
 * 三种模式：
 *   1) sequential : 顺序访问大块缓冲区（局部性极好，应当 TLB 命中率高）
 *   2) strided    : 跨页跳跃访问（局部性差，TLB 缺失率显著上升）
 *   3) matmul     : 矩阵乘法，混合访问模式（与官方 test.c 一致，作为对照）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../my_vm.h"


/*
 * 计算两个 timespec 差值，返回毫秒。
 */
static double diff_ms(struct timespec a, struct timespec b) {
    double s = (double)(b.tv_sec - a.tv_sec) * 1000.0;
    s += (double)(b.tv_nsec - a.tv_nsec) / 1e6;
    return s;
}


/*
 * 顺序访问基准：分配一块连续内存，按 4 字节步长依次写入再依次读出。
 */
static void bench_sequential(unsigned int total_bytes) {
    unsigned int n = total_bytes / sizeof(int);
    void *buf = myMalloc(total_bytes);
    if (!buf) { printf("[seq] alloc fail\n"); return; }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (unsigned int i = 0; i < n; i++) {
        unsigned int addr = (unsigned int)buf + i * sizeof(int);
        int v = (int)i;
        myWrite((void *)addr, &v, sizeof(int));
    }
    int sum = 0;
    for (unsigned int i = 0; i < n; i++) {
        unsigned int addr = (unsigned int)buf + i * sizeof(int);
        int v;
        myRead((void *)addr, &v, sizeof(int));
        sum += v;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("[seq    ] total=%u ints  sum=%d  time=%.2fms\n", n, sum, diff_ms(t0, t1));

    myFree(buf, total_bytes);
}


/*
 * 跨页跳跃访问基准：每次跨越一个页面，使 TLB 不易命中。
 */
static void bench_strided(unsigned int total_bytes, unsigned int stride) {
    void *buf = myMalloc(total_bytes);
    if (!buf) { printf("[stride] alloc fail\n"); return; }

    unsigned int steps = total_bytes / stride;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (unsigned int rep = 0; rep < 3; rep++) {
        for (unsigned int i = 0; i < steps; i++) {
            unsigned int addr = (unsigned int)buf + i * stride;
            int v = (int)i + (int)rep;
            myWrite((void *)addr, &v, sizeof(int));
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("[stride ] stride=%u steps=%u(x3)  time=%.2fms\n",
           stride, steps, diff_ms(t0, t1));

    myFree(buf, total_bytes);
}


/*
 * 矩阵乘法基准：与官方 test.c 算法一致，便于 TLB 数据具有可比性。
 */
static void bench_matmul(int size) {
    int bytes = size * size * sizeof(int);
    void *a = myMalloc(bytes);
    void *b = myMalloc(bytes);
    void *c = myMalloc(bytes);
    if (!a || !b || !c) { printf("[matmul] alloc fail\n"); return; }

    int x = 1;
    for (int i = 0; i < size; i++) {
        for (int j = 0; j < size; j++) {
            unsigned int addr_a = (unsigned int)a + (i * size + j) * sizeof(int);
            unsigned int addr_b = (unsigned int)b + (i * size + j) * sizeof(int);
            myWrite((void *)addr_a, &x, sizeof(int));
            myWrite((void *)addr_b, &x, sizeof(int));
        }
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < size; i++) {
        for (int j = 0; j < size; j++) {
            int total = 0, n1, n2;
            for (int k = 0; k < size; k++) {
                unsigned int addr_a = (unsigned int)a + (i * size + k) * sizeof(int);
                unsigned int addr_b = (unsigned int)b + (k * size + j) * sizeof(int);
                myRead((void *)addr_a, &n1, sizeof(int));
                myRead((void *)addr_b, &n2, sizeof(int));
                total += n1 * n2;
            }
            unsigned int addr_c = (unsigned int)c + (i * size + j) * sizeof(int);
            myWrite((void *)addr_c, &total, sizeof(int));
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("[matmul ] size=%dx%d  time=%.2fms\n", size, size, diff_ms(t0, t1));

    myFree(a, bytes);
    myFree(b, bytes);
    myFree(c, bytes);
}


int main(int argc, char **argv) {
    /* 默认参数；命令行可覆盖：
     *   argv[1] = sequential 总字节数
     *   argv[2] = stride 总字节数
     *   argv[3] = stride 步长
     *   argv[4] = matmul 矩阵尺寸 N
     */
    unsigned int seq_bytes  = (argc > 1) ? (unsigned int)atoi(argv[1]) : (256 * 1024);
    unsigned int str_bytes  = (argc > 2) ? (unsigned int)atoi(argv[2]) : (1 * 1024 * 1024);
    unsigned int str_stride = (argc > 3) ? (unsigned int)atoi(argv[3]) : 4096;
    int          mat_n      = (argc > 4) ? atoi(argv[4]) : 30;

    initMemoryAndDisk();

    bench_sequential(seq_bytes);
    bench_strided(str_bytes, str_stride);
    bench_matmul(mat_n);

    /* TLB 总体统计（累计三种模式） */
    printTLBStats();
    return 0;
}
