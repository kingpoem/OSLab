# 实验过程中遇到的问题与思考

> 对应任务书第 5 节实验报告内容要求第 1 条：
> *"阐述各虚拟内存函数的详细实现逻辑，你在实验过程中遇到的问题和思考。"*
>
> 实现按照任务书"建议步骤"分 6 次提交（`step1` ~ `step6`），每完成一步先做最小验证再提交，
> 这样回滚成本最低、定位 bug 最快。下文按问题领域归纳整个开发过程中真实遇到、并已修复的问题。

---

## 1. 数据结构设计阶段的取舍

### 1.1 PDE / PTE 用什么含义？是 host 指针还是物理偏移？

**问题**：头文件给的 `pde_t` / `pte_t` 都是 `unsigned long`。在 32 位 (`-m32`) 编译下
它正好 4 字节，既能装 host 指针也能装"物理内存内偏移"。

**思考**：

- 用 host 指针：访问最快，但在多次分配/释放时与 `phys_mem` 基址绑死，
  调试时数字很大、不直观。
- 用偏移：所有 PDE/PTE 数值都在 `[0, PM_SIZE)` 区间内，便于 `printf` 调试，
  且与教材一致。

最终选择**物理偏移**，并约定 `0` 代表 "未映射"。这就引出下面两个保留约定：

### 1.2 物理页 0 与虚拟页 0 必须保留

**问题**：如果 PTE = 0 表示未映射，那么物理页 0 就不能再被某个 PTE 合法指向；
否则无法区分"映射到物理页 0"与"未映射"。同理虚拟页 0 一旦被分配出去，
`myMalloc` 返回 `NULL` 会与"分配失败"语义冲突。

**解决**：

- `initMemoryAndDisk` 中将物理页 0 留给一级页目录（正好 4 KB），并 `set_bit(phys_bitmap, 0)`；
- 同时 `set_bit(virt_bitmap, 0)` 保留虚拟页 0，`find_free_vpages_locked` 从 vpn 1 开始扫描。

这样所有合法返回的 `va` 都 ≥ `PAGE_SIZE`，`NULL` 仍专门表示分配失败。

---

## 2. 两级页表对 `PAGE_SIZE` 的隐式要求

**问题**：当用脚本测试 `PAGE_SIZE = 1024` 时，`tlb_bench` 的 `bench_strided` 段出现
大量 `ERROR: Reading from unallocated address`，单步排查发现是**写坏了相邻物理页**。

**根因**：`PAGE_SIZE = 1024` → `offset_bits = 10` → 剩余 22 位均分得 `pte_bits = 11`。
一个二级页表 `num_pte_entries × sizeof(pte_t) = 2048 × 4 = 8192 字节`，
但它被放在一个**只有 1024 字节**的物理页里，写到第 256 个 PTE 之后就溢出到下一页。

**思考**：要让"一个二级页表正好放进一个物理页"，等价约束是

```
(1 << pte_bits) × 4 ≤ (1 << offset_bits)
↳ pte_bits + 2 ≤ offset_bits
↳ (32 − offset_bits)/2 + 2 ≤ offset_bits
↳ offset_bits ≥ 12  ⇒  PAGE_SIZE ≥ 4096
```

任务书提示助教仅测 `log₂(PAGE_SIZE)` 偶数情况，常见配置都 ≥ 4 KB，符合该约束。
我在 `scripts/run_all.sh` 默认矩阵和 `REPORT.md §3.1.1` 都明确写出该限制。
若要支持更小的页面，需要让二级页表跨多个物理页或改成多级页表，超出本实验范围。

---

## 3. 物理内存吃紧时 `myMalloc` 的连锁失败

**问题**：第一次写完 `pageFault` 跑 75 MB > 64 MB 的 swap 压力测试时，
在第 13 个 5 MB 分配处返回 `NULL`，但磁盘还有大量空间。

**调试**：把 evict 路径加 `fprintf(stderr)` 跟踪，发现确实 evict 成功了，
但下一行就走到 `pageMap` 内部，那里又调用 `alloc_phys_page_locked`（**没有** evict 版本）
为新建二级页表分配物理页——而此时物理位图刚因为本次循环把 1280 个数据页全部用掉，
PT 也想分配新页就失败了。

**修复**：把 `pageMap` 内分配二级页表的那行也改成 `alloc_phys_page_with_evict_locked`。
但这个函数定义在 `pageMap` 的下方，需要前向声明，于是补上：

