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
    long *ppn_to_vpn;               // 反向映射：物理页号 → 虚拟页号；-1 空闲，-2 用作页表
    long *vpn_to_disk;              // 虚拟页 → 磁盘页号（被换出时）；-1 表示未在磁盘
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
 * 释放给定物理页号占用的物理页（清除位图位与反向映射）。
 * 调用方需自行持有 vm_lock。
 */
static void free_phys_page_locked(unsigned long ppn) {
    if (ppn < g_vm.num_p_pages) {
        clear_bit(g_vm.phys_bitmap, ppn);
        g_vm.ppn_to_vpn[ppn] = -1;
    }
}

/*
 * 在磁盘位图中查找一个空闲磁盘页，返回页号；找不到返回 -1。
 * 调用方需自行持有 vm_lock。
 */
static long alloc_disk_page_locked() {
    for (unsigned long i = 0; i < g_vm.num_d_pages; i++) {
        if (!get_bit(g_vm.disk_bitmap, i)) {
            set_bit(g_vm.disk_bitmap, i);
            return (long)i;
        }
    }
    return -1;
}

/*
 * 释放给定磁盘页号。调用方需自行持有 vm_lock。
 */
static void free_disk_page_locked(unsigned long dpn) {
    if (dpn < g_vm.num_d_pages) {
        clear_bit(g_vm.disk_bitmap, dpn);
    }
}

/*
 * 在虚拟位图中查找 n 个连续的空闲虚拟页，返回起始虚拟页号；找不到返回 -1。
 * 起点跳过虚拟页 0（已保留为非法地址）。
 * 调用方需自行持有 vm_lock。
 */
static long find_free_vpages_locked(unsigned long n) {
    if (n == 0) return -1;
    unsigned long start = 1;
    unsigned long count = 0;
    for (unsigned long i = 1; i < g_vm.num_v_pages; i++) {
        if (!get_bit(g_vm.virt_bitmap, i)) {
            if (count == 0) start = i;
            count++;
            if (count == n) return (long)start;
        } else {
            count = 0;
        }
    }
    return -1;
}

/*
 * 清除给定虚拟地址对应的页表项（PTE）；返回原物理地址（pa），失败返回 0。
 * 调用方需自行持有 vm_lock。
 */
static unsigned long clear_pte_locked(pde_t *pgdir, unsigned long vaddr) {
    unsigned long pde_idx = get_pde_idx(vaddr);
    unsigned long pte_idx = get_pte_idx(vaddr);
    if (pgdir[pde_idx] == 0) return 0;
    pte_t *page_table = (pte_t *)(g_vm.phys_mem + pgdir[pde_idx]);
    unsigned long old = (unsigned long)page_table[pte_idx];
    page_table[pte_idx] = 0;
    return old;
}

/*
 * 检查从 vaddr 起 size 字节区间内所有虚拟页是否均已分配。
 * 全部已分配返回 1，否则返回 0。
 * 调用方需自行持有 vm_lock。
 */
static int validate_range_locked(unsigned long vaddr, unsigned long size) {
    if (size == 0) return 1;
    unsigned long start_vpn = vaddr / PAGE_SIZE;
    unsigned long end_vpn = (vaddr + size - 1) / PAGE_SIZE;
    if (start_vpn == 0) return 0;  // 虚拟页 0 永远非法
    if (end_vpn >= g_vm.num_v_pages) return 0;
    for (unsigned long vpn = start_vpn; vpn <= end_vpn; vpn++) {
        if (!get_bit(g_vm.virt_bitmap, vpn)) return 0;
    }
    return 1;
}

/* 前向声明：换出/带换出的物理页分配，定义在文件后部。 */
static long evict_one_locked(void);
static long alloc_phys_page_with_evict_locked(void);

/*
 * 在 TLB 中查找并使包含 vpn 的条目失效。
 * 用于 myFree 等操作释放虚拟页时刷新 TLB。
 */
