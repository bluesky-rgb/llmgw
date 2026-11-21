// mock-upstream：可以按需制造各种上游故障的假大模型服务，供 llmgw 的测试和压测使用。
// 用法和行为参数见 README.md。
package main

import (
	"flag"
	"log"
	"net/http"
	"time"
)

func main() {
	listen := flag.String("listen", "127.0.0.1:18081", "监听地址")
	ttft := flag.Duration("ttft", 0, "默认首字延迟")
	chunks := flag.Int("chunks", 5, "默认的流式数据块数")
	interval := flag.Duration("interval", 0, "默认的数据块间隔")
	errorRate := flag.Float64("error-rate", 0, "随机返回 503 的比例（0~1）")
	seed := flag.Int64("seed", 1, "随机数种子")
	flag.Parse()

	s := NewServer(Defaults{TTFT: *ttft, Chunks: *chunks, Interval: *interval}, *errorRate, *seed)
	srv := &http.Server{
		Addr:              *listen,
		Handler:           s.Handler(),
		ReadHeaderTimeout: 10 * time.Second,
	}
	log.Printf("mock-upstream listening on %s", *listen)
	log.Fatal(srv.ListenAndServe())
}
