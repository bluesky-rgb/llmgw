#!/bin/bash
# 用指定的二进制跑一遍功能、断开、超时、并发场景，最后正常退出，检查 sanitizer 报告
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=$1; LOG=$2; LOADCHECK=${LOADCHECK:-loadcheck}
pkill -x coroutine-proxy; sleep 0.5
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 TSAN_OPTIONS=halt_on_error=0 setsid $BIN --workers 2 --total-timeout 3s --idle-timeout 2s > $LOG 2>&1 &
sleep 1.5
U=http://127.0.0.1:18100/v1/chat/completions
curl -s -o /dev/null $U -d '{"model":"mock"}'
curl -sN -o /dev/null $U -d '{"model":"mock:chunks=50","stream":true}'
curl -sN -o /dev/null $U -d '{"model":"mock:chunks=20000,interval=0s","stream":true}'
curl -s -o /dev/null $U -d '{"model":"mock:status=503"}'
curl -s -o /dev/null $U -d '{"model":"mock:blackhole=true"}'
curl -sN -o /dev/null $U -d '{"model":"mock:stall_after=2","stream":true}'
curl -sN -o /dev/null $U -d '{"model":"mock:disconnect_after=3","stream":true}'
python3 -I "$HERE"/cancel_latency.py 18100 "mock:chunks=100000,interval=100ms" "" 10
python3 -I "$HERE"/cancel_before_header.py 18100
python3 -I "$HERE"/slow_reader.py 127.0.0.1 18100 "mock:chunks=50000,interval=0s" 200 2
"$LOADCHECK" -url http://127.0.0.1:18100 -streams 500 -chunks 20 -interval 50ms -ramp 1s 2>&1 | head -1
curl -s http://127.0.0.1:18100/_proxy/stats; echo
pkill -INT -x coroutine-proxy; sleep 3
n=$(grep -cE "ERROR: (Address|Leak)Sanitizer|WARNING: ThreadSanitizer|runtime error" $LOG)
echo "--- sanitizer 报告数: $n（详情见 $LOG）"
[ "$n" -eq 0 ]