static void invalidateTLBEntry(unsigned long vpn) {
    pthread_mutex_lock(&g_vm.tlb_lock);
    for (int i = 0; i < TLB_SIZE; i++) {
        if (g_vm.tlb.entry[i].valid && g_vm.tlb.entry[i].v_page == vpn) {
            g_vm.tlb.entry[i].valid = false;
        }
    }
    pthread_mutex_unlock(&g_vm.tlb_lock);
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
    g_vm.ppn_to_vpn = (long *)malloc(sizeof(long) * g_vm.num_p_pages);
    g_vm.vpn_to_disk = (long *)malloc(sizeof(long) * g_vm.num_v_pages);
    if (!g_vm.virt_bitmap || !g_vm.phys_bitmap || !g_vm.disk_bitmap
        || !g_vm.ppn_to_vpn || !g_vm.vpn_to_disk) {
        fprintf(stderr, "initMemoryAndDisk: failed to allocate bitmaps/maps\n");
        pthread_mutex_unlock(&g_init_lock);
        return;
    }
    for (unsigned long i = 0; i < g_vm.num_p_pages; i++) g_vm.ppn_to_vpn[i] = -1;
    for (unsigned long i = 0; i < g_vm.num_v_pages; i++) g_vm.vpn_to_disk[i] = -1;

    /* 一级页目录占用一个物理页：将物理页 0 保留给页目录使用，
     * 这样虚拟地址 0 就不会被分配（与 NULL 区分），同时也方便 PDE/PTE 用 0 表示“未映射”。 */
    g_vm.page_dir = (pde_t *)g_vm.phys_mem;
    memset(g_vm.page_dir, 0, PAGE_SIZE);
    set_bit(g_vm.phys_bitmap, 0);
    g_vm.ppn_to_vpn[0] = -2;  // 标记物理页 0 为页目录占用，禁止换出
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
 * 流程：先查 TLB；若命中则直接返回；缺失则走两级页表，命中后回填 TLB。
 * 返回指向 phys_mem 中对应字节位置的指针（host 指针），可直接读写。
 * 若虚拟地址未建立有效映射则返回 NULL。
 */
pte_t * translate(pde_t *pgdir, void *va) {
    if (!pgdir) return NULL;

    /* 仅当 pgdir 是当前全局页目录时使用 TLB（TLB 与 page_dir 绑定）。 */
    if (pgdir == g_vm.page_dir) {
        pte_t *cached = checkTLB(va);
        if (cached) return cached;
    }

    unsigned long vaddr = (unsigned long)va;
    unsigned long pde_idx = get_pde_idx(vaddr);
    unsigned long pte_idx = get_pte_idx(vaddr);
    unsigned long off = get_offset(vaddr);

    pde_t pde = pgdir[pde_idx];
    pte_t *page_table = (pde == 0) ? NULL : (pte_t *)(g_vm.phys_mem + pde);
    pte_t pte = page_table ? page_table[pte_idx] : 0;

    /* 若 PTE 为 0 但磁盘上有该 vpn 的备份，触发缺页换入。 */
    if (pte == 0 && pgdir == g_vm.page_dir) {
        unsigned long vpn = vaddr / PAGE_SIZE;
        if (vpn < g_vm.num_v_pages && g_vm.vpn_to_disk[vpn] >= 0) {
            if (pageFault(pgdir, va) == 0) {
                pde = pgdir[pde_idx];
                page_table = (pte_t *)(g_vm.phys_mem + pde);
                pte = page_table[pte_idx];
            }
        }
    }
    if (pte == 0) return NULL;

    /* 页表命中，回填 TLB（仅对当前全局页目录）。 */
    if (pgdir == g_vm.page_dir) {
        addTLB(va, (void *)(unsigned long)pte);
    }

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
        long pt_pn = alloc_phys_page_with_evict_locked();
        if (pt_pn < 0) return -1;
        unsigned long pt_paddr = (unsigned long)pt_pn * PAGE_SIZE;
        memset(g_vm.phys_mem + pt_paddr, 0, PAGE_SIZE);
        pgdir[pde_idx] = (pde_t)pt_paddr;
        g_vm.ppn_to_vpn[pt_pn] = -2;  // 标记为二级页表，不可被换出
    }

    pte_t *page_table = (pte_t *)(g_vm.phys_mem + pgdir[pde_idx]);
    if (page_table[pte_idx] != 0) {
        return -1;
    }
    page_table[pte_idx] = (pte_t)paddr;
    /* 维护反向映射：仅对数据页（paddr 对应的物理页号），仅当 pgdir 为全局页目录时。 */
    if (pgdir == g_vm.page_dir) {
        unsigned long ppn = paddr / PAGE_SIZE;
        unsigned long vpn = vaddr / PAGE_SIZE;
        if (ppn < g_vm.num_p_pages) g_vm.ppn_to_vpn[ppn] = (long)vpn;
    }
    return 0;
}


