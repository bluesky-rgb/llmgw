package main

import (
	"os"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

// Stats 记录 mock 收到的请求，供测试断言使用。
type Stats struct {
	requests  atomic.Int64
	active    atomic.Int64
	completed atomic.Int64
	cancelled atomic.Int64

	mu      sync.Mutex
	byModel map[string]int64

	// 服务端测得的“实际块间隔与设定间隔的偏差”，用来判断 mock 自己是否按时发送
	jitter [len(jitterBucketsUs) + 1]atomic.Int64
}

// 偏差直方图的桶上限（微秒），最后一个桶表示更大的值
var jitterBucketsUs = [...]int64{100, 250, 500, 1000, 2000, 5000, 10000, 20000, 50000}

// ObserveJitter 记录一次块间隔偏差。
func (s *Stats) ObserveJitter(d time.Duration) {
	if d < 0 {
		d = -d
	}
	us := d.Microseconds()
	for i, ub := range jitterBucketsUs {
		if us <= ub {
			s.jitter[i].Add(1)
			return
		}
	}
	s.jitter[len(jitterBucketsUs)].Add(1)
}

// jitterPercentileUs 返回偏差的近似分位数（桶的上限，单位微秒）；超出最大桶时返回 -1。
func (s *Stats) jitterPercentileUs(p float64) int64 {
	var total int64
	for i := range s.jitter {
		total += s.jitter[i].Load()
	}
	if total == 0 {
		return 0
	}
	target := int64(float64(total) * p)
	var acc int64
	for i := range s.jitter {
		acc += s.jitter[i].Load()
		if acc >= target && acc > 0 {
			if i < len(jitterBucketsUs) {
				return jitterBucketsUs[i]
			}
			return -1
		}
	}
	return -1
}

func NewStats() *Stats { return &Stats{byModel: map[string]int64{}} }

func (s *Stats) OnRequest(model string) {
	s.requests.Add(1)
	s.mu.Lock()
	s.byModel[model]++
	s.mu.Unlock()
}

func (s *Stats) Reset() {
	s.requests.Store(0)
	s.completed.Store(0)
	s.cancelled.Store(0)
	s.mu.Lock()
	s.byModel = map[string]int64{}
	s.mu.Unlock()
	for i := range s.jitter {
		s.jitter[i].Store(0)
	}
}

func (s *Stats) Snapshot() map[string]any {
	s.mu.Lock()
	byModel := make(map[string]int64, len(s.byModel))
	for k, v := range s.byModel {
		byModel[k] = v
	}
	s.mu.Unlock()
	var ms runtime.MemStats
	runtime.ReadMemStats(&ms)
	return map[string]any{
		"requests_total":     s.requests.Load(),
		"requests_by_model":  byModel,
		"streams_active":     s.active.Load(),
		"streams_completed":  s.completed.Load(),
		"streams_cancelled":  s.cancelled.Load(),
		"goroutines":         runtime.NumGoroutine(),
		"memory_rss_bytes":   rssBytes(),
		"memory_sys_bytes":   ms.Sys,
		"send_jitter_p50_us": s.jitterPercentileUs(0.50),
		"send_jitter_p99_us": s.jitterPercentileUs(0.99),
	}
}

// rssBytes 读取进程的常驻内存（仅 Linux），读不到时返回 0。
func rssBytes() int64 {
	data, err := os.ReadFile("/proc/self/status")
	if err != nil {
		return 0
	}
	for _, line := range strings.Split(string(data), "\n") {
		if strings.HasPrefix(line, "VmRSS:") {
			f := strings.Fields(line)
			if len(f) >= 2 {
				kb, _ := strconv.ParseInt(f[1], 10, 64)
				return kb * 1024
			}
		}
	}
	return 0
}

// RecordedRequest 是一条请求记录。
type RecordedRequest struct {
	Time    time.Time         `json:"time"`
	Method  string            `json:"method"`
	Path    string            `json:"path"`
	Headers map[string]string `json:"headers"`
	Body    string            `json:"body"`
}

// Recorder 用环形缓冲区保存最近的请求。
type Recorder struct {
	mu    sync.Mutex
	items []RecordedRequest
	next  int
	full  bool
}

const recorderCapacity = 200
const maxRecordedBody = 64 << 10

func NewRecorder() *Recorder { return &Recorder{items: make([]RecordedRequest, recorderCapacity)} }

func (r *Recorder) Add(req RecordedRequest) {
	if len(req.Body) > maxRecordedBody {
		req.Body = req.Body[:maxRecordedBody]
	}
	r.mu.Lock()
	r.items[r.next] = req
	r.next = (r.next + 1) % recorderCapacity
	if r.next == 0 {
		r.full = true
	}
	r.mu.Unlock()
}

// Last 返回最近的 n 条记录，最新的在最后。
func (r *Recorder) Last(n int) []RecordedRequest {
	r.mu.Lock()
	defer r.mu.Unlock()
	size := r.next
	if r.full {
		size = recorderCapacity
	}
	if n <= 0 || n > size {
		n = size
	}
	out := make([]RecordedRequest, 0, n)
	for i := size - n; i < size; i++ {
		idx := (r.next - size + i + recorderCapacity) % recorderCapacity
		out = append(out, r.items[idx])
	}
	return out
}

func (r *Recorder) Reset() {
	r.mu.Lock()
	r.next, r.full = 0, false
	r.mu.Unlock()
}
