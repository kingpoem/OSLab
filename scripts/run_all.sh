#!/bin/bash
#
# run_all.sh
#
# 一键运行多组配置 (PAGE_SIZE × TLB_SIZE × PM_SIZE) 下的：
#   - 自定义实现 (libmy_vm.a)
#   - 标准库基线实现 (libbaseline_vm.a)
# 对官方 benchmark/test.c, benchmark/multi_test.c, tools/tlb_bench.c 三个程序
# 分别执行，将 stdout+stderr 输出到 logs/{custom,baseline}_<bench>_<cfg>.log。
#
# 同时把所有自定义实现的 TLB 统计汇总到 logs/summary.csv。
#
# 用法：
#   bash scripts/run_all.sh                # 运行内置默认配置矩阵
#   PAGE=16384 TLB=8 PM=64 bash run_all.sh # 单一配置
#

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOG_DIR="$ROOT/logs"
DEF_BAK="$ROOT/defines.h.bak"

mkdir -p "$LOG_DIR"
SUMMARY="$LOG_DIR/summary.csv"
echo "page_size,tlb_size,pm_size_mb,bench,impl,tlb_accesses,tlb_misses,tlb_miss_rate,exit" > "$SUMMARY"

# --- 默认配置矩阵 ---------------------------------------------------------
# 元组格式: PAGE_SIZE TLB_SIZE PM_MB
# 说明：本实现按"一个二级页表 = 一个物理页"的约束设计，因此要求
#       num_pte_entries * sizeof(pte_t) <= PAGE_SIZE。
#       在 32 位双级页表下，该约束等价于 PAGE_SIZE >= 4096。
#       任务书提示评分仅测 log2(PAGE_SIZE) 为偶数的情况，下方仅列出受支持的合法配置。
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

# 预编译一份 baseline 库 (与配置无关)
echo "[baseline] building libbaseline_vm.a"
( cd "$ROOT/baseline" && gcc -g -c -m32 baseline_vm.c -o baseline_vm.o \
  && ar -rc libbaseline_vm.a baseline_vm.o && ranlib libbaseline_vm.a ) >/dev/null

# 解析 printTLBStats 的输出（stderr 与 stdout 合流时可能错位，改用宽松匹配）
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

    # 写入新的 defines.h
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

    # ---- custom 实现 ------------------------------------------------
    ( cd "$ROOT" && make clean >/dev/null && make >/dev/null )
    if [[ ! -f "$ROOT/libmy_vm.a" ]]; then
        echo "[FAIL] custom build failed"
        return
    fi

    local CUSTOM_LIBS="-L${ROOT} -lmy_vm -m32 -lm"
    local BASE_LIBS="-L${ROOT}/baseline -lbaseline_vm -m32 -lm"

    # benchmark/test.c
    for IMPL in custom baseline; do
        if [[ "$IMPL" == "custom" ]]; then LIBS="$CUSTOM_LIBS"; else LIBS="$BASE_LIBS"; fi
        local LOG="$LOG_DIR/${IMPL}_test_${TAG}.log"
        gcc -m32 "$ROOT/benchmark/test.c" $LIBS -o /tmp/_test_runner 2>>"$LOG"
        /tmp/_test_runner > "$LOG" 2>&1
        local rc=$?
        echo "  test     [${IMPL}] -> $LOG (rc=$rc)"
        echo "${PAGE_SIZE},${TLB_SIZE},${PM_MB},test,${IMPL},NA,NA,NA,$rc" >> "$SUMMARY"
    done

    # benchmark/multi_test.c
    for IMPL in custom baseline; do
        if [[ "$IMPL" == "custom" ]]; then LIBS="$CUSTOM_LIBS"; else LIBS="$BASE_LIBS"; fi
        local LOG="$LOG_DIR/${IMPL}_multi_${TAG}.log"
        gcc -m32 "$ROOT/benchmark/multi_test.c" $LIBS -lpthread -o /tmp/_multi_runner 2>>"$LOG"
        timeout 60 /tmp/_multi_runner > "$LOG" 2>&1
        local rc=$?
        echo "  multi    [${IMPL}] -> $LOG (rc=$rc)"
        echo "${PAGE_SIZE},${TLB_SIZE},${PM_MB},multi,${IMPL},NA,NA,NA,$rc" >> "$SUMMARY"
    done

    # tools/tlb_bench.c — 仅 custom 有意义，但 baseline 也跑用于性能对照
    for IMPL in custom baseline; do
        if [[ "$IMPL" == "custom" ]]; then LIBS="$CUSTOM_LIBS"; else LIBS="$BASE_LIBS"; fi
        local LOG="$LOG_DIR/${IMPL}_tlb_${TAG}.log"
        gcc -m32 "$ROOT/tools/tlb_bench.c" $LIBS -lpthread -o /tmp/_tlb_runner 2>>"$LOG"
        timeout 60 /tmp/_tlb_runner > "$LOG" 2>&1
        local rc=$?
        local stats; stats="$(parse_tlb "$LOG")"
        echo "  tlb_bench[${IMPL}] -> $LOG (rc=$rc, ${stats})"
        echo "${PAGE_SIZE},${TLB_SIZE},${PM_MB},tlb_bench,${IMPL},${stats},$rc" >> "$SUMMARY"
    done
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
