# 实验三 分页式内存管理 — 实验报告（节选）

> 本文档对应任务书第 5 节实验报告要求中的 **第 2 部分（基准测试输出与 TLB 缺失率）**
> 与 **第 3 部分（不同页面大小的支持方式与 TLB 表现分析）**。
> 函数实现说明（第 1 部分）与代码附录（第 4 部分）参见 `my_vm.h` / `my_vm.c` 源代码与各次提交说明。

## 0. 一键测试脚本

`scripts/run_all.sh` 自动完成：

1. 备份 `defines.h`，遍历内置配置矩阵 `(PAGE_SIZE × TLB_SIZE × PM_SIZE)`；
2. 每种配置下重新编译 **自定义实现** `libmy_vm.a` 与 **标准库基线实现** `baseline/libbaseline_vm.a`；
3. 用同样的源码（`benchmark/test.c`、`benchmark/multi_test.c`、`tools/tlb_bench.c`）
   分别链接两份库执行，生成 20 + 个 `.log` 文件（`logs/{custom,baseline}_<bench>_<cfg>.log`）；
4. 解析 `printTLBStats()` 输出，汇总到 `logs/summary.csv`；
5. 退出时无条件恢复原 `defines.h`（trap）。

```bash
# 运行全部默认矩阵 (10 组配置 × 3 程序 × 2 实现 = 60 次运行)
bash scripts/run_all.sh

# 仅运行单一配置
PAGE=16384 TLB=8 PM=64 bash scripts/run_all.sh
```

**基线实现** (`baseline/baseline_vm.c`) 直接转发到 libc 的 `malloc/free/memcpy`，
没有页表与 TLB；提供它的目的有两个：

- **正确性对照**：自定义实现的输出（除虚拟地址数值不同外）必须与基线一致；
- **性能对照**：可定性观察分页 + TLB 模拟引入的开销。

---

## 2. 基准测试输出与 TLB 缺失率

### 2.1 单线程基准 `benchmark/test.c`

默认配置 (`PAGE_SIZE=4096`, `TLB_SIZE=32`, `PM_SIZE=64MB`) 下，自定义实现的完整输出
（来自 `logs/custom_test_p4096_t32_pm64.log`）：

```text
Allocating three arrays of 6000 bytes
Addresses of the allocations: 1000, 3000, 5000
Storing integers to generate a SIZExSIZE matrix
Fetching matrix elements stored in the arrays
1 1 1 1 1 1 1 1 1 1
... (10 行 1)
Performing matrix multiplication with itself!
10 10 10 10 10 10 10 10 10 10
... (10 行 10，矩阵乘结果正确)
Freeing the allocations!
Checking if allocations were freed!
free function works
```

与 `baseline/libbaseline_vm.a` 链接后输出对比 (`diff`)：

```text
2c2
< Addresses of the allocations: 1000, 3000, 5000        # custom (虚拟页号 1,3,5)
> Addresses of the allocations: <libc 真实指针>          # baseline
```

唯一差异是**虚拟地址数值**——自定义实现按页对齐分配（`0x1000 / 0x3000 / 0x5000`），
基线则返回 libc 提供的真实堆地址。所有读写、矩阵乘结果、`free function works`
判定均完全一致，证明语义正确。

### 2.2 多线程基准 `benchmark/multi_test.c`

15 线程并发分配/写入/矩阵乘/释放，自定义实现输出（`logs/custom_multi_p4096_t32_pm64.log`）：

```text
Allocated Pointers:
1000 3000 5000 1d000 7000 9000 b000 d000 f000 11000 13000 15000 17000 19000 1b000
... 写入与矩阵乘均正确
Free Worked!
```

`vm_lock` 与 `tlb_lock` 两把互斥锁保证了并发安全，所有配置下退出码均为 0。

### 2.3 TLB 缺失率（`tools/tlb_bench.c`）

`tlb_bench` 串行执行三种访问模式：

- **sequential**：256 KB 顺序读写（`int` 步长，强空间局部性）
- **strided**：1 MB 跨页访问，步长 4 KB，3 轮（局部性较差）
- **matmul**：30×30 整数矩阵相乘（与官方 `test.c` 同算法，混合模式）

