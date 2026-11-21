#include "gateway.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "logger.h"
#include "sse.h"
#include "url.h"

namespace llmgw {

using nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

constexpr size_t kMaxRequestBytes = 32 * 1024 * 1024;  // 允许带 base64 图片的请求

double ms_since(Clock::time_point t) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

std::string make_request_id() {
  thread_local std::mt19937_64 rng{std::random_device{}()};
  static const char* kHex = "0123456789abcdef";
  std::string id = "req_";
  uint64_t v = rng();
  for (int i = 0; i < 16; ++i) {
    id += kHex[v & 0xf];
    v >>= 4;
  }
  return id;
}

// 与 OpenAI 的错误格式保持一致，现有 SDK 能直接识别
std::string error_body(const std::string& message, const std::string& type,
                       const std::string& code) {
  return json{{"error", {{"message", message}, {"type", type}, {"code", code}}}}.dump();
}

void send_error(httplib::Response& res, int status, const std::string& message,
                const std::string& type, const std::string& code) {
  res.status = status;
  res.set_content(error_body(message, type, code), "application/json");
}

// 429 和 5xx 换一家上游可能就成功；其他 4xx 是请求本身的问题，换了也没用
bool is_retryable_status(int status) { return status == 429 || status >= 500; }

std::unique_ptr<httplib::Client> make_client(const ProviderConfig& p, const BaseUrl& url) {
  auto client = std::make_unique<httplib::Client>(url.scheme_host_port);
  client->set_connection_timeout(p.connect_timeout_seconds, 0);
  client->set_read_timeout(p.read_timeout_seconds, 0);
  client->set_write_timeout(30, 0);
  client->enable_server_certificate_verification(true);
  return client;
}

httplib::Headers upstream_headers(const ProviderConfig& p, const std::string& request_id,
                                  bool stream) {
  httplib::Headers h;
  if (!p.api_key.empty()) h.emplace("Authorization", "Bearer " + p.api_key);
  h.emplace("X-Request-Id", request_id);
  if (stream) h.emplace("Accept", "text/event-stream");
  for (const auto& [k, v] : p.extra_headers) h.emplace(k, v);
  return h;
}

std::string upstream_payload(json body, const Target& target) {
  body["model"] = target.model;
  return body.dump();
}

// 流式请求里，上游读取线程和响应客户端的线程之间用这个结构传递数据
struct StreamChannel {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::string> chunks;
  bool headers_ready = false;  // 已经确定了状态码和要用哪个上游
  bool finished = false;       // 上游已经读完（或失败）
  int status = 0;
  std::string content_type;
  std::string provider = "none";
  int attempts = 0;
  TokenUsage usage;
  std::atomic<bool> cancelled{false};  // 客户端断开后通知上游线程停止
  std::thread worker;

  void push(std::string data) {
    {
      std::lock_guard<std::mutex> lock(mu);
      chunks.push_back(std::move(data));
    }
    cv.notify_all();
  }
  void finish() {
    {
      std::lock_guard<std::mutex> lock(mu);
      headers_ready = true;
      finished = true;
    }
    cv.notify_all();
  }
  void join() {
    if (worker.joinable()) worker.join();
  }
};

}  // namespace

struct Gateway::Impl {
  Config cfg;
  httplib::Server server;
  Metrics metrics;
  std::map<std::string, BaseUrl> base_urls;

  explicit Impl(Config c) : cfg(std::move(c)) {
    for (const auto& p : cfg.providers) base_urls[p.name] = parse_base_url(p.base_url);
    register_routes();
  }