```c
static long evict_one_locked(void);
static long alloc_phys_page_with_evict_locked(void);
```

**思考**：这是"任何会消耗资源的辅助函数都应该走资源回收路径"的典型例子。
后来再扫一遍代码，确认 `pageFault` 内部重建 PTE 时也已经使用了 `with_evict` 版本。

---

## 4. 区分 "从未分配" 与 "已分配但被换出"

**问题**：仅凭 `PTE == 0` 不能区分这两种情形。`myRead` / `myWrite` 在 `validate_range_locked`
里看到 `virt_bitmap == 1` 后就放心走 `translate`，但被换出的页此刻 PTE 是 0，
若直接报错就把 swap 路径切断了。

**解决**：

- 维护 `vpn_to_disk[vpn]`：默认 `-1`，被换出时存放磁盘页号；
- `translate` 内部在 `pte == 0` 时再查 `vpn_to_disk`，若 `≥ 0` 则调用 `pageFault` 换入；
- `myFree` 释放时也要 `free_disk_page_locked` 释放磁盘备份并复位 `vpn_to_disk`。

**思考**：换入路径放在 `translate` 而不是 `myRead/myWrite` 里有两个好处：

1. 任何调用 `translate` 的代码都自动获得 swap-in；
2. TLB 与 swap 解耦——TLB 命中直接返回，根本不会走到这条路径。

---

## 5. 反向映射：选 victim 时怎么知道某个 ppn 属于谁？

**问题**：FIFO 选 victim 物理页时，需要立即知道**当前持有该 ppn 的虚拟页号**，
否则没法清掉 owner 的 PTE。如果每次都遍历所有 PDE/PTE 找匹配，开销 `O(num_pde × num_pte)`，
完全不可接受。

**解决**：增加一张 `ppn_to_vpn[]` 反向表，约定：

| 值 | 含义 |
|---|---|
| `-1` | 该 ppn 空闲 |
| `-2` | 该 ppn 被一级页目录或某个二级页表占用，**不可换出** |
| `≥ 0` | 该 ppn 是一个数据页，对应的 vpn |

在 `pageMap` 设置 PTE 时 `ppn_to_vpn[ppn] = vpn`，在 `pageMap` 分配 PT 物理页时
`ppn_to_vpn[pt_pn] = -2`。`evict_one_locked` 只跳过 `< 0` 的项即可。

**思考**：用 `-2` 标记 PT 是为了**绝对禁止换出页表**，否则会出现"二级页表自己被换出，
导致整个 PDE 下的所有数据页全部失访"的灾难。代价是少量物理页（页表）永远占内存，
但相比一次错误的 PT 换出收益太大。

---

## 6. 多线程下的锁与死锁

**问题**：`multi_test.c` 用 15 线程跑 `myMalloc/myWrite/myFree`，并要求矩阵乘结果正确。
最初版本只有 `vm_lock` 一把锁，TLB 操作裸读裸写——结果偶发出现 stale TLB hit
读到错误数据。

**解决**：增加 `tlb_lock` 单独保护 TLB；约定**锁顺序固定为 `vm_lock` → `tlb_lock`**。

```text
myMalloc / myFree / myRead / myWrite
   └─ 持 vm_lock
        ├─ translate
        │     └─ checkTLB / addTLB    (持 tlb_lock，短临界区)
        └─ invalidateTLBEntry         (持 tlb_lock)
```

**思考**：`checkTLB` 与 `addTLB` 也对外开放（任务书要求），独立调用时它们只持 `tlb_lock`，
不会反向去拿 `vm_lock`。锁顺序单向，因此不会死锁。

---

## 7. `pte_t * translate(pde_t *pgdir, void *va)` 的语义到底是什么？

**问题**：头文件签名只说"返回 PTE 指针"，但实际上下层 `myRead/myWrite` 想得到的是
"可以直接 `memcpy` 的字节地址"。

**最终约定**：`translate` 返回 `(pte_t *)(phys_mem + ppn × PAGE_SIZE + offset)`，
即指向 phys_mem 内字节位置的 host 指针，类型 `pte_t*` 仅出于头文件兼容。
`myRead/myWrite` 把它当 `void*` 用，`memcpy(dst, p, chunk)` 即可。

**思考**：这是工程上常见的"接口与实际语义不完全对齐"，靠注释明确说明即可。
`translate` 内部还兼顾了 TLB 查询、TLB 回填、swap-in 三件事——所有路径都共享同一个
返回值约定，避免上层重复工作。

---