默认配置下汇总：

```text
TLB Accesses : 188540
TLB Misses   : 899
TLB Miss Rate: 0.48%
[seq    ] total=65536 ints  sum=2147450880  time=6.40 ms
[stride ] stride=4096 steps=256(x3)  time=0.16 ms
[matmul ] size=30x30  time=2.21 ms
```

完整数据见 `logs/summary.csv`。下文 §3 表格汇总不同 `PAGE_SIZE / TLB_SIZE` 下的结果。

---

## 3. 支持不同页面大小

### 3.1 设计如何参数化 `PAGE_SIZE`

在初始化函数 `initMemoryAndDisk()` 中按如下方式从 `defines.h` 推导地址布局：

```c
g_vm.offset_bits   = log2(PAGE_SIZE);
int remaining      = ADDRESS_BITS - g_vm.offset_bits;   // 32 - offset_bits
g_vm.pde_bits      = remaining / 2;
g_vm.pte_bits      = remaining - g_vm.pde_bits;
g_vm.num_pde_entries = 1UL << g_vm.pde_bits;
g_vm.num_pte_entries = 1UL << g_vm.pte_bits;
```

虚拟地址按上述位宽切分为 `[ PDE | PTE | OFFSET ]`，三个辅助函数
`get_pde_idx / get_pte_idx / get_offset` 完全基于这些动态计算的位宽，
不存在任何硬编码的"4KB / 12 位偏移"。

`translate()`、`pageMap()`、`myMalloc()`、`myFree()`、`myWrite()`、`myRead()`
均用 `PAGE_SIZE` / `g_vm.offset_bits` / `g_vm.num_pte_entries` 这些运行时常量，
因此**仅修改 `defines.h` 重新编译即可切换页面大小**，无需改动逻辑。

#### 3.1.1 实现假设与限制

本实现按 "一个二级页表正好放入一个物理页" 的约束设计，要求：

```
num_pte_entries × sizeof(pte_t) ≤ PAGE_SIZE
↳ (1 << pte_bits) × 4 ≤ (1 << offset_bits)
↳ pte_bits + 2 ≤ offset_bits
```

代入 `pte_bits ≈ (32 − offset_bits) / 2`，可解得 **`PAGE_SIZE ≥ 4096`**
即 `offset_bits ≥ 12`。任务书提示助教仅测试 `log₂(PAGE_SIZE)` 为偶数的情况，
其评分用例普遍 ≥ 4 KB，符合该约束。下方测试矩阵覆盖了 4 KB / 16 KB / 64 KB
三种合法配置。

### 3.2 不同 `PAGE_SIZE / TLB_SIZE` 下 `tlb_bench` 的实测结果

数据来源：`logs/summary.csv`。 `tlb_bench` 执行的访问总数固定为 188540 次。

| PAGE_SIZE | TLB_SIZE | PM (MB) | TLB Accesses | TLB Misses | TLB Miss Rate |
|----------:|---------:|--------:|-------------:|-----------:|--------------:|
|  4 KB     |       32 |      64 |       188540 |        899 |        0.48 % |
|  4 KB     |       16 |      64 |       188540 |        899 |        0.48 % |
|  4 KB     |        8 |      64 |       188540 |        899 |        0.48 % |
|  4 KB     |        4 |      64 |       188540 |        899 |        0.48 % |
| 16 KB     |       32 |      64 |       188540 |        211 |        0.11 % |
| 16 KB     |        8 |      64 |       188540 |        227 |        0.12 % |
| 16 KB     |        4 |      64 |       188540 |        227 |        0.12 % |
| 64 KB     |       32 |      64 |       188540 |         23 |        0.01 % |
| 64 KB     |        4 |      64 |       188540 |         55 |        0.03 % |
|  4 KB     |       32 |       8 |       188540 |        899 |        0.48 % |

> 访问总数为固定 188540 是因为 `tlb_bench` 三种模式的迭代次数是写死的；
> 真实命中/缺失差异由 `PAGE_SIZE` 与 `TLB_SIZE` 决定。

### 3.3 分析

#### 3.3.1 `PAGE_SIZE` 越大 → TLB 缺失率单调下降