  void register_routes() {
    const size_t threads = static_cast<size_t>(cfg.listen.threads);
    server.new_task_queue = [threads] { return new httplib::ThreadPool(threads); };
    server.set_payload_max_length(kMaxRequestBytes);

    server.Get("/health", [](const httplib::Request&, httplib::Response& res) {
      res.set_content(R"({"status":"ok"})", "application/json");
    });

    server.Get("/metrics", [this](const httplib::Request&, httplib::Response& res) {
      res.set_content(metrics.render(), "text/plain; version=0.0.4");
    });

    server.Get("/v1/models", [this](const httplib::Request& req, httplib::Response& res) {
      if (!authorized(req)) {
        send_error(res, 401, "Invalid API key", "authentication_error", "invalid_api_key");
        return;
      }
      json data = json::array();
      for (const auto& m : cfg.models) {
        data.push_back({{"id", m.name}, {"object", "model"}, {"created", 0}, {"owned_by", "llmgw"}});
      }
      res.set_content(json{{"object", "list"}, {"data", data}}.dump(), "application/json");
    });

    server.Post("/v1/chat/completions",
                [this](const httplib::Request& req, httplib::Response& res) {
                  handle_chat(req, res);
                });

    server.set_error_handler([](const httplib::Request&, httplib::Response& res) {
      if (res.body.empty()) {
        res.set_content(error_body("Route not found", "invalid_request_error", "not_found"),
                        "application/json");
      }
    });

    server.set_exception_handler(
        [](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
          std::string what = "unknown";
          try {
            if (ep) std::rethrow_exception(ep);
          } catch (const std::exception& e) {
            what = e.what();
          } catch (...) {
          }
          log_event("error", "unhandled exception", {{"error", what}});
          send_error(res, 500, "Internal gateway error", "server_error", "internal_error");
        });
  }

  bool authorized(const httplib::Request& req) const {
    if (cfg.api_keys.empty()) return true;
    const auto auth = req.get_header_value("Authorization");
    static const std::string kBearer = "Bearer ";
    if (auth.compare(0, kBearer.size(), kBearer) != 0) return false;
    const auto key = auth.substr(kBearer.size());
    for (const auto& k : cfg.api_keys) {
      if (k == key) return true;
    }
    return false;
  }

  void handle_chat(const httplib::Request& req, httplib::Response& res) {
    const auto start = Clock::now();
    std::string request_id = req.get_header_value("X-Request-Id");
    if (request_id.empty() || request_id.size() > 128) request_id = make_request_id();
    res.set_header("X-Request-Id", request_id);

    if (!authorized(req)) {
      send_error(res, 401, "Invalid API key", "authentication_error", "invalid_api_key");
      return;
    }

    auto body = json::parse(req.body, nullptr, /*allow_exceptions=*/false);
    if (body.is_discarded() || !body.is_object()) {
      send_error(res, 400, "Request body must be a JSON object", "invalid_request_error",
                 "invalid_json");
      return;
    }
    if (!body.contains("model") || !body["model"].is_string()) {
      send_error(res, 400, "Missing required field: model", "invalid_request_error",
                 "missing_model");
      return;
    }
    const auto model = body["model"].get<std::string>();
    const ModelRoute* route = cfg.find_model(model);
    if (!route) {
      send_error(res, 404, "Model '" + model + "' is not configured on this gateway",
                 "invalid_request_error", "model_not_found");
      return;
    }

    const bool stream = body.contains("stream") && body["stream"].is_boolean() &&
                        body["stream"].get<bool>();
    if (stream) {
      handle_stream(*route, std::move(body), request_id, start, res);
    } else {
      handle_non_stream(*route, body, request_id, start, res);
    }
  }

  void note_failure(const std::string& request_id, const std::string& provider,
                    const std::string& reason) {
    metrics.record_upstream_failure(provider, reason);
    log_event("warn", "upstream attempt failed, trying next target",
              {{"request_id", request_id}, {"provider", provider}, {"reason", reason}});
  }

  void log_done(const std::string& request_id, const std::string& model,
                const std::string& provider, int status, bool stream, int attempts,
                double latency_ms, const TokenUsage& usage, bool client_aborted = false) {
    metrics.record_request(model, provider, status, stream, latency_ms);
    if (usage.present) {
      metrics.record_tokens(model, provider, usage.prompt_tokens, usage.completion_tokens);
    }
    json f = {{"request_id", request_id}, {"model", model},     {"provider", provider},
              {"status", status},         {"stream", stream},   {"attempts", attempts},
              {"latency_ms", static_cast<int64_t>(latency_ms)}};
    if (usage.present) {
      f["prompt_tokens"] = usage.prompt_tokens;
      f["completion_tokens"] = usage.completion_tokens;
    }
    if (client_aborted) f["client_aborted"] = true;
    log_event(status < 400 ? "info" : "warn", "request done", f);
  }

