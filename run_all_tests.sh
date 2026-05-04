#!/usr/bin/env bash
# run_all_tests.sh
# ----------------------------------------------------------------
# 一次启动跑完全部基准测试：
#   1) my_pthread 库（PSJF 与 MLFQ 两种调度策略）
#      -> log_my_pthread.log
#   2) 系统标准 pthread 库
#      -> log_std_pthread.log
#
# 用法：
#   ./run_all_tests.sh                       # 默认线程数 1 2 4 8，每组 3 次
#   THREADS="1 4" RUNS=2 ./run_all_tests.sh  # 自定义
# ----------------------------------------------------------------

set -u  # 引用未定义变量时报错（不要 -e，避免单次 benchmark 超时就退出）
cd "$(dirname "$0")"
ROOT="$(pwd)"

# ============================ 可调参数 ============================
THREADS_LIST=( ${THREADS:-1 2 4 8} )
RUNS=${RUNS:-3}
TIMEOUT_SEC=${TIMEOUT_SEC:-180}
BENCHES=( parallelCal vectorMultiply externalCal )

MY_LOG="$ROOT/log_my_pthread.log"
STD_LOG="$ROOT/log_std_pthread.log"
HEADER_FILE="$ROOT/my_pthread_t.h"

# ============================ 工具函数 ============================
hr() { printf '%.0s=' {1..70}; printf '\n'; }
sub() { printf '%.0s-' {1..70}; printf '\n'; }

# tee_log <logfile> ：把 stdin 同时输出到 stdout 和 logfile
tee_log() { tee -a "$1"; }

# 打印一行到指定 log 与终端
say() {
	local logfile="$1"; shift
	echo "$@" | tee -a "$logfile"
}

# 重新生成 externalCal 需要的 record 目录
ensure_record() {
	if [ ! -d "$ROOT/Benchmark/record" ]; then
		( cd "$ROOT/Benchmark" && bash genRecord.sh ) >/dev/null 2>&1
	fi
}

# 编译 my_pthread.a：参数为 SCHED 名字（PSJF/MLFQ），空表示默认 PSJF
build_lib() {
	local sched="$1"
	( cd "$ROOT" && make clean >/dev/null 2>&1
	  if [ -n "$sched" ]; then
		  make SCHED="$sched" >/dev/null 2>&1
	  else
		  make >/dev/null 2>&1
	  fi
	)
	return $?
}

# 编译 benchmark 可执行文件
build_benchmarks() {
	( cd "$ROOT/Benchmark" && make clean >/dev/null 2>&1 && make >/dev/null 2>&1 )
	return $?
}

# 运行单个 benchmark 一次：参数 logfile bench_name thread_num run_idx
run_one() {
	local logfile="$1" bench="$2" nth="$3" run="$4"
	echo ""                                       | tee_log "$logfile" >/dev/null
	echo ">>> $bench  threads=$nth  run #$run"    | tee_log "$logfile"
	if [ "$bench" = "externalCal" ]; then ensure_record; fi
	# 进入 Benchmark 目录运行（externalCal 用相对路径 ./record）
	( cd "$ROOT/Benchmark" \
	  && timeout "$TIMEOUT_SEC" "./$bench" "$nth" 2>&1 ) | tee_log "$logfile"
	local rc=${PIPESTATUS[0]}
	if [ "$rc" -ne 0 ]; then
		echo "!!! $bench (threads=$nth) exited with code $rc" | tee_log "$logfile"
	fi
}

# 跑一整轮：所有 benchmark × 所有线程数 × RUNS 次
run_round() {
	local label="$1" logfile="$2"
	{
		hr
		echo "[$(date '+%F %T')] $label"
		echo "threads=${THREADS_LIST[*]}  runs=$RUNS  timeout=${TIMEOUT_SEC}s"
		hr
	} | tee_log "$logfile"
	for nth in "${THREADS_LIST[@]}"; do
		for bench in "${BENCHES[@]}"; do
			for ((i=1; i<=RUNS; i++)); do
				run_one "$logfile" "$bench" "$nth" "$i"
			done
		done
		echo "" | tee_log "$logfile" >/dev/null
		sub | tee_log "$logfile" >/dev/null
	done
}