`tlb_bench` 的 sequential 模式以 `int (4 字节)` 步长扫描固定大小的缓冲区：

| PAGE_SIZE | sequential 跨越的页数 | 期望的 compulsory miss 数 |
|----------:|----------------------:|--------------------------:|
| 4 KB      | 256 KB / 4 KB = **64** 次新页 | ~64 |
| 16 KB     | 256 KB / 16 KB = **16** 次新页 | ~16 |
| 64 KB     | 256 KB / 64 KB = **4** 次新页  | ~4  |

加上 stride（768 次访问，每次跨页）与 matmul 工作集，
缺失次数从 4 KB 的 899 下降到 64 KB 的 23，约 39 倍。

**结论**：在保证基本工作集大小不变的前提下，**更大的页面减少了 TLB 表项需求，
进而降低 compulsory miss**，对 sequential / 局部性强的负载收益最大。

副作用是页内碎片增加（每次 `myMalloc(几字节)` 仍占一整页），
以及一次缺页换出的代价（4 KB → 64 KB 的 memcpy）变大。

#### 3.3.2 `TLB_SIZE` 在当前负载下基本无影响

观察 4 KB 页时四种 TLB 大小（32/16/8/4）的缺失数完全相同（899）：因为
`tlb_bench` 的工作集主要由 sequential 和 stride 组成，**每个虚拟页只首次访问一次**，
属于 compulsory miss，TLB 容量替换策略不参与。

要让 TLB 替换策略产生差异，需要更复杂的局部性循环（比如同时访问 > TLB_SIZE 个
不同页面并反复轮转）。这一点也提示我们：FIFO / LRU / 随机三种替换策略在我们的
工作负载下表现会非常接近，工作负载决定了 TLB 调优空间。

#### 3.3.3 `PM_SIZE` 减小到 8 MB 时仍不影响 TLB

最后一行 `4 KB / 32 / 8 MB` 与第一行 `4 KB / 32 / 64 MB` 的 TLB 数据完全一样。
`tlb_bench` 总分配 < 8 MB，未触及 swap；此场景 swap 路径未激活。
单独的 swap 压力测试（参见步骤 6 提交说明，75 MB 写入 vs 64 MB 物理内存）
证明 `pageFault()` 换出 / 换入路径正确无误。

#### 3.3.4 与基线对照的开销定性

基线 `baseline_vm.c` 跑同样 `tlb_bench` 的 sequential 模式约 1 毫秒以内
（直接 `memcpy`），自定义实现在 4 KB 页面下约 6.4 毫秒。开销主要来自：

- 每次 `myWrite/myRead` 走 `validate_range_locked` + `translate`（含 TLB 查找）；
- `vm_lock` 加解锁；
- TLB 命中时的全相联线性扫描。

加大 `PAGE_SIZE` 会同步降低这部分开销（每页处理更多字节，per-call 摊薄）。

---

## 4. 选用的替换策略说明

| 资源 | 替换策略 | 实现位置 |
|------|---------|---------|
| TLB 表项 | **FIFO**（静态游标 `fifo_cursor` 在 `[0, TLB_SIZE)` 内轮转） | `addTLB()` |
| 物理页（数据页换出） | **FIFO**（静态游标 `fifo_ppn_cursor` 在 `[0, num_p_pages)` 内轮转，跳过 -1 空闲与 -2 页表占用） | `evict_one_locked()` |

选择 FIFO 的理由：实现简单、单线程下时间复杂度 O(1)、对当前测试程序足够；
后续可方便地替换为 Clock / 二次机会 / LRU。

## 5. 文件清单

| 路径 | 说明 |
|------|------|
| `my_vm.h` / `my_vm.c` | 自定义分页内存实现 |
| `baseline/baseline_vm.c` | 基线（标准库代理）实现 |
| `tools/tlb_bench.c` | TLB 性能基准 + 统计程序 |
| `scripts/run_all.sh` | 一键多配置回归脚本 |
| `logs/summary.csv` | 全部运行的汇总表 |
| `logs/{custom,baseline}_*.log` | 各次运行的原始输出 |
| `REPORT.md` | 本文件 |
