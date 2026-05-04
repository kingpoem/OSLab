#ifndef __MY_VM_H__
#define __MY_VM_H__

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include "defines.h"

/*

Illustration of a 2-level page table with 4KB pages.
In this example, lower 12 bits are used for page offset, and rest of
the bits are divided equally into 2 levels.

 31      22 21      12 11        0
+----------+----------+----------+
| PDE (L2) | PTE (L1) |  OFFSET  |
+----------+----------+----------+

*/

// Represents a page directory entry
typedef unsigned long pde_t;

// Represents a page table entry
typedef unsigned long pte_t;

// TLB data structure example. Feel free to modify
typedef struct {
	struct {
		bool valid;  // valid bit
		unsigned long v_page;  // virtual page number
		unsigned long p_page;  // physical page number
	} entry[TLB_SIZE];
    unsigned int tlb_accesses;
    unsigned int tlb_misses;
} TLB;

void initMemoryAndDisk();
pte_t* translate(pde_t *pgdir, void *va);
int pageMap(pde_t *pgdir, void *va, void* pa);
int pageFault(pde_t *pgdir, void *va);
pte_t *checkTLB(void *va);
int addTLB(void *va, void *pa);

void myFree(void *va, int size);
void *myMalloc(unsigned int num_bytes);
void myWrite(void *va, void *val, int size);
void myRead(void *va, void *val, int size);

#endif
