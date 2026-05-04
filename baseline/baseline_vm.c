/*
 * baseline_vm.c
 *
 * 基线实现：使用 C 标准库 (malloc/free/memcpy) 直接模拟 my_vm.h 中的 API。
 * 该实现不做分页、TLB 模拟，仅用于与自定义实现做正确性与吞吐对照。
 * 与自定义实现共用相同的头文件，编译为 libbaseline_vm.a，链接同一份 benchmark
 * 即可复用全部测试程序，便于对比两份运行日志。
 */
#include "../my_vm.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>


/*
 * 基线无需任何全局初始化；保持函数存在以满足链接需要。
 */
void initMemoryAndDisk() {
}


/*
 * 直接调用 libc malloc 返回分配地址。
 */
void *myMalloc(unsigned int num_bytes) {
    if (num_bytes == 0) return NULL;
    return malloc(num_bytes);
}


/*
 * 调用 libc free 释放；size 参数被忽略（libc 通过内部簿记知道实际大小）。
 * NULL 视为非法释放，输出 Segmentation Fault 与自定义实现保持语义一致。
 */
void myFree(void *va, int size) {
    (void)size;
    if (!va) {
        printf("Segmentation Fault\n");
        return;
    }
    free(va);
}


/*
 * memcpy 写入；va 为 NULL 时输出错误信息以匹配自定义实现的 ERROR 提示。
 */
void myWrite(void *va, void *val, int size) {
    if (!va || !val || size <= 0) {
        printf("ERROR: Writing to unallocated address\n");
        return;
    }
    memcpy(va, val, size);
}


/*
 * memcpy 读取；va 为 NULL 时输出错误信息。
 */
void myRead(void *va, void *val, int size) {
    if (!va || !val || size <= 0) {
        printf("ERROR: Reading from unallocated address\n");
        return;
    }
    memcpy(val, va, size);
}


/*
 * 以下函数仅为满足链接需要而存在，基线实现不使用页表/TLB。
 */
pte_t *translate(pde_t *pgdir, void *va) { (void)pgdir; return (pte_t *)va; }
int pageMap(pde_t *pgdir, void *va, void *pa) { (void)pgdir; (void)va; (void)pa; return 0; }
int pageFault(pde_t *pgdir, void *va) { (void)pgdir; (void)va; return 0; }
pte_t *checkTLB(void *va) { (void)va; return NULL; }
int addTLB(void *va, void *pa) { (void)va; (void)pa; return 0; }


/*
 * 基线没有 TLB，统计始终为零。仍然按相同格式输出便于脚本解析。
 */
void printTLBStats() {
    fprintf(stderr, "TLB Accesses : 0\n");
    fprintf(stderr, "TLB Misses   : 0\n");
    fprintf(stderr, "TLB Miss Rate: 0.00%%\n");
}
