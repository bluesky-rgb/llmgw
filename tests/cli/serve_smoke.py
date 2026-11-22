#!/usr/bin/env python3
"""llmgw serve 的进程级冒烟测试（任务 1.7 X19）。只用 Python 标准库。

启动真实的 llmgw 进程：配置中第一个目标连不上、第二个目标是本脚本里的假上游。检查：
- 进程启动、输出实际的端口；/health 可用
- 一次请求：切换到第二个目标并成功，响应头中有 X-LLMGW-Provider；上游收到的密钥来自环境变量
- 未配置的模型：404
- SIGTERM：5 秒内退出，退出码 0；密钥从未出现在输出中
- 配置错误：退出码 1

用法：serve_smoke.py <llmgw 可执行文件>
"""
import http.client
import http.server
import json
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

SECRET = "sk-SMOKE-SECRET-123"


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class FakeUpstream(http.server.BaseHTTPRequestHandler):
    seen = []

    def do_POST(self):  # noqa: N802
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        req = json.loads(body)
        FakeUpstream.seen.append({"path": self.path, "auth": self.headers.get("Authorization"), "body": req})
        if req.get("stream"):
            events = [{"choices": [{"delta": {"content": "he"}}]}, {"choices": [{"delta": {"content": "llo"}}]}]
            if req.get("stream_options", {}).get("include_usage"):
                events.append({"choices": [], "usage": {"prompt_tokens": 3, "completion_tokens": 2}})
            sse = "".join("data: " + json.dumps(e) + "\n\n" for e in events) + "data: [DONE]\n\n"
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Length", str(len(sse.encode())))
            self.end_headers()
            self.wfile.write(sse.encode())
            return
        out = json.dumps({"id": "x", "choices": [{"message": {"content": "hi"}}],
                          "usage": {"prompt_tokens": 3, "completion_tokens": 1}}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)

    def log_message(self, *args):
        pass


def fail(msg: str, proc=None) -> None:
    print("失败：" + msg)
    if proc is not None and proc.poll() is None:
        proc.kill()
    sys.exit(1)


def request(port: int, method: str, path: str, body=None, headers=None):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    conn.request(method, path, body=json.dumps(body) if body is not None else None,
                 headers={"Content-Type": "application/json", **(headers or {})})
    r = conn.getresponse()
    data = r.read()
    conn.close()
    return r.status, dict(r.getheaders()), data