## 8. 跨页 `myWrite/myRead` 的 chunk 计算

**问题**：跨页拷贝时 chunk 大小写错过一次，导致最后一页越界写。

**正确写法**：

```c
unsigned long off   = get_offset(vaddr);
unsigned long chunk = PAGE_SIZE - off;        // 当前页剩余可用字节
if (chunk > remaining) chunk = remaining;     // 但不能超过总剩余字节
```

**思考**：除了第一页可能有 `off > 0`，后续每页都是 `off = 0`，因此
后续 chunk 退化为 `min(PAGE_SIZE, remaining)`，符合直觉。
单元测试里要专门覆盖"`size` 跨页"和"起始就在页中间"两种情况。

---

## 9. `myFree` 必须先验证再释放

**问题**：第一版 `myFree` 一边检查一边释放，遇到中间某页未分配时已经释放了前面几页，
违反任务书"仅当所有页成功释放时返回成功"。

**修复**：分两遍循环：

1. **第一遍**：扫描全部目标 vpn，全部 `virt_bitmap == 1` 才算合法，否则
   只打印 `Segmentation Fault`，**不动任何状态**直接返回；
2. **第二遍**：对每页执行 PTE 清理、物理页归还、磁盘备份归还、TLB 失效、虚拟位图清零。

**思考**：原子性是 OS 接口设计的基本要求；分两遍换来代码简洁，开销可忽略。

---

## 10. 测试基础设施踩过的坑

### 10.1 `stdout` 与 `stderr` 合流后 `grep '^TLB'` 失败

`printTLBStats` 走 `stderr`，业务输出走 `stdout`，shell `2>&1` 合并到同一文件时
两路缓冲粒度不同，输出可能被切到一行（`ERROR: Reading from unallocaTLB Accesses : 187772`）。
导致 `grep -E '^TLB Accesses'` 漏抓。

**解决**：`parse_tlb` 改用 `grep -oE 'TLB Accesses[[:space:]]*:[[:space:]]*[0-9]+'` 宽松匹配，
不依赖行首。

### 10.2 32 位编译环境

Ubuntu 22.04 默认只装 64 位 gcc。需要：

```bash
sudo dpkg --add-architecture i386
sudo apt install gcc-multilib libc6-dev-i386
```

否则 `-m32` 报 `bits/libc-header-start.h: No such file or directory`。

### 10.3 全局静态 `g_init_lock` 必须在 `.data` 中初始化

第一版 `pthread_mutex_init(&g_init_lock, NULL)` 写在 `initMemoryAndDisk` 内部，
但 `initMemoryAndDisk` 自己也用这把锁——形成"用未初始化的锁去保护初始化"的鸡生蛋问题。

**修复**：直接用 `PTHREAD_MUTEX_INITIALIZER` 静态初始化：

```c
static pthread_mutex_t g_init_lock = PTHREAD_MUTEX_INITIALIZER;
```

---

## 11. 如果重做会怎么改进

1. **空闲虚拟页用更高效结构**：当前 `find_free_vpages_locked` 线性扫位图，
   `VM_SIZE = 4 GB / PAGE_SIZE` 项，最坏 1 M 次循环；可改空闲段链表 / 红黑树。
2. **TLB 替换换 LRU 或 Clock**：FIFO 在循环访问 > TLB_SIZE 个页时会反复淘汰刚用过的项；
   实测当前 benchmark 是 compulsory miss 占主导，FIFO/LRU 几乎没区别，
   所以暂时没改。
3. **更精细的 `pageFault` 错误码**：当前返回 `-1` 不区分"磁盘满"还是"无可换页"，
   生产代码应有不同返回。
4. **per-PDE 锁**：目前一把全局 `vm_lock` 串行所有 VM 操作；多线程下吞吐受限。
   按一级页目录索引拆锁可显著提升并发度。

---

## 附：与本次报告关联的 commit 链

```
86aea58 step1: 设计基础数据结构并实现 initMemoryAndDisk
cdc040e step2: 实现 translate() 与 pageMap()
5723906 step3: 实现 myMalloc 与 myFree
d02fdb0 step4: 实现 myWrite 与 myRead
31be715 step5: 加入 TLB 全相联缓存
df09cc0 step6: pageFault 与磁盘换出/换入
4fa6641 benchmark: 一键多配置测试脚本 + 报告（曾含 baseline 对照）
89059a0 benchmark: 移除 baseline 对照，只保留自定义实现的基准测试
```

每一步的具体修改面参见对应 commit message。
