#!/bin/bash
#
# run_all.sh
#
# 一键运行多组配置 (PAGE_SIZE × TLB_SIZE × PM_SIZE) 下的基准测试。
# 对每种配置执行：
#   - benchmark/test.c       (官方单线程矩阵基准)
#   - benchmark/multi_test.c (官方多线程基准)
#   - tools/tlb_bench.c      (自定义 TLB 性能基准 + printTLBStats)
#
# 输出：
#   - logs/<bench>_<cfg>.log         每次运行的 stdout+stderr
#   - logs/summary.csv               TLB 统计汇总（仅 tlb_bench 行有数据）
#
# 用法：
#   bash scripts/run_all.sh                 # 运行默认配置矩阵
#   PAGE=16384 TLB=8 PM=64 bash run_all.sh  # 运行单一配置
#

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOG_DIR="$ROOT/logs"
DEF_BAK="$ROOT/defines.h.bak"

mkdir -p "$LOG_DIR"
SUMMARY="$LOG_DIR/summary.csv"
echo "page_size,tlb_size,pm_size_mb,bench,tlb_accesses,tlb_misses,tlb_miss_rate,exit" > "$SUMMARY"

# --- 默认配置矩阵 ---------------------------------------------------------
# 元组格式: PAGE_SIZE TLB_SIZE PM_MB
# 说明：本实现按"一个二级页表 = 一个物理页"的约束设计，要求
#       num_pte_entries * sizeof(pte_t) <= PAGE_SIZE。
#       在 32 位双级页表下，等价于 PAGE_SIZE >= 4096。
#       任务书提示助教仅测 log2(PAGE_SIZE) 为偶数，下方仅列合法配置。
DEFAULT_CONFIGS=(
    "4096 32 64"
    "4096 16 64"
    "4096 8 64"
    "4096 4 64"
    "16384 32 64"
    "16384 8 64"
    "16384 4 64"
    "65536 32 64"
    "65536 4 64"
    "4096 32 8"     # 物理内存吃紧，触发更多 swap
)

if [[ -n "${PAGE:-}" && -n "${TLB:-}" && -n "${PM:-}" ]]; then
    CONFIGS=("$PAGE $TLB $PM")
else
    CONFIGS=("${DEFAULT_CONFIGS[@]}")
fi

# 备份原 defines.h，结束时无论成功失败都恢复
cp "$ROOT/defines.h" "$DEF_BAK"
trap 'cp "$DEF_BAK" "$ROOT/defines.h"; rm -f "$DEF_BAK"; echo "[restored defines.h]"' EXIT

# 解析 printTLBStats 输出（stdout/stderr 合流可能错位，宽松匹配）
parse_tlb() {
    local logfile="$1"
    local acc miss rate
    acc=$(grep -oE 'TLB Accesses[[:space:]]*:[[:space:]]*[0-9]+' "$logfile" | tail -1 | awk -F: '{print $2}' | tr -d ' ')
    miss=$(grep -oE 'TLB Misses[[:space:]]*:[[:space:]]*[0-9]+' "$logfile" | tail -1 | awk -F: '{print $2}' | tr -d ' ')
    rate=$(grep -oE 'TLB Miss Rate[[:space:]]*:[[:space:]]*[0-9.]+%?' "$logfile" | tail -1 | awk -F: '{print $2}' | tr -d ' %')
    echo "${acc:-NA},${miss:-NA},${rate:-NA}"
}

run_one_config() {
    local PAGE_SIZE="$1"; local TLB_SIZE="$2"; local PM_MB="$3"
    local TAG="p${PAGE_SIZE}_t${TLB_SIZE}_pm${PM_MB}"

    cat > "$ROOT/defines.h" <<EOF
#ifndef __DEFINES_H__
#define __DEFINES_H__
#define ADDRESS_BITS 32
#define VM_SIZE 4ULL*1024*1024*1024
#define DISK_SIZE 1ULL*1024*1024*1024
#define PM_SIZE ${PM_MB}*1024*1024
#define PAGE_SIZE ${PAGE_SIZE}
#define TLB_SIZE ${TLB_SIZE}
#endif
EOF

    echo ""
    echo "============================================================"
    echo "Config: PAGE=${PAGE_SIZE}  TLB=${TLB_SIZE}  PM=${PM_MB}MB"
    echo "============================================================"

    # 重新编译自定义实现
    ( cd "$ROOT" && make clean >/dev/null && make >/dev/null )
    if [[ ! -f "$ROOT/libmy_vm.a" ]]; then
        echo "[FAIL] build failed"
        return
    fi

    local LIBS="-L${ROOT} -lmy_vm -m32 -lm"

    # benchmark/test.c
    local LOG="$LOG_DIR/test_${TAG}.log"
    gcc -m32 "$ROOT/benchmark/test.c" $LIBS -o /tmp/_test_runner 2>>"$LOG"
    /tmp/_test_runner > "$LOG" 2>&1
    local rc=$?
    echo "  test      -> $LOG (rc=$rc)"
    echo "${PAGE_SIZE},${TLB_SIZE},${PM_MB},test,NA,NA,NA,$rc" >> "$SUMMARY"

    # benchmark/multi_test.c
    LOG="$LOG_DIR/multi_${TAG}.log"
    gcc -m32 "$ROOT/benchmark/multi_test.c" $LIBS -lpthread -o /tmp/_multi_runner 2>>"$LOG"
    timeout 60 /tmp/_multi_runner > "$LOG" 2>&1
    rc=$?
    echo "  multi     -> $LOG (rc=$rc)"
    echo "${PAGE_SIZE},${TLB_SIZE},${PM_MB},multi,NA,NA,NA,$rc" >> "$SUMMARY"

    # tools/tlb_bench.c
    LOG="$LOG_DIR/tlb_${TAG}.log"
    gcc -m32 "$ROOT/tools/tlb_bench.c" $LIBS -lpthread -o /tmp/_tlb_runner 2>>"$LOG"
    timeout 60 /tmp/_tlb_runner > "$LOG" 2>&1
    rc=$?
    local stats; stats="$(parse_tlb "$LOG")"
    echo "  tlb_bench -> $LOG (rc=$rc, ${stats})"
    echo "${PAGE_SIZE},${TLB_SIZE},${PM_MB},tlb_bench,${stats},$rc" >> "$SUMMARY"
}

for cfg in "${CONFIGS[@]}"; do
    read -r P T M <<<"$cfg"
    run_one_config "$P" "$T" "$M"
done

echo ""
echo "============================================================"
echo "All runs done. Summary written to: $SUMMARY"
echo "============================================================"
column -t -s, "$SUMMARY"
