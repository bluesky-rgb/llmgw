// loadcheck：同时打开大量流式请求，测量成功率、首字节时延和数据块间隔的偏差。
// 直接压 mock 时用于验证 mock 自身不会成为瓶颈；压网关时用于测量网关的额外延迟。
package main

import (
	"bufio"
	"encoding/json"
	"flag"
	"fmt"
	"net"
	"net/http"
	"os"
	"sort"
	"strings"
	"sync"
	"time"
)

func main() {
	url := flag.String("url", "http://127.0.0.1:18081", "mock 上游地址")
	streams := flag.Int("streams", 5000, "并发流数")
	chunks := flag.Int("chunks", 20, "每条流的数据块数")
	interval := flag.Duration("interval", 50*time.Millisecond, "数据块间隔")
	ramp := flag.Duration("ramp", 2*time.Second, "在这段时间内逐步建立所有连接")
	modelFlag := flag.String("model", "", "请求中的模型名；默认根据 -chunks 和 -interval 生成 mock 模型名")
	auth := flag.String("auth", "", "Bearer 令牌（压需要鉴权的网关时使用）")
	statsURL := flag.String("stats-url", "", "mock 统计接口所在地址；默认与 -url 相同")
	flag.Parse()
	if *statsURL == "" {
		*statsURL = *url
	}

	client := &http.Client{Transport: &http.Transport{
		MaxIdleConnsPerHost: *streams,
		DialContext:         (&net.Dialer{Timeout: 10 * time.Second}).DialContext,
	}}
	model := *modelFlag
	if model == "" {
		model = fmt.Sprintf("mock:chunks=%d,interval=%s", *chunks, *interval)
	}
	body := fmt.Sprintf(`{"model":%q,"stream":true,"messages":[{"role":"user","content":"hi"}]}`, model)

	var (
		mu       sync.Mutex
		gaps     []time.Duration
		ttfbs    []time.Duration
		ok, fail int
		firstErr error
		peakRSS  int64
	)
	stopSampling := make(chan struct{})
	go func() { // 定期采样 mock 的内存，记录峰值
		for {
			select {
			case <-stopSampling:
				return
			case <-time.After(200 * time.Millisecond):
				if rss := mockRSS(*statsURL); rss > peakRSS {
					peakRSS = rss
				}
			}
		}
	}()

	var wg sync.WaitGroup
	start := time.Now()
	for i := 0; i < *streams; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			time.Sleep(time.Duration(int64(*ramp) * int64(i) / int64(*streams)))
			local, ttfb, err := runStream(client, *url, body, *auth)
			mu.Lock()
			defer mu.Unlock()
			if err != nil || len(local) != *chunks-1 {
				fail++
				if firstErr == nil {
					firstErr = fmt.Errorf("gaps=%d err=%v", len(local), err)
				}
				return
			}
			ok++
			gaps = append(gaps, local...)
			ttfbs = append(ttfbs, ttfb)
		}(i)
	}
	wg.Wait()
	close(stopSampling)
	elapsed := time.Since(start)

	devs := make([]time.Duration, len(gaps))
	for i, g := range gaps {
		d := g - *interval
		if d < 0 {
			d = -d
		}
		devs[i] = d
	}
	pct := percentiles(devs)
	ttfbPct := percentiles(ttfbs)
	fmt.Printf("streams=%d ok=%d fail=%d elapsed=%s\n", *streams, ok, fail, elapsed.Round(time.Millisecond))
	fmt.Printf("chunk interval deviation: p50=%s p99=%s max=%s (samples=%d)\n",
		pct(0.50).Round(10*time.Microsecond), pct(0.99).Round(10*time.Microsecond), pct(1).Round(10*time.Microsecond), len(devs))
	fmt.Printf("time to first chunk: p50=%s p99=%s max=%s\n",
		ttfbPct(0.50).Round(10*time.Microsecond), ttfbPct(0.99).Round(10*time.Microsecond), ttfbPct(1).Round(10*time.Microsecond))
	fmt.Printf("mock peak RSS: %.1f MB\n", float64(peakRSS)/1024/1024)
	if p50, p99 := mockJitter(*statsURL); p99 != 0 {
		fmt.Printf("mock-side send deviation (bucket upper bound): p50<=%dus p99<=%dus\n", p50, p99)
	}
	if firstErr != nil {
		fmt.Println("first error:", firstErr)
	}
	if fail > 0 {
		os.Exit(1)
	}
}

// percentiles 对样本排序，返回按分位数取值的函数。
func percentiles(v []time.Duration) func(p float64) time.Duration {
	sort.Slice(v, func(a, b int) bool { return v[a] < v[b] })
	return func(p float64) time.Duration {
		if len(v) == 0 {
			return 0
		}
		return v[int(float64(len(v)-1)*p)]
	}
}

// runStream 发起一个流式请求，返回相邻两个内容块之间的时间间隔，
// 以及从发出请求到收到第一个内容块的时间。
func runStream(client *http.Client, url, body, auth string) ([]time.Duration, time.Duration, error) {
	req, err := http.NewRequest(http.MethodPost, url+"/v1/chat/completions", strings.NewReader(body))
	if err != nil {
		return nil, 0, err
	}
	req.Header.Set("Content-Type", "application/json")
	if auth != "" {
		req.Header.Set("Authorization", "Bearer "+auth)
	}
	start := time.Now()
	resp, err := client.Do(req)
	if err != nil {
		return nil, 0, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, 0, fmt.Errorf("HTTP %d", resp.StatusCode)
	}
	var ttfb time.Duration
	var gaps []time.Duration
	var last time.Time
	sc := bufio.NewScanner(resp.Body)
	for sc.Scan() {
		line := sc.Text()
		if !strings.HasPrefix(line, "data: ") || !strings.Contains(line, `"content"`) {
			continue
		}
		now := time.Now()
		if last.IsZero() {
			ttfb = now.Sub(start)
		} else {
			gaps = append(gaps, now.Sub(last))
		}
		last = now
	}
	return gaps, ttfb, sc.Err()
}

func mockRSS(url string) int64 {
	resp, err := http.Get(url + "/_mock/stats")
	if err != nil {
		return 0
	}
	defer resp.Body.Close()
	var s struct {
		RSS int64 `json:"memory_rss_bytes"`
	}
	_ = json.NewDecoder(resp.Body).Decode(&s)
	return s.RSS
}

// mockJitter 读取 mock 服务端测得的发送偏差分位数（微秒）。
func mockJitter(url string) (int64, int64) {
	resp, err := http.Get(url + "/_mock/stats")
	if err != nil {
		return 0, 0
	}
	defer resp.Body.Close()
	var s struct {
		P50 int64 `json:"send_jitter_p50_us"`
		P99 int64 `json:"send_jitter_p99_us"`
	}
	_ = json.NewDecoder(resp.Body).Decode(&s)
	return s.P50, s.P99
}
