package main

import (
	"fmt"
	"strconv"
	"strings"
	"time"
)

// Behavior 描述一次请求中 mock 上游应该怎样表现。
// 通过模型名控制，例如 "mock:status=503,retry_after=2"。
type Behavior struct {
	Status          int           // 非 0 时直接返回这个状态码
	RetryAfter      int           // >0 时带上 Retry-After 响应头（秒）
	HeaderDelay     time.Duration // 返回响应头之前的等待
	Blackhole       bool          // 永远不返回任何数据
	TTFT            time.Duration // 响应头之后、第一个数据块之前的等待
	Chunks          int           // 流式输出的数据块数
	Interval        time.Duration // 数据块之间的间隔
	StallAfter      int           // >=0 时，发出这么多块后停住
	DisconnectAfter int           // >=0 时，发出这么多块后直接断开连接
	BizError        int           // 非 0 时返回 HTTP 200 + base_resp.status_code（MiniMax 风格）
	Reasoning       bool          // 输出中包含 reasoning_content
	Usage           bool          // 是否返回 usage
	QuotaRequests   int           // >=0 时带上 x-ratelimit-remaining-requests
	QuotaTokens     int           // >=0 时带上 x-ratelimit-remaining-tokens
	Echo            bool          // 把收到的请求体放进回答内容
}

// Defaults 来自命令行参数，模型名中没有指定的项使用这里的值。
type Defaults struct {
	TTFT     time.Duration
	Chunks   int
	Interval time.Duration
}

// ParseBehavior 解析模型名。不以 "mock" 开头的模型名使用默认行为，
// 这样也可以用 "gpt" 之类的普通名字做测试。无法识别的参数返回错误，
// 避免测试因为拼写错误而“意外通过”。
func ParseBehavior(model string, d Defaults) (Behavior, error) {
	b := Behavior{
		TTFT: d.TTFT, Chunks: d.Chunks, Interval: d.Interval,
		StallAfter: -1, DisconnectAfter: -1, QuotaRequests: -1, QuotaTokens: -1,
		Usage: true,
	}
	name, params, found := strings.Cut(model, ":")
	if name != "mock" || !found || params == "" {
		return b, nil
	}
	for _, kv := range strings.Split(params, ",") {
		key, val, ok := strings.Cut(strings.TrimSpace(kv), "=")
		if !ok {
			return b, fmt.Errorf("参数 %q 缺少 \"=\"", kv)
		}
		var err error
		switch key {
		case "status":
			b.Status, err = parseInt(val, 100, 599)
		case "retry_after":
			b.RetryAfter, err = parseInt(val, 0, 3600)
		case "header_delay":
			b.HeaderDelay, err = time.ParseDuration(val)
		case "blackhole":
			b.Blackhole, err = strconv.ParseBool(val)
		case "ttft":
			b.TTFT, err = time.ParseDuration(val)
		case "chunks":
			b.Chunks, err = parseInt(val, 0, 100000)
		case "interval":
			b.Interval, err = time.ParseDuration(val)
		case "stall_after":
			b.StallAfter, err = parseInt(val, 0, 100000)
		case "disconnect_after":
			b.DisconnectAfter, err = parseInt(val, 0, 100000)
		case "biz_error":
			b.BizError, err = parseInt(val, 1, 1<<30)
		case "reasoning":
			b.Reasoning, err = strconv.ParseBool(val)
		case "usage":
			b.Usage, err = strconv.ParseBool(val)
		case "quota_requests":
			b.QuotaRequests, err = parseInt(val, 0, 1<<30)
		case "quota_tokens":
			b.QuotaTokens, err = parseInt(val, 0, 1<<30)
		case "echo":
			b.Echo, err = strconv.ParseBool(val)
		default:
			return b, fmt.Errorf("无法识别的参数 %q", key)
		}
		if err != nil {
			return b, fmt.Errorf("参数 %s 的值 %q 无效：%v", key, val, err)
		}
	}
	return b, nil
}

func parseInt(s string, lo, hi int) (int, error) {
	v, err := strconv.Atoi(s)
	if err != nil {
		return 0, err
	}
	if v < lo || v > hi {
		return 0, fmt.Errorf("超出范围 [%d, %d]", lo, hi)
	}
	return v, nil
}
