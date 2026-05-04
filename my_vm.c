#include "my_vm.h"
#include <string.h>
#include <pthread.h>

/*
 * 全局虚拟内存管理上下文。
 * 该结构体保存模拟物理内存、磁盘、位图、页目录及同步原语等信息。
 */
typedef struct {
    char *phys_mem;                 // 模拟物理内存的起始地址（通过 malloc 分配）
    char *disk_mem;                 // 模拟磁盘的起始地址（通过 malloc 分配）
    pde_t *page_dir;                // 一级页目录基地址
    char *virt_bitmap;              // 虚拟页位图（每页 1 位）
    char *phys_bitmap;              // 物理页位图（每页 1 位）
    char *disk_bitmap;              // 磁盘页位图（每页 1 位）
    unsigned long num_v_pages;      // 虚拟页总数
    unsigned long num_p_pages;      // 物理页总数
    unsigned long num_d_pages;      // 磁盘页总数
    int offset_bits;                // 页内偏移所占位数 = log2(PAGE_SIZE)
    int pde_bits;                   // 一级页表索引位数（高位）
    int pte_bits;                   // 二级页表索引位数（中位）
    unsigned long num_pde_entries;  // 一级页表项数 = 1 << pde_bits
    unsigned long num_pte_entries;  // 每个二级页表项数 = 1 << pte_bits
    int initialized;                // 是否已初始化
    TLB tlb;                        // TLB 缓存
    pthread_mutex_t vm_lock;        // 虚拟内存全局互斥锁
    pthread_mutex_t tlb_lock;       // TLB 互斥锁
} VMSystem;

static VMSystem g_vm;
/*
 * 仅用于保护初始化阶段的互斥锁，确保只有第一个进入的线程执行初始化。
 */
static pthread_mutex_t g_init_lock = PTHREAD_MUTEX_INITIALIZER;


/*
 * 计算 log2(x)，仅适用于 2 的整数幂。
 * 用于根据 PAGE_SIZE 推导偏移位数。
 */
static int log2_int(unsigned long x) {
    int r = 0;
    while ((x & 1UL) == 0 && x != 0) {
        x >>= 1;
        r++;
    }
    return r;
}

/*
 * 在字符位图的第 index 位写 1。
 */
static void set_bit(char *bitmap, unsigned long index) {
    bitmap[index / 8] |= (char)(1u << (index % 8));
}

/*
 * 在字符位图的第 index 位写 0。
 */
static void clear_bit(char *bitmap, unsigned long index) {
    bitmap[index / 8] &= (char)~(1u << (index % 8));
}

/*
 * 读取字符位图第 index 位的值，返回 0 或 1。
 */
static int get_bit(const char *bitmap, unsigned long index) {
    return (bitmap[index / 8] >> (index % 8)) & 0x1;
}


/*
 * 从虚拟地址中提取一级页表索引（高位 pde_bits 位）。
 */
static unsigned long get_pde_idx(unsigned long va) {
    return va >> (g_vm.pte_bits + g_vm.offset_bits);
}

/*
 * 从虚拟地址中提取二级页表索引（中间 pte_bits 位）。
 */
static unsigned long get_pte_idx(unsigned long va) {
    return (va >> g_vm.offset_bits) & ((1UL << g_vm.pte_bits) - 1);
}

/*
 * 从虚拟地址中提取页内偏移（低 offset_bits 位）。
 */
static unsigned long get_offset(unsigned long va) {
    return va & ((1UL << g_vm.offset_bits) - 1);
}


/*
 * 在物理位图中查找第一个空闲物理页并标记占用，返回物理页号。
 * 若没有空闲物理页则返回 -1。
 * 调用方需自行持有 vm_lock。
 */
static long alloc_phys_page_locked() {
    for (unsigned long i = 0; i < g_vm.num_p_pages; i++) {
        if (!get_bit(g_vm.phys_bitmap, i)) {
            set_bit(g_vm.phys_bitmap, i);
            return (long)i;
        }
    }
    return -1;
}

/*
 * 释放给定物理页号占用的物理页（清除位图位）。
 * 调用方需自行持有 vm_lock。
 */