  // ---- 非流式：依次尝试每个 target，遇到可重试的错误就换下一个 -----------------
  void handle_non_stream(const ModelRoute& route, const json& body, const std::string& request_id,
                         Clock::time_point start, httplib::Response& res) {
    std::string last_error = "no upstream attempted";
    int attempts = 0;
    for (size_t i = 0; i < route.targets.size(); ++i) {
      const auto& target = route.targets[i];
      const ProviderConfig& p = *cfg.find_provider(target.provider);
      const BaseUrl& url = base_urls.at(p.name);
      const bool has_next = i + 1 < route.targets.size();

      auto client = make_client(p, url);
      ++attempts;
      auto r = client->Post(url.path_prefix + "/chat/completions",
                            upstream_headers(p, request_id, false),
                            upstream_payload(body, target), "application/json");
      if (!r) {
        last_error = p.name + ": " + httplib::to_string(r.error());
        note_failure(request_id, p.name, httplib::to_string(r.error()));
        continue;
      }
      if (is_retryable_status(r->status) && has_next) {
        last_error = p.name + ": HTTP " + std::to_string(r->status);
        note_failure(request_id, p.name, "http_" + std::to_string(r->status));
        continue;
      }

      // 最终结果（成功，或不可重试的错误）原样转发给客户端
      res.status = r->status;
      auto ct = r->get_header_value("Content-Type");
      res.set_content(r->body, ct.empty() ? "application/json" : ct);
      res.set_header("X-LLMGW-Provider", p.name);
      TokenUsage usage;
      if (r->status >= 200 && r->status < 300) usage = extract_usage_from_json(r->body);
      log_done(request_id, route.name, p.name, r->status, false, attempts, ms_since(start), usage);
      return;
    }

    send_error(res, 502, "All upstream providers failed. Last error: " + last_error,
               "upstream_error", "upstream_unavailable");
    log_done(request_id, route.name, "none", 502, false, attempts, ms_since(start), {});
  }

  // ---- 流式 --------------------------------------------------------------------
  // 上游在独立线程里读取。拿到上游的状态码以后，这里才决定返回给客户端的状态码：
  // 这样上游返回 400 时客户端收到的也是 400，而不是 200 加一条错误事件；
  // 上游在发出任何数据之前失败，还可以无感知地切换到下一个上游。
  void handle_stream(const ModelRoute& route, json body, const std::string& request_id,
                     Clock::time_point start, httplib::Response& res) {
    auto ch = std::make_shared<StreamChannel>();
    ch->worker = std::thread([this, ch, &route, body = std::move(body), request_id]() {
      run_stream_worker(*ch, route, body, request_id);
    });

    {
      std::unique_lock<std::mutex> lock(ch->mu);
      ch->cv.wait(lock, [&] { return ch->headers_ready; });
    }

    if (ch->status < 200 || ch->status >= 300) {
      // 错误响应体一般很小，读完后一次性返回
      ch->join();
      std::string payload;
      for (auto& c : ch->chunks) payload += c;
      res.status = ch->status;
      res.set_content(payload, ch->content_type.empty() ? "application/json" : ch->content_type);
      log_done(request_id, route.name, ch->provider, ch->status, true, ch->attempts,
               ms_since(start), {});
      return;
    }

    res.status = ch->status;
    res.set_header("Cache-Control", "no-cache");
    res.set_header("X-Accel-Buffering", "no");  // 防止 Nginx 缓冲流式输出
    res.set_header("X-LLMGW-Provider", ch->provider);
    const std::string ct = ch->content_type.empty() ? "text/event-stream" : ch->content_type;

    res.set_chunked_content_provider(
        ct,
        [ch](size_t /*offset*/, httplib::DataSink& sink) {
          std::unique_lock<std::mutex> lock(ch->mu);
          ch->cv.wait(lock, [&] { return !ch->chunks.empty() || ch->finished; });
          while (!ch->chunks.empty()) {
            std::string data = std::move(ch->chunks.front());
            ch->chunks.pop_front();
            lock.unlock();
            if (!sink.write(data.data(), data.size())) {
              ch->cancelled = true;  // 客户端已断开
              return false;
            }
            lock.lock();
          }
          if (ch->finished) sink.done();
          return true;
        },
        [this, ch, route_name = route.name, request_id, start](bool success) {
          ch->cancelled = !success || ch->cancelled.load();
          ch->cv.notify_all();
          ch->join();
          log_done(request_id, route_name, ch->provider, ch->status, true, ch->attempts,
                   ms_since(start), ch->usage, ch->cancelled.load());
        });
  }

