package main

import (
	"strings"
	"testing"
	"time"
)

var testDefaults = Defaults{TTFT: 0, Chunks: 5, Interval: 0}

func TestParseBehaviorDefaults(t *testing.T) {
	for _, model := range []string{"gpt-5.6-sol", "mock", "mock:"} {
		b, err := ParseBehavior(model, testDefaults)
		if err != nil {
			t.Fatalf("%s: %v", model, err)
		}
		if b.Status != 0 || b.Chunks != 5 || !b.Usage || b.StallAfter != -1 || b.DisconnectAfter != -1 ||
			b.QuotaRequests != -1 || b.QuotaTokens != -1 {
			t.Fatalf("%s: unexpected defaults %+v", model, b)
		}
	}
}

func TestParseBehaviorAllParams(t *testing.T) {
	b, err := ParseBehavior("mock:status=503,retry_after=2,header_delay=1s,blackhole=true,ttft=2s,"+
		"chunks=20,interval=50ms,stall_after=3,disconnect_after=4,biz_error=1002,reasoning=true,"+
		"usage=false,quota_requests=7,quota_tokens=900,echo=true", testDefaults)
	if err != nil {
		t.Fatal(err)
	}
	want := Behavior{Status: 503, RetryAfter: 2, HeaderDelay: time.Second, Blackhole: true, TTFT: 2 * time.Second,
		Chunks: 20, Interval: 50 * time.Millisecond, StallAfter: 3, DisconnectAfter: 4, BizError: 1002,
		Reasoning: true, Usage: false, QuotaRequests: 7, QuotaTokens: 900, Echo: true}
	if b != want {
		t.Fatalf("got %+v\nwant %+v", b, want)
	}
}

func TestParseBehaviorRejectsMistakes(t *testing.T) {
	cases := map[string]string{
		"mock:stauts=503":   "无法识别的参数",
		"mock:status":       "缺少",
		"mock:status=abc":   "无效",
		"mock:status=99":    "超出范围",
		"mock:ttft=2":       "无效", // 缺少单位
		"mock:chunks=-1":    "超出范围",
		"mock:reasoning=ok": "无效",
	}
	for model, wantMsg := range cases {
		if _, err := ParseBehavior(model, testDefaults); err == nil || !strings.Contains(err.Error(), wantMsg) {
			t.Errorf("%s: want error containing %q, got %v", model, wantMsg, err)
		}
	}
}