static void free_phys_page_locked(unsigned long ppn) {
    if (ppn < g_vm.num_p_pages) {
        clear_bit(g_vm.phys_bitmap, ppn);
    }
}


/*
 * 负责分配并设置模拟的物理内存与磁盘空间。
 * 同时计算物理/虚拟页数并初始化对应位图。
 * 该函数仅在首次调用时真正执行初始化，多线程下采用互斥锁保护。
 */
void initMemoryAndDisk() {
    pthread_mutex_lock(&g_init_lock);
    if (g_vm.initialized) {
        pthread_mutex_unlock(&g_init_lock);
        return;
    }

    g_vm.offset_bits = log2_int(PAGE_SIZE);
    int remaining = ADDRESS_BITS - g_vm.offset_bits;
    g_vm.pde_bits = remaining / 2;
    g_vm.pte_bits = remaining - g_vm.pde_bits;
    g_vm.num_pde_entries = 1UL << g_vm.pde_bits;
    g_vm.num_pte_entries = 1UL << g_vm.pte_bits;

    g_vm.num_v_pages = (unsigned long)(VM_SIZE / PAGE_SIZE);
    g_vm.num_p_pages = (unsigned long)(PM_SIZE / PAGE_SIZE);
    g_vm.num_d_pages = (unsigned long)(DISK_SIZE / PAGE_SIZE);

    g_vm.phys_mem = (char *)malloc(PM_SIZE);
    if (!g_vm.phys_mem) {
        fprintf(stderr, "initMemoryAndDisk: failed to allocate physical memory\n");
        pthread_mutex_unlock(&g_init_lock);
        return;
    }
    memset(g_vm.phys_mem, 0, PM_SIZE);

    g_vm.disk_mem = (char *)malloc(DISK_SIZE);
    if (!g_vm.disk_mem) {
        fprintf(stderr, "initMemoryAndDisk: failed to allocate disk memory\n");
        free(g_vm.phys_mem);
        g_vm.phys_mem = NULL;
        pthread_mutex_unlock(&g_init_lock);
        return;
    }
    memset(g_vm.disk_mem, 0, DISK_SIZE);

    unsigned long vbm_bytes = (g_vm.num_v_pages + 7) / 8;
    unsigned long pbm_bytes = (g_vm.num_p_pages + 7) / 8;
    unsigned long dbm_bytes = (g_vm.num_d_pages + 7) / 8;
    g_vm.virt_bitmap = (char *)calloc(1, vbm_bytes);
    g_vm.phys_bitmap = (char *)calloc(1, pbm_bytes);
    g_vm.disk_bitmap = (char *)calloc(1, dbm_bytes);
    if (!g_vm.virt_bitmap || !g_vm.phys_bitmap || !g_vm.disk_bitmap) {
        fprintf(stderr, "initMemoryAndDisk: failed to allocate bitmaps\n");
    }

    /* 一级页目录占用一个物理页：将物理页 0 保留给页目录使用，
     * 这样虚拟地址 0 就不会被分配（与 NULL 区分），同时也方便 PDE/PTE 用 0 表示“未映射”。 */
    g_vm.page_dir = (pde_t *)g_vm.phys_mem;
    memset(g_vm.page_dir, 0, PAGE_SIZE);
    set_bit(g_vm.phys_bitmap, 0);
    /* 同样保留虚拟页 0，防止 myMalloc 返回 NULL（0）。 */
    set_bit(g_vm.virt_bitmap, 0);

    /* 初始化 TLB。 */
    memset(&g_vm.tlb, 0, sizeof(TLB));

    pthread_mutex_init(&g_vm.vm_lock, NULL);
    pthread_mutex_init(&g_vm.tlb_lock, NULL);

    g_vm.initialized = 1;
    pthread_mutex_unlock(&g_init_lock);
}



/*
 * 根据一级页表基地址和虚拟地址进行地址翻译。
 * 返回指向 phys_mem 中对应字节位置的指针（host 指针），调用者可直接读写。
 * 若虚拟地址未建立有效映射则返回 NULL。
 */