/*
 * 选择一个 victim 物理数据页，将其内容写入磁盘并断开原映射，返回该物理页号。
 * 替换策略：FIFO（用静态 cursor 在合法物理页上轮转）。
 * 若没有可换出的页（例如物理内存全部被页目录/二级页表占用）则返回 -1。
 * 调用方需自行持有 vm_lock。
 */
static long evict_one_locked() {
    static unsigned long fifo_ppn_cursor = 0;
    unsigned long n = g_vm.num_p_pages;
    if (n == 0) return -1;

    for (unsigned long step = 0; step < n; step++) {
        unsigned long ppn = (fifo_ppn_cursor + step) % n;
        long owner_vpn = g_vm.ppn_to_vpn[ppn];
        if (owner_vpn < 0) continue;  // -1 空闲（不该选） / -2 是页表

        long dpn = alloc_disk_page_locked();
        if (dpn < 0) return -1;  // 磁盘也满了

        memcpy(g_vm.disk_mem + (unsigned long)dpn * PAGE_SIZE,
               g_vm.phys_mem + ppn * PAGE_SIZE,
               PAGE_SIZE);

        unsigned long vaddr = (unsigned long)owner_vpn * PAGE_SIZE;
        clear_pte_locked(g_vm.page_dir, vaddr);
        g_vm.vpn_to_disk[owner_vpn] = dpn;

        invalidateTLBEntry((unsigned long)owner_vpn);

        clear_bit(g_vm.phys_bitmap, ppn);
        g_vm.ppn_to_vpn[ppn] = -1;

        fifo_ppn_cursor = (ppn + 1) % n;
        return (long)ppn;
    }
    return -1;
}

/*
 * 分配一个空闲物理页；若无空闲则触发 evict_one_locked 换出一页再分配。
 * 调用方需自行持有 vm_lock。
 */
static long alloc_phys_page_with_evict_locked() {
    long ppn = alloc_phys_page_locked();
    if (ppn >= 0) return ppn;
    long victim = evict_one_locked();
    if (victim < 0) return -1;
    set_bit(g_vm.phys_bitmap, (unsigned long)victim);
    return victim;
}

/*
 * 处理缺页错误：将给定虚拟地址对应的页从磁盘换回内存。
 * 调用前提：va 已分配（virt_bitmap 为 1）但当前 PTE 为 0，且 vpn_to_disk[vpn] >= 0。
 * 成功返回 0，失败返回 -1。
 * 调用方需自行持有 vm_lock。
 */
