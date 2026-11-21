package main

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func newTestServer(t *testing.T, errorRate float64) (*Server, *httptest.Server) {
	t.Helper()
	s := NewServer(testDefaults, errorRate, 1)
	ts := httptest.NewServer(s.Handler())
	t.Cleanup(ts.Close)
	return s, ts
}

func post(t *testing.T, ctx context.Context, url, body string) (*http.Response, error) {
	t.Helper()
	req, _ := http.NewRequestWithContext(ctx, http.MethodPost, url+"/v1/chat/completions", strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	return http.DefaultClient.Do(req)
}

func chatBody(model string, stream, includeUsage bool) string {
	b := map[string]any{"model": model, "stream": stream, "messages": []any{map[string]any{"role": "user", "content": "hi"}}}
	if includeUsage {
		b["stream_options"] = map[string]any{"include_usage": true}
	}
	out, _ := json.Marshal(b)
	return string(out)
}

// readEvents 读取 SSE 的 data 行，返回读到的事件和读取结束时的错误。
func readEvents(r io.Reader) ([]string, error) {
	var events []string
	sc := bufio.NewScanner(r)
	for sc.Scan() {
		if line := sc.Text(); strings.HasPrefix(line, "data: ") {
			events = append(events, strings.TrimPrefix(line, "data: "))
		}
	}
	return events, sc.Err()
}

func waitFor(t *testing.T, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for %s", what)
}

func decode(t *testing.T, r io.Reader) map[string]any {
	t.Helper()
	var m map[string]any
	if err := json.NewDecoder(r).Decode(&m); err != nil {
		t.Fatal(err)
	}
	return m
}

func TestNonStreamOK(t *testing.T) {
	s, ts := newTestServer(t, 0)
	resp, err := post(t, context.Background(), ts.URL, chatBody("mock:reasoning=true", false, false))
	if err != nil || resp.StatusCode != 200 {
		t.Fatalf("status %v err %v", resp, err)
	}
	m := decode(t, resp.Body)
	msg := m["choices"].([]any)[0].(map[string]any)["message"].(map[string]any)
	if msg["reasoning_content"] != "mock reasoning" || m["usage"] == nil {
		t.Fatalf("unexpected body %v", m)
	}
	if got := s.stats.Snapshot()["requests_by_model"].(map[string]int64)["mock:reasoning=true"]; got != 1 {
		t.Fatalf("requests_by_model = %d", got)
	}
}

func TestNonStreamWithoutUsageAndEcho(t *testing.T) {
	_, ts := newTestServer(t, 0)
	resp, _ := post(t, context.Background(), ts.URL, chatBody("mock:usage=false,echo=true", false, false))
	m := decode(t, resp.Body)
	content := m["choices"].([]any)[0].(map[string]any)["message"].(map[string]any)["content"].(string)
	if m["usage"] != nil || !strings.Contains(content, `"messages"`) {
		t.Fatalf("unexpected body %v", m)
	}
}

func TestErrorStatusWithRetryAfter(t *testing.T) {
	_, ts := newTestServer(t, 0)
	resp, _ := post(t, context.Background(), ts.URL, chatBody("mock:status=503,retry_after=2", true, false))
	if resp.StatusCode != 503 || resp.Header.Get("Retry-After") != "2" {
		t.Fatalf("status %d retry-after %q", resp.StatusCode, resp.Header.Get("Retry-After"))
	}
	if code := decode(t, resp.Body)["error"].(map[string]any)["code"]; code != "mock_error" {
		t.Fatalf("code %v", code)
	}
}

func TestBusinessErrorInBody(t *testing.T) {
	_, ts := newTestServer(t, 0)
	resp, _ := post(t, context.Background(), ts.URL, chatBody("mock:biz_error=1002", false, false))
	m := decode(t, resp.Body)
	if resp.StatusCode != 200 || m["base_resp"].(map[string]any)["status_code"].(float64) != 1002 {
		t.Fatalf("status %d body %v", resp.StatusCode, m)
	}
}

func TestQuotaHeaders(t *testing.T) {
	_, ts := newTestServer(t, 0)
	resp, _ := post(t, context.Background(), ts.URL, chatBody("mock:quota_requests=3,quota_tokens=500", false, false))
	if resp.Header.Get("x-ratelimit-remaining-requests") != "3" || resp.Header.Get("x-ratelimit-remaining-tokens") != "500" {
		t.Fatalf("headers %v", resp.Header)
	}
}

func TestStreamChunksUsageAndDone(t *testing.T) {
	s, ts := newTestServer(t, 0)
	resp, _ := post(t, context.Background(), ts.URL, chatBody("mock:chunks=3,reasoning=true", true, true))
	if resp.Header.Get("Content-Type") != "text/event-stream" {
		t.Fatalf("content-type %q", resp.Header.Get("Content-Type"))
	}
	events, err := readEvents(resp.Body)
	if err != nil {
		t.Fatal(err)
	}
	// 3 个内容块 + 结束块 + usage 块 + [DONE]
	if len(events) != 6 || events[5] != "[DONE]" || !strings.Contains(events[0], "reasoning_content") ||
		!strings.Contains(events[4], `"usage"`) {
		t.Fatalf("events %v", events)
	}
	waitFor(t, "completed", func() bool { return s.stats.completed.Load() == 1 })
}

func TestStreamUsageOnlyWhenRequested(t *testing.T) {
	_, ts := newTestServer(t, 0)
	resp, _ := post(t, context.Background(), ts.URL, chatBody("mock:chunks=2", true, false))
	events, _ := readEvents(resp.Body)
	for _, e := range events {
		if strings.Contains(e, `"usage"`) {
			t.Fatalf("usage sent without include_usage: %v", events)
		}
	}
	resp, _ = post(t, context.Background(), ts.URL, chatBody("mock:chunks=2,usage=false", true, true))
	events, _ = readEvents(resp.Body)
	for _, e := range events {
		if strings.Contains(e, `"usage"`) {
			t.Fatalf("usage sent despite usage=false: %v", events)
		}
	}
}

func TestDisconnectMidStream(t *testing.T) {
	_, ts := newTestServer(t, 0)
	resp, _ := post(t, context.Background(), ts.URL, chatBody("mock:chunks=5,disconnect_after=2", true, false))
	events, err := readEvents(resp.Body)
	if err == nil || !errors.Is(err, io.ErrUnexpectedEOF) && !strings.Contains(err.Error(), "unexpected EOF") {
		t.Fatalf("want unexpected EOF, got %v (events %v)", err, events)
	}
	if len(events) != 2 {
		t.Fatalf("want 2 events before disconnect, got %v", events)
	}
}

func TestStallAfterIsCountedAsCancelledWhenClientGivesUp(t *testing.T) {
	s, ts := newTestServer(t, 0)
	ctx, cancel := context.WithTimeout(context.Background(), 200*time.Millisecond)
	defer cancel()
	resp, err := post(t, ctx, ts.URL, chatBody("mock:chunks=5,stall_after=1", true, false))
	if err != nil {
		t.Fatal(err)
	}
	events, _ := readEvents(resp.Body)
	if len(events) != 1 {
		t.Fatalf("want 1 event before stall, got %v", events)
	}
	waitFor(t, "cancelled", func() bool { return s.stats.cancelled.Load() == 1 })
}

func TestBlackholeNeverResponds(t *testing.T) {
	s, ts := newTestServer(t, 0)
	ctx, cancel := context.WithTimeout(context.Background(), 200*time.Millisecond)
	defer cancel()
	start := time.Now()
	if _, err := post(t, ctx, ts.URL, chatBody("mock:blackhole=true", true, false)); err == nil {
		t.Fatal("want timeout error")
	}
	if time.Since(start) < 190*time.Millisecond {
		t.Fatal("returned too early")
	}
	waitFor(t, "cancelled", func() bool { return s.stats.cancelled.Load() == 1 })
}

func TestHeaderDelay(t *testing.T) {
	_, ts := newTestServer(t, 0)
	start := time.Now()
	resp, _ := post(t, context.Background(), ts.URL, chatBody("mock:header_delay=200ms", false, false))
	if resp.StatusCode != 200 || time.Since(start) < 200*time.Millisecond {
		t.Fatalf("status %d after %v", resp.StatusCode, time.Since(start))
	}
}

func TestClientCancelMidStreamIsDetected(t *testing.T) {
	s, ts := newTestServer(t, 0)
	ctx, cancel := context.WithCancel(context.Background())
	resp, _ := post(t, ctx, ts.URL, chatBody("mock:chunks=10,interval=100ms", true, false))
	br := bufio.NewReader(resp.Body)
	if _, err := br.ReadString('\n'); err != nil { // 读到第一块
		t.Fatal(err)
	}
	cancel()
	waitFor(t, "cancelled", func() bool { return s.stats.cancelled.Load() == 1 })
	if s.stats.completed.Load() != 0 {
		t.Fatal("stream should not be completed")
	}
}

func TestRejectsBadInput(t *testing.T) {
	_, ts := newTestServer(t, 0)
	resp, _ := post(t, context.Background(), ts.URL, "not json")
	if resp.StatusCode != 400 {
		t.Fatalf("invalid json: %d", resp.StatusCode)
	}
	resp, _ = post(t, context.Background(), ts.URL, chatBody("mock:stauts=503", false, false))
	if resp.StatusCode != 400 || decode(t, resp.Body)["error"].(map[string]any)["code"] != "invalid_mock_behavior" {
		t.Fatalf("typo should be rejected: %d", resp.StatusCode)
	}
}

func TestErrorRate(t *testing.T) {
	_, ts := newTestServer(t, 1.0)
	resp, _ := post(t, context.Background(), ts.URL, chatBody("mock", false, false))
	if resp.StatusCode != 503 {
		t.Fatalf("status %d", resp.StatusCode)
	}
}

func TestRecorderKeepsLastRequestsAndReset(t *testing.T) {
	s, ts := newTestServer(t, 0)
	for i := 0; i < recorderCapacity+50; i++ {
		s.recorder.Add(RecordedRequest{Body: strings.Repeat("x", i)})
	}
	all := s.recorder.Last(0)
	if len(all) != recorderCapacity || len(all[len(all)-1].Body) != recorderCapacity+49 {
		t.Fatalf("len %d last %d", len(all), len(all[len(all)-1].Body))
	}
	if got := s.recorder.Last(3); len(got) != 3 || len(got[2].Body) != recorderCapacity+49 {
		t.Fatalf("Last(3) wrong")
	}
	resp, _ := http.Post(ts.URL+"/_mock/reset", "application/json", nil)
	resp.Body.Close()
	if len(s.recorder.Last(0)) != 0 || s.stats.requests.Load() != 0 {
		t.Fatal("reset did not clear state")
	}
}

func TestRequestsEndpointShowsHeadersAndBody(t *testing.T) {
	_, ts := newTestServer(t, 0)
	req, _ := http.NewRequest(http.MethodPost, ts.URL+"/v1/chat/completions", strings.NewReader(chatBody("mock", false, false)))
	req.Header.Set("X-Request-Id", "req-123")
	resp, _ := http.DefaultClient.Do(req)
	resp.Body.Close()
	resp, _ = http.Get(ts.URL + "/_mock/requests?limit=1")
	var got []RecordedRequest
	_ = json.NewDecoder(resp.Body).Decode(&got)
	if len(got) != 1 || got[0].Headers["X-Request-Id"] != "req-123" || !strings.Contains(got[0].Body, `"model":"mock"`) {
		t.Fatalf("got %+v", got)
	}
}

func TestSendJitterIsMeasured(t *testing.T) {
	s, ts := newTestServer(t, 0)
	resp, _ := post(t, context.Background(), ts.URL, chatBody("mock:chunks=5,interval=20ms", true, false))
	_, _ = readEvents(resp.Body)
	var total int64
	for i := range s.stats.jitter {
		total += s.stats.jitter[i].Load()
	}
	if total != 4 { // 5 个块之间有 4 个间隔
		t.Fatalf("want 4 jitter samples, got %d", total)
	}
	if p99 := s.stats.jitterPercentileUs(0.99); p99 <= 0 || p99 > 20000 {
		t.Fatalf("unexpected p99 %d", p99)
	}
}
