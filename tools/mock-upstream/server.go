package main

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"math/rand"
	"net/http"
	"strconv"
	"sync"
	"time"
)

const maxBodyBytes = 32 << 20

// Server 是 mock 上游的 HTTP 处理器。
type Server struct {
	defaults  Defaults
	errorRate float64
	stats     *Stats
	recorder  *Recorder

	rngMu sync.Mutex
	rng   *rand.Rand
}

func NewServer(d Defaults, errorRate float64, seed int64) *Server {
	return &Server{defaults: d, errorRate: errorRate, stats: NewStats(), recorder: NewRecorder(),
		rng: rand.New(rand.NewSource(seed))}
}

func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("POST /v1/chat/completions", s.handleChat)
	mux.HandleFunc("GET /v1/models", func(w http.ResponseWriter, _ *http.Request) {
		writeJSON(w, http.StatusOK, map[string]any{"object": "list",
			"data": []any{map[string]any{"id": "mock", "object": "model", "owned_by": "mock"}}})
	})
	mux.HandleFunc("GET /health", func(w http.ResponseWriter, _ *http.Request) {
		writeJSON(w, http.StatusOK, map[string]any{"status": "ok"})
	})
	mux.HandleFunc("GET /_mock/stats", func(w http.ResponseWriter, _ *http.Request) {
		writeJSON(w, http.StatusOK, s.stats.Snapshot())
	})
	mux.HandleFunc("GET /_mock/requests", func(w http.ResponseWriter, r *http.Request) {
		n, _ := strconv.Atoi(r.URL.Query().Get("limit"))
		writeJSON(w, http.StatusOK, s.recorder.Last(n))
	})
	mux.HandleFunc("POST /_mock/reset", func(w http.ResponseWriter, _ *http.Request) {
		s.stats.Reset()
		s.recorder.Reset()
		writeJSON(w, http.StatusOK, map[string]any{"reset": true})
	})
	return mux
}

type chatRequest struct {
	Model         string `json:"model"`
	Stream        bool   `json:"stream"`
	StreamOptions *struct {
		IncludeUsage bool `json:"include_usage"`
	} `json:"stream_options"`
}

func (s *Server) handleChat(w http.ResponseWriter, r *http.Request) {
	raw, err := io.ReadAll(io.LimitReader(r.Body, maxBodyBytes+1))
	if err != nil || len(raw) > maxBodyBytes {
		writeError(w, http.StatusRequestEntityTooLarge, "request body too large", "payload_too_large")
		return
	}
	s.record(r, raw)

	var req chatRequest
	if err := json.Unmarshal(raw, &req); err != nil {
		writeError(w, http.StatusBadRequest, "invalid JSON: "+err.Error(), "invalid_json")
		return
	}
	s.stats.OnRequest(req.Model)

	b, err := ParseBehavior(req.Model, s.defaults)
	if err != nil {
		writeError(w, http.StatusBadRequest, "mock: "+err.Error(), "invalid_mock_behavior")
		return
	}
	if s.randomFailure() {
		b.Status = http.StatusServiceUnavailable
	}
	ctx := r.Context()

	if b.Blackhole {
		<-ctx.Done()
		s.stats.cancelled.Add(1)
		return
	}
	if !sleep(ctx, b.HeaderDelay) {
		s.stats.cancelled.Add(1)
		return
	}
	setQuotaHeaders(w, b)
	if b.Status != 0 {
		if b.RetryAfter > 0 {
			w.Header().Set("Retry-After", strconv.Itoa(b.RetryAfter))
		}
		writeError(w, b.Status, fmt.Sprintf("mock error %d", b.Status), "mock_error")
		return
	}
	if b.BizError != 0 {
		writeJSON(w, http.StatusOK, map[string]any{
			"id": "chatcmpl-mock", "object": "chat.completion", "choices": []any{},
			"base_resp": map[string]any{"status_code": b.BizError, "status_msg": "mock business error"},
		})
		return
	}
	content := "Hello from mock upstream."
	if b.Echo {
		content = string(raw)
	}
	if req.Stream {
		includeUsage := req.StreamOptions != nil && req.StreamOptions.IncludeUsage
		s.stream(w, r, req.Model, b, content, includeUsage && b.Usage)
		return
	}
	if !sleep(ctx, b.TTFT) {
		s.stats.cancelled.Add(1)
		return
	}
	msg := map[string]any{"role": "assistant", "content": content}
	if b.Reasoning {
		msg["reasoning_content"] = "mock reasoning"
	}
	resp := map[string]any{
		"id": "chatcmpl-mock", "object": "chat.completion", "created": time.Now().Unix(), "model": req.Model,
		"choices": []any{map[string]any{"index": 0, "message": msg, "finish_reason": "stop"}},
	}
	if b.Usage {
		resp["usage"] = usage(b.Chunks)
	}
	writeJSON(w, http.StatusOK, resp)
}