int pageFault(pde_t *pgdir, void *va) {
    if (!pgdir) return -1;
    unsigned long vaddr = (unsigned long)va;
    unsigned long vpn = vaddr / PAGE_SIZE;
    if (vpn >= g_vm.num_v_pages) return -1;

    long dpn = g_vm.vpn_to_disk[vpn];
    if (dpn < 0) return -1;  // 不在磁盘上，无法换入

    long ppn = alloc_phys_page_with_evict_locked();
    if (ppn < 0) return -1;

    memcpy(g_vm.phys_mem + (unsigned long)ppn * PAGE_SIZE,
           g_vm.disk_mem + (unsigned long)dpn * PAGE_SIZE,
           PAGE_SIZE);

    free_disk_page_locked((unsigned long)dpn);
    g_vm.vpn_to_disk[vpn] = -1;

    /* 重建 PTE。 */
    unsigned long pde_idx = get_pde_idx(vaddr);
    unsigned long pte_idx = get_pte_idx(vaddr);
    if (pgdir[pde_idx] == 0) {
        long pt_pn = alloc_phys_page_with_evict_locked();
        if (pt_pn < 0) return -1;
        unsigned long pt_paddr = (unsigned long)pt_pn * PAGE_SIZE;
        memset(g_vm.phys_mem + pt_paddr, 0, PAGE_SIZE);
        pgdir[pde_idx] = (pde_t)pt_paddr;
        g_vm.ppn_to_vpn[pt_pn] = -2;
    }
    pte_t *page_table = (pte_t *)(g_vm.phys_mem + pgdir[pde_idx]);
    page_table[pte_idx] = (pte_t)((unsigned long)ppn * PAGE_SIZE);
    g_vm.ppn_to_vpn[ppn] = (long)vpn;
    return 0;
}


/*
 * 在 TLB 中查找虚拟地址 va 对应的转换。
 * 累加 tlb_accesses；命中时返回指向 phys_mem 中对应字节的 host 指针，
 * 缺失时累加 tlb_misses 并返回 NULL。
 */
pte_t *checkTLB(void *va) {
    if (!g_vm.initialized) return NULL;
    unsigned long vaddr = (unsigned long)va;
    unsigned long vpn = vaddr / PAGE_SIZE;
    unsigned long off = vaddr % PAGE_SIZE;

    pthread_mutex_lock(&g_vm.tlb_lock);
    g_vm.tlb.tlb_accesses++;
    for (int i = 0; i < TLB_SIZE; i++) {
        if (g_vm.tlb.entry[i].valid && g_vm.tlb.entry[i].v_page == vpn) {
            unsigned long ppn = g_vm.tlb.entry[i].p_page;
            pthread_mutex_unlock(&g_vm.tlb_lock);
            return (pte_t *)(g_vm.phys_mem + ppn * PAGE_SIZE + off);
        }
    }
    g_vm.tlb.tlb_misses++;
    pthread_mutex_unlock(&g_vm.tlb_lock);
    return NULL;
}


/*
 * 将一对 (va, pa) 的虚拟页号到物理页号的映射写入 TLB。
 * 若已存在相同 vpn 的项则更新，否则优先写入空闲项；
 * 若 TLB 已满，按 FIFO 策略选择一个旧条目替换。
 */
int addTLB(void *va, void *pa) {
    if (!g_vm.initialized) return -1;
    unsigned long vpn = (unsigned long)va / PAGE_SIZE;
    unsigned long ppn = (unsigned long)pa / PAGE_SIZE;

    /* FIFO 替换游标，使用静态变量在函数间保持状态。 */
    static int fifo_cursor = 0;

    pthread_mutex_lock(&g_vm.tlb_lock);
    int free_slot = -1;
    for (int i = 0; i < TLB_SIZE; i++) {
        if (g_vm.tlb.entry[i].valid && g_vm.tlb.entry[i].v_page == vpn) {
            g_vm.tlb.entry[i].p_page = ppn;
            pthread_mutex_unlock(&g_vm.tlb_lock);
            return 0;
        }
        if (!g_vm.tlb.entry[i].valid && free_slot < 0) free_slot = i;
    }
    int slot;
    if (free_slot >= 0) {
        slot = free_slot;
    } else {
        slot = fifo_cursor;
        fifo_cursor = (fifo_cursor + 1) % TLB_SIZE;
    }
    g_vm.tlb.entry[slot].valid = true;
    g_vm.tlb.entry[slot].v_page = vpn;
    g_vm.tlb.entry[slot].p_page = ppn;
    pthread_mutex_unlock(&g_vm.tlb_lock);
    return 0;
}


/*
 * 输出 TLB 访问次数和缺失率（缺失率 = 缺失次数 / 访问次数 × 100%）。
 */
