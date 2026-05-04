#ifndef __DEFINES_H__
#define __DEFINES_H__


// 32-bit address space, 
// out of which 48 bits are used in modern OS.
#define ADDRESS_BITS 32

// The virtual memory size is set to 4GB.
#define VM_SIZE 4ULL*1024*1024*1024

// Size of the simulated disk spece
#define DISK_SIZE 1ULL*1024*1024*1024  // 1GB


// Size of the simulated physical memory
// We will modify this value when testing your program
#define PM_SIZE 64*1024*1024  // 64MB

// Size of a single page
// We will modify this value when testing your program
#define PAGE_SIZE 4096

// TLB entries
// We will modify this value when testing your program
#define TLB_SIZE 32

#endif