def main() -> None:
    exe = sys.argv[1]
    upstream = http.server.ThreadingHTTPServer(("127.0.0.1", 0), FakeUpstream)
    threading.Thread(target=upstream.serve_forever, daemon=True).start()
    up_port = upstream.server_address[1]

    config = {
        # 配置不允许端口 0：先找空闲端口（与其他进程抢端口的窗口很小，可以接受）
        "server": {"host": "127.0.0.1", "port": free_port(), "workers": 2,
                   "admin_listen": f"127.0.0.1:{free_port()}",
                   "observability_listen": f"127.0.0.1:{free_port()}"},
        "providers": [
            {"name": "down", "base_url": f"http://127.0.0.1:{free_port()}/v1", "api_key_env": "SMOKE_KEY"},
            {"name": "fake", "base_url": f"http://127.0.0.1:{up_port}/v1", "api_key_env": "SMOKE_KEY"},
        ],
        "models": [{"name": "smart", "targets": [{"provider": "down", "model": "m1"},
                                                 {"provider": "fake", "model": "m2"}]}],
    }
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "llmgw.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump(config, f)
        env = dict(os.environ, SMOKE_KEY=SECRET)
        proc = subprocess.Popen([exe, "serve", "-c", path], env=env, stderr=subprocess.PIPE, text=True)
        line = proc.stderr.readline()
        m = re.search(r"数据面 [^:]+:(\d+)，管理面 [^:]+:(\d+)，观测面 [^:]+:(\d+)", line)
        if not m:
            fail("没有读到启动信息：" + line, proc)
        data_port, _, obs_port = (int(x) for x in m.groups())

        status, _, _ = request(obs_port, "GET", "/health")
        if status != 200:
            fail(f"/health 返回 {status}", proc)

        status, headers, body = request(data_port, "POST", "/v1/chat/completions",
                                        {"model": "smart", "messages": [{"role": "user", "content": "hi"}]})
        if status != 200:
            fail(f"chat 返回 {status}：{body!r}", proc)
        if headers.get("X-LLMGW-Provider") != "fake" or headers.get("X-LLMGW-Attempts") != "2":
            fail(f"响应头不对：{headers}", proc)
        if json.loads(body)["choices"][0]["message"]["content"] != "hi":
            fail(f"响应体不对：{body!r}", proc)
        seen = FakeUpstream.seen
        if len(seen) != 1 or seen[0]["path"] != "/v1/chat/completions" or seen[0]["auth"] != "Bearer " + SECRET \
                or seen[0]["body"]["model"] != "m2":
            fail(f"上游收到的请求不对：{seen}", proc)

        # 流式：网关替客户端加上了 include_usage，上游的 usage 块不能转发给客户端
        status, headers, body = request(data_port, "POST", "/v1/chat/completions",
                                        {"model": "smart", "stream": True, "messages": []})
        text = body.decode()
        if status != 200 or "llo" not in text or "[DONE]" not in text or "usage" in text:
            fail(f"流式响应不对：{status} {text!r}", proc)
        if not seen[-1]["body"].get("stream_options", {}).get("include_usage"):
            fail("网关没有加上 include_usage", proc)

        status, _, body = request(data_port, "POST", "/v1/chat/completions", {"model": "nope", "messages": []})
        if status != 404 or b"model_not_found" not in body:
            fail(f"未配置的模型应该是 404：{status} {body!r}", proc)

        t0 = time.monotonic()
        proc.send_signal(signal.SIGTERM)
        try:
            _, err = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            fail("SIGTERM 之后 5 秒内没有退出", proc)
        if proc.returncode != 0:
            fail(f"退出码 {proc.returncode}：{err}")
        if SECRET in line + err:
            fail("输出中出现了密钥")
        print(f"通过：切换后成功，SIGTERM 后 {time.monotonic() - t0:.2f} 秒退出")

        # 任务 1.9：没有鉴权（2.2 之前）时，数据面监听非本机地址 → 拒绝启动，除非显式确认
        config["server"]["host"] = "0.0.0.0"
        port = config["server"]["port"] = free_port()
        with open(path, "w", encoding="utf-8") as f:
            json.dump(config, f)
        r = subprocess.run([exe, "serve", "-c", path], env=env, capture_output=True, text=True, timeout=10)
        if r.returncode != 1 or "--allow-unauthenticated" not in r.stderr:
            fail(f"监听 0.0.0.0 且没有 --allow-unauthenticated 时应该拒绝启动：{r.returncode} {r.stderr}")
        with socket.socket() as s:  # 拒绝启动：端口不能有任何时刻在监听（检查发生在绑定之前）
            s.settimeout(1)
            if s.connect_ex(("127.0.0.1", port)) == 0:
                fail("拒绝启动之后端口仍在监听")
        proc = subprocess.Popen([exe, "serve", "-c", path, "--allow-unauthenticated"], env=env,
                                stderr=subprocess.PIPE, text=True)
        first = proc.stderr.readline()
        started = proc.stderr.readline() if "没有客户端鉴权" in first else first
        if "没有客户端鉴权" not in first or "已启动" not in started:
            fail(f"加了 --allow-unauthenticated 应该启动并打印警告：{first!r} {started!r}", proc)
        proc.send_signal(signal.SIGTERM)
        proc.communicate(timeout=5)
        if proc.returncode != 0:
            fail(f"退出码 {proc.returncode}")
        config["server"]["host"] = "127.0.0.1"

        # 任务 2.2：keygen 生成 key 和哈希；启用 auth 之后监听 0.0.0.0 不需要 --allow-unauthenticated；
        # 没有 key 是 401、有 key 是 200；key 不出现在任何输出中
        kg = subprocess.run([exe, "keygen", "--id", "smoke"], capture_output=True, text=True, timeout=10)
        gateway_key = kg.stdout.strip()
        hm = re.search(r'"hash": "(sha256:[0-9a-f]{64})"', kg.stderr)
        if kg.returncode != 0 or not gateway_key.startswith("sk-gw-") or not hm or gateway_key in kg.stderr:
            fail(f"keygen 的输出不对：{kg.returncode} {kg.stdout!r} {kg.stderr!r}")
        auth_config = json.loads(json.dumps(config))
        auth_config["server"]["host"] = "0.0.0.0"
        auth_config["server"]["port"] = free_port()
        auth_config["pipeline"] = ["auth"]
        auth_config["keys"] = [{"id": "smoke", "hash": hm.group(1)}]
        with open(path, "w", encoding="utf-8") as f:
            json.dump(auth_config, f)
        proc = subprocess.Popen([exe, "serve", "-c", path], env=env, stderr=subprocess.PIPE, text=True)
        line = proc.stderr.readline()
        m = re.search(r"数据面 [^:]+:(\d+)", line)
        if not m:
            fail("启用 auth 后监听 0.0.0.0 应该直接启动：" + line, proc)
        auth_port = int(m.group(1))
        chat = {"model": "smart", "messages": []}
        status, headers, _ = request(auth_port, "POST", "/v1/chat/completions", chat)
        if status != 401 or headers.get("WWW-Authenticate") != "Bearer":
            fail(f"没有 key 应该是 401：{status} {headers}", proc)
        status, _, _ = request(auth_port, "POST", "/v1/chat/completions", chat,
                               {"Authorization": "Bearer sk-gw-wrong"})
        if status != 401:
            fail(f"错的 key 应该是 401：{status}", proc)
        status, _, body = request(auth_port, "POST", "/v1/chat/completions", chat,
                                  {"Authorization": "Bearer " + gateway_key})
        if status != 200:
            fail(f"有效的 key 应该是 200：{status} {body!r}", proc)
        if seen[-1]["auth"] != "Bearer " + SECRET:
            fail("上游收到的不是网关配置的上游 key", proc)
        proc.send_signal(signal.SIGTERM)
        _, err = proc.communicate(timeout=5)
        if gateway_key in line + err or SECRET in line + err:
            fail("输出中出现了 key")
        print("通过：auth（401 / 200，监听 0.0.0.0 不需要 --allow-unauthenticated）")

        # 配置错误：退出码 1，不启动
        config["models"][0]["targets"][0]["provider"] = "missing"
        with open(path, "w", encoding="utf-8") as f:
            json.dump(config, f)
        r = subprocess.run([exe, "serve", "-c", path], env=env, capture_output=True, text=True, timeout=10)
        if r.returncode != 1:
            fail(f"配置错误时退出码应该是 1，实际 {r.returncode}：{r.stderr}")
        if SECRET in r.stdout + r.stderr:
            fail("输出中出现了密钥")
    upstream.shutdown()


if __name__ == "__main__":
    main()