void printTLBStats() {
    unsigned int acc, miss;
    pthread_mutex_lock(&g_vm.tlb_lock);
    acc = g_vm.tlb.tlb_accesses;
    miss = g_vm.tlb.tlb_misses;
    pthread_mutex_unlock(&g_vm.tlb_lock);
    double rate = (acc == 0) ? 0.0 : ((double)miss / (double)acc) * 100.0;
    fprintf(stderr, "TLB Accesses : %u\n", acc);
    fprintf(stderr, "TLB Misses   : %u\n", miss);
    fprintf(stderr, "TLB Miss Rate: %.2f%%\n", rate);
}


/*
 * 分配 num_bytes 字节并返回起始虚拟地址。
 * 内部按页粒度分配；若需多于一页则分配连续的虚拟页，
 * 但对应的物理页可不连续。
 * 物理内存不足时返回 NULL。
 */
void *myMalloc(unsigned int num_bytes) {
    if (num_bytes == 0) return NULL;
    if (!g_vm.initialized) initMemoryAndDisk();
    if (!g_vm.initialized) return NULL;

    unsigned long n_pages = (num_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    pthread_mutex_lock(&g_vm.vm_lock);

    long start_vpn = find_free_vpages_locked(n_pages);
    if (start_vpn < 0) {
        pthread_mutex_unlock(&g_vm.vm_lock);
        return NULL;
    }

    /* 为每一虚拟页分配并映射一个物理页。失败时回滚。 */
    unsigned long allocated_ppns[n_pages];
    for (unsigned long i = 0; i < n_pages; i++) allocated_ppns[i] = (unsigned long)-1;

    int ok = 1;
    for (unsigned long i = 0; i < n_pages; i++) {
        long ppn = alloc_phys_page_with_evict_locked();
        if (ppn < 0) { ok = 0; break; }
        allocated_ppns[i] = (unsigned long)ppn;
        unsigned long vaddr = (unsigned long)(start_vpn + i) * PAGE_SIZE;
        unsigned long paddr = (unsigned long)ppn * PAGE_SIZE;
        if (pageMap(g_vm.page_dir, (void *)vaddr, (void *)paddr) != 0) {
            ok = 0;
            break;
        }
        set_bit(g_vm.virt_bitmap, (unsigned long)(start_vpn + i));
    }

    if (!ok) {
        for (unsigned long i = 0; i < n_pages; i++) {
            if (allocated_ppns[i] != (unsigned long)-1) {
                free_phys_page_locked(allocated_ppns[i]);
            }
            unsigned long vpn = (unsigned long)(start_vpn + i);
            if (get_bit(g_vm.virt_bitmap, vpn)) {
                clear_pte_locked(g_vm.page_dir, vpn * PAGE_SIZE);
                clear_bit(g_vm.virt_bitmap, vpn);
            }
        }
        pthread_mutex_unlock(&g_vm.vm_lock);
        return NULL;
    }

    pthread_mutex_unlock(&g_vm.vm_lock);
    return (void *)((unsigned long)start_vpn * PAGE_SIZE);
}


/*
 * 释放从虚拟地址 va 起 size 字节占用的所有页（按页粒度）。
 * 仅当所有目标页均已分配时才执行释放；否则输出 Segmentation Fault 且不释放任何页。
 */
void myFree(void *va, int size) {
    if (size <= 0) return;
    if (!g_vm.initialized) {
        printf("Segmentation Fault\n");
        return;
    }

    unsigned long vaddr = (unsigned long)va;
    /* 仅按页对齐的起始地址释放：要求 va 与 myMalloc 返回值相同含义。 */
    unsigned long start_vpn = vaddr / PAGE_SIZE;
    unsigned long n_pages = ((unsigned long)size + PAGE_SIZE - 1) / PAGE_SIZE;

    pthread_mutex_lock(&g_vm.vm_lock);

    /* 第一遍：检查所有目标页都已被分配。 */
    for (unsigned long i = 0; i < n_pages; i++) {
        unsigned long vpn = start_vpn + i;
        if (vpn == 0 || vpn >= g_vm.num_v_pages || !get_bit(g_vm.virt_bitmap, vpn)) {
            pthread_mutex_unlock(&g_vm.vm_lock);
            printf("Segmentation Fault\n");
            return;
        }
    }

    /* 第二遍：执行释放——清 PTE/物理位图、磁盘备份、虚拟位图，失效 TLB。 */
    for (unsigned long i = 0; i < n_pages; i++) {
        unsigned long vpn = start_vpn + i;
        unsigned long pa = clear_pte_locked(g_vm.page_dir, vpn * PAGE_SIZE);
        if (pa != 0) {
            free_phys_page_locked(pa / PAGE_SIZE);
        }
        if (g_vm.vpn_to_disk[vpn] >= 0) {
            free_disk_page_locked((unsigned long)g_vm.vpn_to_disk[vpn]);
            g_vm.vpn_to_disk[vpn] = -1;
        }
        clear_bit(g_vm.virt_bitmap, vpn);
        invalidateTLBEntry(vpn);
    }

    pthread_mutex_unlock(&g_vm.vm_lock);
}


/*
 * 将 val 指向的 size 字节数据写入由 va 起始的虚拟内存。
 * 若 va 起始 size 字节范围内任何虚拟页未分配，则输出错误并直接返回，不写入。
 * 跨页访问时按页拷贝。
 */
void myWrite(void *va, void *val, int size) {
    if (size <= 0 || !val) return;
    if (!g_vm.initialized) {
        printf("ERROR: Writing to unallocated address\n");
        return;
    }

    unsigned long vaddr = (unsigned long)va;
    unsigned long remaining = (unsigned long)size;
    const char *src = (const char *)val;

    pthread_mutex_lock(&g_vm.vm_lock);
    if (!validate_range_locked(vaddr, remaining)) {
        pthread_mutex_unlock(&g_vm.vm_lock);
        printf("ERROR: Writing to unallocated address\n");
        return;
    }

    while (remaining > 0) {
        pte_t *dst = translate(g_vm.page_dir, (void *)vaddr);
        if (!dst) {
            pthread_mutex_unlock(&g_vm.vm_lock);
            printf("ERROR: Writing to unallocated address\n");
            return;
        }
        unsigned long off = get_offset(vaddr);
        unsigned long chunk = PAGE_SIZE - off;
        if (chunk > remaining) chunk = remaining;
        memcpy((void *)dst, src, chunk);
        src += chunk;
        vaddr += chunk;
        remaining -= chunk;
    }
    pthread_mutex_unlock(&g_vm.vm_lock);
}


/*
 * 从虚拟地址 va 起读取 size 字节到 val 指向的缓冲区。
 * 若任何虚拟页未分配则输出错误并直接返回。
 * （TLB 查询逻辑将在 step5 中接入到 translate 路径上）
 */
void myRead(void *va, void *val, int size) {
    if (size <= 0 || !val) return;
    if (!g_vm.initialized) {
        printf("ERROR: Reading from unallocated address\n");
        return;
    }

    unsigned long vaddr = (unsigned long)va;
    unsigned long remaining = (unsigned long)size;
    char *dst = (char *)val;

    pthread_mutex_lock(&g_vm.vm_lock);
    if (!validate_range_locked(vaddr, remaining)) {
        pthread_mutex_unlock(&g_vm.vm_lock);
        printf("ERROR: Reading from unallocated address\n");
        return;
    }

    while (remaining > 0) {
        pte_t *src = translate(g_vm.page_dir, (void *)vaddr);
        if (!src) {
            pthread_mutex_unlock(&g_vm.vm_lock);
            printf("ERROR: Reading from unallocated address\n");
            return;
        }
        unsigned long off = get_offset(vaddr);
        unsigned long chunk = PAGE_SIZE - off;
        if (chunk > remaining) chunk = remaining;
        memcpy(dst, (void *)src, chunk);
        dst += chunk;
        vaddr += chunk;
        remaining -= chunk;
    }
    pthread_mutex_unlock(&g_vm.vm_lock);
}