func (s *Server) stream(w http.ResponseWriter, r *http.Request, model string, b Behavior, content string, withUsage bool) {
	ctx := r.Context()
	rc := http.NewResponseController(w)
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.WriteHeader(http.StatusOK)
	_ = rc.Flush()

	s.stats.active.Add(1)
	defer s.stats.active.Add(-1)
	cancelled := func() { s.stats.cancelled.Add(1) }

	if !sleep(ctx, b.TTFT) {
		cancelled()
		return
	}
	created := time.Now().Unix()
	// 内容块的固定部分只构造一次，每块只需要编码内容字符串（性能见任务设计 M4）
	modelJSON, _ := json.Marshal(model)
	prefix := fmt.Sprintf(`data: {"id":"chatcmpl-mock","object":"chat.completion.chunk","created":%d,"model":%s,`+
		`"choices":[{"index":0,"delta":{"content":`, created, modelJSON)
	const suffix = "},\"finish_reason\":null}]}\n\n"
	buf := make([]byte, 0, 512)
	var lastSend time.Time
	for i := 0; i < b.Chunks; i++ {
		if b.StallAfter >= 0 && i == b.StallAfter {
			<-ctx.Done()
			cancelled()
			return
		}
		if b.DisconnectAfter >= 0 && i == b.DisconnectAfter {
			// 不发送结束标记，直接断开连接，模拟上游输出到一半崩溃
			panic(http.ErrAbortHandler)
		}
		if b.Reasoning && i == 0 {
			if !writeEvent(w, rc, chunk(model, created, map[string]any{"reasoning_content": "mock reasoning"}, nil)) {
				cancelled()
				return
			}
		} else {
			text, _ := json.Marshal(chunkText(content, i, b.Chunks))
			buf = append(append(append(buf[:0], prefix...), text...), suffix...)
			if _, err := w.Write(buf); err != nil || rc.Flush() != nil {
				cancelled()
				return
			}
		}
		if i > 0 && b.Interval > 0 {
			now := time.Now()
			s.stats.ObserveJitter(now.Sub(lastSend) - b.Interval)
			lastSend = now
		} else {
			lastSend = time.Now()
		}
		if i+1 < b.Chunks && !sleep(ctx, b.Interval) {
			cancelled()
			return
		}
	}
	stop := "stop"
	if !writeEvent(w, rc, chunk(model, created, map[string]any{}, &stop)) {
		cancelled()
		return
	}
	if withUsage {
		u := map[string]any{"id": "chatcmpl-mock", "object": "chat.completion.chunk", "created": created,
			"model": model, "choices": []any{}, "usage": usage(b.Chunks)}
		if !writeEvent(w, rc, u) {
			cancelled()
			return
		}
	}
	if _, err := io.WriteString(w, "data: [DONE]\n\n"); err != nil || rc.Flush() != nil {
		cancelled()
		return
	}
	s.stats.completed.Add(1)
}

func (s *Server) record(r *http.Request, body []byte) {
	h := make(map[string]string, len(r.Header))
	for k, v := range r.Header {
		if len(v) > 0 {
			h[k] = v[0]
		}
	}
	s.recorder.Add(RecordedRequest{Time: time.Now(), Method: r.Method, Path: r.URL.Path, Headers: h, Body: string(body)})
}

func (s *Server) randomFailure() bool {
	if s.errorRate <= 0 {
		return false
	}
	s.rngMu.Lock()
	defer s.rngMu.Unlock()
	return s.rng.Float64() < s.errorRate
}

// sleep 等待 d，期间客户端断开则返回 false。
func sleep(ctx context.Context, d time.Duration) bool {
	if d <= 0 {
		return ctx.Err() == nil
	}
	t := time.NewTimer(d)
	defer t.Stop()
	select {
	case <-t.C:
		return true
	case <-ctx.Done():
		return false
	}
}

func setQuotaHeaders(w http.ResponseWriter, b Behavior) {
	if b.QuotaRequests >= 0 {
		w.Header().Set("x-ratelimit-remaining-requests", strconv.Itoa(b.QuotaRequests))
	}
	if b.QuotaTokens >= 0 {
		w.Header().Set("x-ratelimit-remaining-tokens", strconv.Itoa(b.QuotaTokens))
	}
}

func chunk(model string, created int64, delta map[string]any, finish *string) map[string]any {
	var fr any
	if finish != nil {
		fr = *finish
	}
	return map[string]any{"id": "chatcmpl-mock", "object": "chat.completion.chunk", "created": created,
		"model": model, "choices": []any{map[string]any{"index": 0, "delta": delta, "finish_reason": fr}}}
}

// chunkText 把内容切成 n 份，第 i 份作为第 i 个数据块。
func chunkText(content string, i, n int) string {
	runes := []rune(content)
	if n <= 0 {
		return ""
	}
	start, end := len(runes)*i/n, len(runes)*(i+1)/n
	return string(runes[start:end])
}

func usage(chunks int) map[string]any {
	return map[string]any{"prompt_tokens": 10, "completion_tokens": chunks, "total_tokens": 10 + chunks}
}

func writeEvent(w http.ResponseWriter, rc *http.ResponseController, v any) bool {
	data, _ := json.Marshal(v)
	if _, err := fmt.Fprintf(w, "data: %s\n\n", data); err != nil {
		return false
	}
	return rc.Flush() == nil
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func writeError(w http.ResponseWriter, status int, msg, code string) {
	writeJSON(w, status, map[string]any{"error": map[string]any{"message": msg, "type": "mock_error", "code": code}})
}