# 把 USE_MY_PTHREAD 宏置于启用/禁用状态
enable_my_pthread() {
	# 已启用直接返回；否则取消注释
	if grep -qE '^#define USE_MY_PTHREAD 1' "$HEADER_FILE"; then return; fi
	sed -i 's|^//\s*#define USE_MY_PTHREAD 1|#define USE_MY_PTHREAD 1|' "$HEADER_FILE"
}

disable_my_pthread() {
	# 已禁用直接返回；否则改为注释
	if grep -qE '^//\s*#define USE_MY_PTHREAD 1' "$HEADER_FILE"; then return; fi
	sed -i 's|^#define USE_MY_PTHREAD 1|// #define USE_MY_PTHREAD 1|' "$HEADER_FILE"
}

# ============================ 退出清理 ============================
# 如果脚本中途异常退出，把头文件恢复为启用 my_pthread 的状态（默认状态）
cleanup() {
	enable_my_pthread
}
trap cleanup EXIT

# ============================ 1. my_pthread 测试 ============================
: > "$MY_LOG"   # 清空日志

enable_my_pthread

{
	hr
	echo "MY_PTHREAD BENCHMARK SUITE"
	echo "host:    $(uname -a)"
	echo "started: $(date '+%F %T')"
	hr
} | tee_log "$MY_LOG"

# ---- PSJF（默认） ----
echo "== building libmy_pthread.a (SCHED=PSJF) ==" | tee_log "$MY_LOG"
build_lib ""        || { echo "build PSJF failed" | tee_log "$MY_LOG"; exit 1; }
build_benchmarks    || { echo "build benchmarks failed" | tee_log "$MY_LOG"; exit 1; }
run_round "MY_PTHREAD :: SCHED = PSJF" "$MY_LOG"

# ---- MLFQ ----
echo "== building libmy_pthread.a (SCHED=MLFQ) ==" | tee_log "$MY_LOG"
build_lib "MLFQ"    || { echo "build MLFQ failed" | tee_log "$MY_LOG"; exit 1; }
build_benchmarks    || { echo "build benchmarks failed" | tee_log "$MY_LOG"; exit 1; }
run_round "MY_PTHREAD :: SCHED = MLFQ" "$MY_LOG"

echo ""                                              | tee_log "$MY_LOG"
echo "[$(date '+%F %T')] MY_PTHREAD suite finished" | tee_log "$MY_LOG"

# ============================ 2. 标准 pthread 测试 ============================
: > "$STD_LOG"

disable_my_pthread

{
	hr
	echo "STANDARD PTHREAD BENCHMARK SUITE"
	echo "host:    $(uname -a)"
	echo "started: $(date '+%F %T')"
	hr
} | tee_log "$STD_LOG"

# 标准 pthread 不依赖我们的策略宏；编译一次即可
echo "== building (libmy_pthread.a is linked but unused) ==" | tee_log "$STD_LOG"
build_lib ""        || { echo "build failed" | tee_log "$STD_LOG"; exit 1; }
build_benchmarks    || { echo "build benchmarks failed" | tee_log "$STD_LOG"; exit 1; }
run_round "STANDARD PTHREAD" "$STD_LOG"

echo ""                                                | tee_log "$STD_LOG"
echo "[$(date '+%F %T')] STD_PTHREAD suite finished"  | tee_log "$STD_LOG"

# ============================ 3. 汇总输出 ============================
{
	echo ""
	hr
	echo "ALL DONE"
	echo "  my_pthread log : $MY_LOG"
	echo "  std pthread log: $STD_LOG"
	hr
} | tee /dev/stderr >/dev/null   # 也输出到 stderr 方便看
echo ""
echo "Done."
echo "  my_pthread log : $MY_LOG"
echo "  std pthread log: $STD_LOG"
