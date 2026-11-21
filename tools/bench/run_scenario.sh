#!/bin/bash
# 运行一个压测场景：用 loadcheck 压指定目标，同时采集被测进程的峰值 RSS 和 CPU 时间。
#
# 用法：run_scenario.sh <名称> <被测进程 pid，直连时填 0> -- <loadcheck 参数...>
# 环境变量：
#   LOADCHECK  loadcheck 路径（默认 loadcheck）
#   LOAD_CPU   负载端（loadcheck）固定的 CPU（默认 1）；被测进程和 mock 的分核由调用者负责
#
# 输出一行 TSV：名称、成功/总数、耗时、块间隔偏差 p50/p99、TTFB p50/p99、峰值 RSS(KB)、空载 RSS(KB)、CPU 用户态/内核态(ms)
set -euo pipefail
name=$1; pid=$2; shift 2; [ "$1" = "--" ] && shift
LOADCHECK=${LOADCHECK:-loadcheck}; LOAD_CPU=${LOAD_CPU:-1}

rss() { [ "$pid" = 0 ] && echo 0 || awk '/VmRSS/{print $2}' /proc/$pid/status; }
cpu_ticks() { [ "$pid" = 0 ] && echo '0 0' || awk '{print $14, $15}' /proc/$pid/stat; }

idle_rss=$(rss); c0=$(cpu_ticks)
peak_file=$(mktemp)
( peak=0; while :; do r=$(rss 2>/dev/null || echo 0); [ "$r" -gt "$peak" ] && peak=$r && echo $peak > "$peak_file"; sleep 0.2; done ) &
sampler=$!
out=$(taskset -c "$LOAD_CPU" "$LOADCHECK" "$@" 2>&1 || true)
kill $sampler 2>/dev/null; wait $sampler 2>/dev/null || true
c1=$(cpu_ticks); peak=$(cat "$peak_file" 2>/dev/null || echo 0); rm -f "$peak_file"
tck=$(getconf CLK_TCK)

line1=$(echo "$out" | grep '^streams=')
ok=$(echo "$line1" | grep -oE 'ok=[0-9]+' | cut -d= -f2)
total=$(echo "$line1" | grep -oE 'streams=[0-9]+' | cut -d= -f2)
elapsed=$(echo "$line1" | grep -oE 'elapsed=[^ ]+' | cut -d= -f2)
dev=$(echo "$out" | grep '^chunk interval')
ttfb=$(echo "$out" | grep '^time to first')
g() { echo "$1" | grep -oE "$2=[^ ]+" | cut -d= -f2; }
printf '%s\t%s/%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "$ok" "$total" "$elapsed" \
  "$(g "$dev" p50)" "$(g "$dev" p99)" "$(g "$ttfb" p50)" "$(g "$ttfb" p99)" \
  "${peak:-0}" "$idle_rss" \
  "$(( (${c1% *} - ${c0% *}) * 1000 / tck ))/$(( (${c1#* } - ${c0#* }) * 1000 / tck ))"
first_err=$(echo "$out" | grep '^first error' || true)
[ -n "$first_err" ] && echo "    $first_err" >&2
exit 0