  void run_stream_worker(StreamChannel& ch, const ModelRoute& route, const json& body,
                         const std::string& request_id) {
    std::string last_error = "no upstream attempted";

    for (size_t i = 0; i < route.targets.size(); ++i) {
      if (ch.cancelled) break;
      const auto& target = route.targets[i];
      const ProviderConfig& p = *cfg.find_provider(target.provider);
      const BaseUrl& url = base_urls.at(p.name);
      const bool has_next = i + 1 < route.targets.size();

      bool retry_next = false;
      bool started = false;
      SseUsageScanner scanner;

      httplib::Request r;
      r.method = "POST";
      r.path = url.path_prefix + "/chat/completions";
      r.headers = upstream_headers(p, request_id, true);
      r.set_header("Content-Type", "application/json");
      r.body = upstream_payload(body, target);

      r.response_handler = [&](const httplib::Response& resp) {
        if (is_retryable_status(resp.status) && has_next) {
          retry_next = true;
          last_error = p.name + ": HTTP " + std::to_string(resp.status);
          return false;  // 放弃这个上游，换下一个
        }
        {
          std::lock_guard<std::mutex> lock(ch.mu);
          ch.status = resp.status;
          ch.content_type = resp.get_header_value("Content-Type");
          ch.provider = p.name;
          ch.headers_ready = true;
        }
        ch.cv.notify_all();
        started = true;
        return true;
      };
      r.content_receiver = [&](const char* data, size_t len, size_t, size_t) {
        if (ch.cancelled) return false;
        scanner.feed(data, len);
        ch.push(std::string(data, len));
        return true;
      };

      auto client = make_client(p, url);
      {
        std::lock_guard<std::mutex> lock(ch.mu);
        ++ch.attempts;
      }
      auto result = client->send(r);

      if (started) {
        if (!result && result.error() != httplib::Error::Canceled) {
          // 已经开始输出后上游中断：没法再切换，只能告诉客户端出错了
          metrics.record_upstream_failure(p.name, "stream_interrupted");
          ch.push("data: " +
                  error_body("Upstream stream interrupted: " + httplib::to_string(result.error()),
                             "upstream_error", "stream_interrupted") +
                  "\n\n");
        }
        {
          std::lock_guard<std::mutex> lock(ch.mu);
          ch.usage = scanner.usage();
        }
        ch.finish();
        return;
      }

      if (retry_next) {
        note_failure(request_id, p.name, last_error.substr(p.name.size() + 2));
      } else {
        last_error = p.name + ": " + httplib::to_string(result.error());
        note_failure(request_id, p.name, httplib::to_string(result.error()));
      }
    }

    {
      std::lock_guard<std::mutex> lock(ch.mu);
      ch.status = 502;
      ch.content_type = "application/json";
      ch.provider = "none";
      ch.chunks.push_back(error_body("All upstream providers failed. Last error: " + last_error,
                                     "upstream_error", "upstream_unavailable"));
    }
    ch.finish();
  }
};

Gateway::Gateway(Config cfg) : impl_(std::make_unique<Impl>(std::move(cfg))) {}

Gateway::~Gateway() { stop(); }

int Gateway::bind(const std::string& host, int port) {
  if (port == 0) return impl_->server.bind_to_any_port(host);
  return impl_->server.bind_to_port(host, port) ? port : -1;
}

bool Gateway::run() { return impl_->server.listen_after_bind(); }

void Gateway::stop() {
  if (impl_ && impl_->server.is_running()) impl_->server.stop();
}

void Gateway::wait_until_ready() const { impl_->server.wait_until_ready(); }

Metrics& Gateway::metrics() { return impl_->metrics; }

}  // namespace llmgw