pte_t * translate(pde_t *pgdir, void *va) {
    if (!pgdir) return NULL;
    unsigned long vaddr = (unsigned long)va;
    unsigned long pde_idx = get_pde_idx(vaddr);
    unsigned long pte_idx = get_pte_idx(vaddr);
    unsigned long off = get_offset(vaddr);

    pde_t pde = pgdir[pde_idx];
    if (pde == 0) return NULL;

    pte_t *page_table = (pte_t *)(g_vm.phys_mem + pde);
    pte_t pte = page_table[pte_idx];
    if (pte == 0) return NULL;

    return (pte_t *)(g_vm.phys_mem + pte + off);
}


/*
 * 在给定页目录中为虚拟地址 va 建立到物理地址 pa 的映射（pa 为 phys_mem 内的偏移）。
 * 若一级页目录项尚未指向二级页表，则分配一个新物理页用作二级页表。
 * 若 va 已存在映射或资源耗尽则返回 -1，成功返回 0。
 * 调用方需自行持有 vm_lock。
 */
int
pageMap(pde_t *pgdir, void *va, void *pa)
{
    if (!pgdir) return -1;
    unsigned long vaddr = (unsigned long)va;
    unsigned long paddr = (unsigned long)pa;
    unsigned long pde_idx = get_pde_idx(vaddr);
    unsigned long pte_idx = get_pte_idx(vaddr);

    if (pgdir[pde_idx] == 0) {
        long pt_pn = alloc_phys_page_locked();
        if (pt_pn < 0) return -1;
        unsigned long pt_paddr = (unsigned long)pt_pn * PAGE_SIZE;
        memset(g_vm.phys_mem + pt_paddr, 0, PAGE_SIZE);
        pgdir[pde_idx] = (pde_t)pt_paddr;
    }

    pte_t *page_table = (pte_t *)(g_vm.phys_mem + pgdir[pde_idx]);
    if (page_table[pte_idx] != 0) {
        return -1;
    }
    page_table[pte_idx] = (pte_t)paddr;
    return 0;
}


/*
 * 处理缺页错误：当物理页耗尽时进行物理页替换并加载新页面。
 */
int pageFault(pde_t *pgdir, void *va) {
    (void)pgdir;
    (void)va;
    return -1;
}


/*
 * 在 TLB 中查找虚拟地址对应的转换；命中则返回指向对应 PTE 的指针。
 */
pte_t *checkTLB(void *va) {
    (void)va;
    return NULL;
}


/*
 * 将一对 (虚拟地址, 物理地址) 加入 TLB。
 */
int addTLB(void *va, void *pa) {
    (void)va;
    (void)pa;
    return -1;
}


/*
 * 输出 TLB 访问次数和缺失率。
 */
void printTLBStats() {
    unsigned int acc, miss;
    pthread_mutex_lock(&g_vm.tlb_lock);
    acc = g_vm.tlb.tlb_accesses;
    miss = g_vm.tlb.tlb_misses;
    pthread_mutex_unlock(&g_vm.tlb_lock);
    double rate = (acc == 0) ? 0.0 : ((double)miss / (double)acc) * 100.0;
    fprintf(stderr, "TLB Accesses: %u\n", acc);
    fprintf(stderr, "TLB Misses : %u\n", miss);
    fprintf(stderr, "TLB Miss Rate: %.2f%%\n", rate);
}


/*
 * 分配 num_bytes 字节并返回起始虚拟地址。
 * 内部按页粒度分配；若需多于一页则分配连续的虚拟页，
 * 但物理页可不连续。
 */
void *myMalloc(unsigned int num_bytes) {
    (void)num_bytes;
    return NULL;
}


/*
 * 释放从虚拟地址 va 起 size 字节占用的所有页。
 * 仅当所有目标页均已分配时才执行释放并返回成功；否则输出 Segmentation Fault。
 */
void myFree(void *va, int size) {
    (void)va;
    (void)size;
}


/*
 * 将 val 指向的 size 字节数据写入由 va 起始的虚拟内存。
 * 若 size 跨越多页，循环按页拷贝。
 */
void myWrite(void *va, void *val, int size) {
    (void)va;
    (void)val;
    (void)size;
}


/*
 * 从虚拟地址 va 起读取 size 字节到 val 指向的缓冲区。
 * 写读流程必须先经过 TLB 查询并在缺失时回填。
 */
void myRead(void *va, void *val, int size) {
    (void)va;
    (void)val;
    (void)size;
}
