#!/usr/bin/env python3
"""用真实 key 探测三家上游的行为（任务 1.8，providers.md 第 5 节的验证清单）。只用 Python 标准库。

用法（在有 key 的电脑上）：
    export OPENAI_API_KEY=sk-...  MINIMAX_API_KEY=...  MIMO_API_KEY=tp-...
    python3 tools/provider-probe/probe.py --gateway build/src/app/llmgw

没有设置 key 的上游会被跳过。结果写到 probe-out-<时间>/：report.md、report.json、samples/。
写任何文件之前都检查内容中没有任何一个 key；出现就中止。

可选的环境变量：OPENAI_MODEL、MINIMAX_MODEL、MIMO_MODEL、MIMO_BASE_URL（以控制台显示的为准）；
代理用 HTTPS_PROXY / NO_PROXY（直连部分和网关部分都会使用）。
"""
from __future__ import annotations

import argparse
import concurrent.futures
import datetime
import http.client
import json
import os
import re
import select
import shutil
import signal
import socket
import ssl
import statistics
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass, field

INVALID_KEY = "sk-llmgw-probe-invalid"
PROMPT = "只回答一个字：好"
TINY = 16       # 普通请求的 max_completion_tokens
ROOMY = 256     # 需要看到推理 token 的请求


@dataclass
class Provider:
    name: str
    key_env: str
    base_url: str
    model: str
    key: str = ""
    style: str = "openai"  # 只影响网关的临时配置（MiniMax 加上 error_mapping）


def default_providers() -> list[Provider]:
    env = os.environ.get
    return [
        Provider("openai", "OPENAI_API_KEY", env("OPENAI_BASE_URL", "https://api.openai.com/v1"),
                 env("OPENAI_MODEL", "gpt-5.4-mini")),
        Provider("minimax", "MINIMAX_API_KEY", env("MINIMAX_BASE_URL", "https://api.minimaxi.com/v1"),
                 env("MINIMAX_MODEL", "MiniMax-M3"), style="minimax"),
        Provider("mimo", "MIMO_API_KEY", env("MIMO_BASE_URL", "https://token-plan-cn.xiaomimimo.com/v1"),
                 env("MIMO_MODEL", "mimo-v2.6-pro")),
    ]


# MiniMax 的业务错误码（providers.md 3.4）：网关的临时配置用它，验证 error_mapping 对真实响应有效
MINIMAX_ERROR_MAPPING = {"field": "base_resp.status_code",
                         "codes": {"1002": 429, "1004": 401, "1008": 402, "1001": 504, "1013": 500}}


# ---------------------------------------------------------------- 密钥检查

class SecretLeak(Exception):
    pass


class Secrets:
    """所有写出去的内容都经过这里：出现完整的 key 就中止；出现 key 的前 8 位时替换为 ***。"""

    def __init__(self, keys: list[str]):
        self.keys = [k for k in keys if k]

    def scrub(self, text: str) -> str:
        for k in self.keys:
            if k in text:
                raise SecretLeak("要写出的内容中出现了完整的 key（上游回显了 key？）。已中止，没有写出这个文件")
            if len(k) >= 12:
                text = text.replace(k[:8], "***")
        return text


# ---------------------------------------------------------------- HTTP

@dataclass
class Exchange:
    """一次请求的结果。headers 只保留名字（小写）和值；不保存请求头。"""
    status: int = 0
    headers: dict = field(default_factory=dict)
    body: str = ""
    events: list = field(default_factory=list)   # 流式：每个 data: 的内容（字符串）
    t_head: float | None = None                  # 秒
    t_first_event: float | None = None
    t_first_content: float | None = None         # 第一个带 content 或 reasoning_content 的事件
    elapsed: float = 0.0
    error: str | None = None

    def json(self):
        try:
            return json.loads(self.body)
        except ValueError:
            return None


def chat(p: Provider, body: dict, *, key: str | None = None, timeout: float = 120.0,
         burst: bool = False) -> Exchange:
    url = p.base_url.rstrip("/") + "/chat/completions"
    data = json.dumps(body).encode()
    stream = bool(body.get("stream"))
    req = urllib.request.Request(url, data=data, method="POST", headers={
        "Authorization": "Bearer " + (key if key is not None else p.key),
        "Content-Type": "application/json",
        "Accept": "text/event-stream" if stream else "application/json",
        "User-Agent": "llmgw-provider-probe/1",
        **({"X-Probe-Burst": "1"} if burst else {}),
    })
    ex = Exchange()
    t0 = time.monotonic()
    try:
        resp = urllib.request.urlopen(req, timeout=timeout)  # noqa: S310（地址来自配置）
    except urllib.error.HTTPError as e:
        resp = e
    except Exception as e:  # noqa: BLE001
        ex.error = f"{type(e).__name__}: {e}"
        ex.elapsed = time.monotonic() - t0
        return ex
    ex.t_head = time.monotonic() - t0
    ex.status = resp.status if hasattr(resp, "status") else resp.code
    ex.headers = {k.lower(): v for k, v in resp.headers.items()}
    try:
        if stream and "text/event-stream" in ex.headers.get("content-type", ""):
            raw = []
            for line in resp:
                raw.append(line)
                s = line.decode("utf-8", "replace").rstrip("\r\n")
                if not s.startswith("data:"):
                    continue
                payload = s[5:].lstrip()
                now = time.monotonic() - t0
                if ex.t_first_event is None:
                    ex.t_first_event = now
                ex.events.append(payload)
                if ex.t_first_content is None and has_content(payload):
                    ex.t_first_content = now
            ex.body = b"".join(raw).decode("utf-8", "replace")
        else:
            ex.body = resp.read().decode("utf-8", "replace")
    except Exception as e:  # noqa: BLE001
        ex.error = f"读取响应体时出错：{type(e).__name__}: {e}"
    ex.elapsed = time.monotonic() - t0
    return ex


def has_content(payload: str) -> bool:
    try:
        obj = json.loads(payload)
    except ValueError:
        return False
    for c in obj.get("choices") or []:
        d = c.get("delta") or {}
        if d.get("content") or d.get("reasoning_content"):
            return True
    return False


def parse_events(ex: Exchange) -> list:
    out = []
    for e in ex.events:
        if e == "[DONE]":
            out.append("[DONE]")
            continue
        try:
            out.append(json.loads(e))
        except ValueError:
            out.append({"_unparsed": e[:200]})
    return out


def usage_of(events: list):
    """(usage 对象, 它是不是最后一个 JSON 事件, 所在事件的 choices 是否为空)"""
    objs = [e for e in events if isinstance(e, dict)]
    for i, e in enumerate(objs):
        if e.get("usage"):
            return e["usage"], i == len(objs) - 1, not e.get("choices")
    return None, False, False


def text_of(ex: Exchange) -> tuple[str, str]:
    """(content, reasoning_content)"""
    if ex.events:
        content, reasoning = [], []
        for e in parse_events(ex):
            if isinstance(e, dict):
                for c in e.get("choices") or []:
                    d = c.get("delta") or {}
                    content.append(d.get("content") or "")
                    reasoning.append(d.get("reasoning_content") or "")
        return "".join(content), "".join(reasoning)
    j = ex.json() or {}
    msg = ((j.get("choices") or [{}])[0] or {}).get("message") or {}
    return msg.get("content") or "", msg.get("reasoning_content") or ""


def quota_headers(ex: Exchange) -> dict:
    pat = re.compile(r"ratelimit|quota|remaining|limit", re.I)
    return {k: v for k, v in ex.headers.items() if pat.search(k)}


def summary(ex: Exchange) -> dict:
    content, reasoning = text_of(ex)
    j = ex.json() if not ex.events else None
    return {
        "status": ex.status, "error": ex.error,
        "head_s": round(ex.t_head, 3) if ex.t_head is not None else None,
        "elapsed_s": round(ex.elapsed, 3),
        "content_chars": len(content), "reasoning_chars": len(reasoning),
        "base_resp": (j or {}).get("base_resp") if isinstance(j, dict) else None,
        "error_body": ex.body[:500] if ex.status >= 400 or (j or {}).get("base_resp", {}).get("status_code") else None,
    }


# ---------------------------------------------------------------- 各项探测

def base(p: Provider, **extra) -> dict:
    b = {"model": p.model, "messages": [{"role": "user", "content": PROMPT}], "max_completion_tokens": TINY}
    b.update(extra)
    return b


def probe_provider(p: Provider, args, samples: dict) -> dict:
    r: dict = {}

    def rec(name: str, ex: Exchange):
        samples[f"{p.name}/{name}"] = ex
        return ex

    # 1 基本调用
    ns = rec("basic-nonstream", chat(p, base(p)))
    st = rec("basic-stream", chat(p, base(p, stream=True)))
    r["1_basic"] = {"nonstream": summary(ns), "stream": summary(st) | {"events": len(st.events),
                                                                        "done": "[DONE]" in st.events}}
    # 2、3 usage 与推理 token
    with_usage = rec("stream-include-usage", chat(p, base(p, stream=True, max_completion_tokens=ROOMY,
                                                         stream_options={"include_usage": True})))
    u, last, empty_choices = usage_of(parse_events(with_usage))
    nu, _, _ = usage_of(parse_events(st))
    r["2_include_usage"] = {"status": with_usage.status, "usage_returned": u is not None,
                            "usage_is_last_event": last, "usage_chunk_has_empty_choices": empty_choices,
                            "usage_without_include_usage": nu is not None, "usage": u}
    _, reasoning = text_of(with_usage)
    details = (u or {}).get("completion_tokens_details") or {}
    nsj = ns.json() or {}
    r["3_reasoning"] = {"reasoning_tokens_in_stream_usage": details.get("reasoning_tokens"),
                        "reasoning_tokens_in_nonstream_usage": ((nsj.get("usage") or {}).get(
                            "completion_tokens_details") or {}).get("reasoning_tokens"),
                        "reasoning_content_chars": len(reasoning)}
    # 4 错误的 key
    bad_ns = rec("invalid-key-nonstream", chat(p, base(p), key=INVALID_KEY))
    bad_st = rec("invalid-key-stream", chat(p, base(p, stream=True), key=INVALID_KEY))
    r["4_invalid_key"] = {"nonstream": summary(bad_ns), "stream": summary(bad_st)
                          | {"content_type": bad_st.headers.get("content-type")},
                          "http_200_on_error": bad_ns.status == 200}
    # 6 其他厂商的专有字段
    history = [{"role": "user", "content": "1+1=?"},
               {"role": "assistant", "content": "2", "reasoning_content": "1 加 1 等于 2"},
               {"role": "user", "content": PROMPT}]
    r["6_foreign_fields"] = {}
    for name, body in [("reasoning_content", base(p, messages=history)),
                       ("thinking", base(p, thinking={"type": "disabled"})),
                       ("reasoning_split", base(p, reasoning_split=True))]:
        r["6_foreign_fields"][name] = summary(rec(f"foreign-{name}", chat(p, body)))
    # 7 max_tokens / max_completion_tokens
    mt = base(p)
    del mt["max_completion_tokens"]
    mt["max_tokens"] = TINY
    r["7_max_tokens"] = {}
    for name, body in [("max_tokens", mt), ("max_completion_tokens", base(p))]:
        ex = rec(f"limit-{name}", chat(p, body))
        out = ((ex.json() or {}).get("usage") or {}).get("completion_tokens")
        r["7_max_tokens"][name] = summary(ex) | {"completion_tokens": out}
    # 8 首字延迟（默认推理强度）
    firsts, contents = [], []
    for _ in range(args.ttft_runs):
        ex = chat(p, base(p, stream=True, max_completion_tokens=ROOMY))
        if ex.t_first_event is not None:
            firsts.append(ex.t_first_event)
        if ex.t_first_content is not None:
            contents.append(ex.t_first_content)
    r["8_ttft"] = {"runs": args.ttft_runs,
                   "first_event_median_s": round(statistics.median(firsts), 3) if firsts else None,
                   "first_content_median_s": round(statistics.median(contents), 3) if contents else None,
                   "first_content_max_s": round(max(contents), 3) if contents else None}
    # 11 配额相关的响应头
    r["11_quota_headers"] = quota_headers(ns) or quota_headers(st) or {}
    # 5 触发限流（默认不做）
    if args.burst > 0:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.burst) as pool:
            exs = list(pool.map(lambda _: chat(p, base(p), burst=True), range(args.burst)))
        limited = [e for e in exs if e.status == 429 or (e.json() or {}).get("base_resp", {}).get(
            "status_code") == 1002]
        r["5_rate_limit"] = {"burst": args.burst, "statuses": [e.status for e in exs],
                             "errors": sorted({e.error for e in exs if e.error}),
                             "limited": len(limited),
                             "retry_after": limited[0].headers.get("retry-after") if limited else None,
                             "body": limited[0].body[:500] if limited else None}
        if limited:
            samples[f"{p.name}/rate-limited"] = limited[0]
    else:
        r["5_rate_limit"] = {"skipped": "默认不做；需要时加 --burst N"}
    return r


def idle_close_time(p: Provider, max_s: float, step: float) -> dict:
    """一次请求（GET /models，不消耗额度）之后保持连接，每 step 秒检查服务端是否关闭了连接。"""
    u = urllib.parse.urlsplit(p.base_url)
    port = u.port or (443 if u.scheme == "https" else 80)
    proxy = urllib.request.getproxies().get(u.scheme)
    if proxy and urllib.request.proxy_bypass(u.hostname):
        proxy = None
    try:
        if proxy:
            pu = urllib.parse.urlsplit(proxy)
            conn_cls = http.client.HTTPSConnection if u.scheme == "https" else http.client.HTTPConnection
            conn = conn_cls(pu.hostname, pu.port or 8080, timeout=30)
            headers = {}
            if pu.username:
                import base64
                cred = f"{urllib.parse.unquote(pu.username)}:{urllib.parse.unquote(pu.password or '')}"
                headers["Proxy-Authorization"] = "Basic " + base64.b64encode(cred.encode()).decode()
            conn.set_tunnel(u.hostname, port, headers=headers)
        elif u.scheme == "https":
            conn = http.client.HTTPSConnection(u.hostname, port, timeout=30, context=ssl.create_default_context())
        else:
            conn = http.client.HTTPConnection(u.hostname, port, timeout=30)
        conn.request("GET", u.path.rstrip("/") + "/models",
                     headers={"Authorization": "Bearer " + p.key, "Connection": "keep-alive"})
        resp = conn.getresponse()
        resp.read()
        if (resp.getheader("connection") or "").lower() == "close":
            return {"closed_after_s": 0, "note": "服务端在响应中要求关闭连接"}
        sock = conn.sock
        start = time.monotonic()
        while time.monotonic() - start < max_s:
            time.sleep(step)
            readable, _, _ = select.select([sock], [], [], 0)
            if readable:
                try:
                    peek = sock.recv(1, socket.MSG_PEEK) if not isinstance(sock, ssl.SSLSocket) else sock.recv(1)
                except (ssl.SSLError, OSError):
                    peek = b""
                if peek == b"":
                    return {"closed_after_s": round(time.monotonic() - start, 1), "via_proxy": bool(proxy)}
        conn.close()
        return {"closed_after_s": None, "note": f"超过 {max_s:.0f} 秒仍未关闭", "via_proxy": bool(proxy)}
    except Exception as e:  # noqa: BLE001
        return {"error": f"{type(e).__name__}: {e}"}


# ---------------------------------------------------------------- 经过网关

def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def gateway_checks(exe: str, providers: list[Provider], self_test: bool) -> dict:
    """生成临时配置（key 只写环境变量的名字），启动 llmgw serve，经过网关再测一遍。"""
    cfg = {
        "server": {"host": "127.0.0.1", "port": free_port(), "workers": 2,
                   "admin_listen": f"127.0.0.1:{free_port()}",
                   "observability_listen": f"127.0.0.1:{free_port()}"},
        "providers": [], "models": [],
    }
    if not self_test and (os.environ.get("HTTPS_PROXY") or os.environ.get("https_proxy")):
        cfg["proxy"] = {"https": "env"}  # 代理地址（可能带密码）留在环境变量中，不写进文件
    env = dict(os.environ, LLMGW_PROBE_INVALID_KEY=INVALID_KEY)
    for p in providers:
        for prefix, key_env in (("", p.key_env), ("bad-", "LLMGW_PROBE_INVALID_KEY")):
            entry = {"name": prefix + p.name, "base_url": p.base_url, "api_key_env": key_env}
            if p.style == "minimax":
                entry["error_mapping"] = MINIMAX_ERROR_MAPPING
            cfg["providers"].append(entry)
        cfg["models"].append({"name": f"probe-{p.name}", "targets": [{"provider": p.name, "model": p.model}]})
        cfg["models"].append({"name": f"probe-failover-{p.name}",
                              "targets": [{"provider": "bad-" + p.name, "model": p.model},
                                          {"provider": p.name, "model": p.model}]})
    out: dict = {}
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "llmgw.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump(cfg, f)
        proc = subprocess.Popen([exe, "serve", "-c", path], env=env, stderr=subprocess.PIPE, text=True)
        try:
            line = proc.stderr.readline()
            m = re.search(r"数据面 [^:]+:(\d+)", line)
            if not m:
                return {"error": "网关没有启动：" + line.strip()}
            gw = Provider("gateway", "", f"http://127.0.0.1:{m.group(1)}/v1", "")
            gw.key = "unused"
            for p in providers:
                res: dict = {}
                gw.model = f"probe-{p.name}"
                ns = chat(gw, base(gw))
                res["nonstream"] = {"status": ns.status, "provider": ns.headers.get("x-llmgw-provider"),
                                    "ok": ns.status == 200 and bool(text_of(ns)[0] or text_of(ns)[1])}
                st = chat(gw, base(gw, stream=True))
                res["stream_without_include_usage"] = {
                    "status": st.status, "done": "[DONE]" in st.events,
                    "usage_chunk_hidden": usage_of(parse_events(st))[0] is None}
                su = chat(gw, base(gw, stream=True, stream_options={"include_usage": True}))
                res["stream_with_include_usage"] = {"status": su.status,
                                                    "usage_kept": usage_of(parse_events(su))[0] is not None}
                gw.model = f"probe-failover-{p.name}"
                fo = chat(gw, base(gw))
                res["failover_after_invalid_key"] = {"status": fo.status,
                                                     "attempts": fo.headers.get("x-llmgw-attempts"),
                                                     "provider": fo.headers.get("x-llmgw-provider"),
                                                     "ok": fo.status == 200 and fo.headers.get("x-llmgw-attempts") == "2"}
                out[p.name] = res
        finally:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
            out["_exit_code"] = proc.returncode
    return out


# ---------------------------------------------------------------- 输出

def sanitize_body(text: str) -> str:
    """样本中去掉每次都不同、或者能关联到账号的值（id、时间、指纹、请求 ID）"""
    text = re.sub(r'"(id|request_id|system_fingerprint|trace_id)"\s*:\s*"[^"]*"', r'"\1":"REDACTED"', text)
    return re.sub(r'"created"\s*:\s*\d+', '"created":0', text)


def write(path: str, text: str, secrets: Secrets) -> None:
    clean = secrets.scrub(text)  # 先检查再打开文件：泄漏时不留下文件
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(clean)


def report_md(results: dict) -> str:
    lines = ["# 上游探测报告", "", f"时间：{results['time']}　工具版本：1", ""]
    for name, r in results["providers"].items():
        lines += [f"## {name}", ""]
        if "skipped" in r:
            lines += [r["skipped"], ""]
            continue
        lines += ["```json", json.dumps(r, ensure_ascii=False, indent=2), "```", ""]
    lines += ["## 经过网关", "", "```json", json.dumps(results.get("gateway"), ensure_ascii=False, indent=2), "```", "",
              "## 第 12 项：账号的实际配额（请在各家控制台查看后填写）", "",
              "| | OpenAI | MiniMax | MiMo |", "|---|---|---|---|",
              "| RPM | | | |", "| TPM | | | |", "| 套餐额度 | | | |", ""]
    return "\n".join(lines)


def run(args) -> int:
    providers = default_providers() if not args.self_test else args.self_test_providers
    for p in providers:
        p.key = p.key or os.environ.get(p.key_env, "")
    secrets = Secrets([p.key for p in providers])
    active = [p for p in providers if p.key and (not args.only or p.name in args.only)]
    results = {"time": datetime.datetime.now().isoformat(timespec="seconds"), "providers": {}}
    for p in providers:
        if p not in active:
            results["providers"][p.name] = {"skipped": f"没有设置 {p.key_env}，跳过"}
    samples: dict = {}
    print(f"探测：{', '.join(p.name for p in active) or '（没有设置任何 key）'}", flush=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, len(active))) as pool:
        idle = {p.name: pool.submit(idle_close_time, p, args.idle_max, args.idle_step) for p in active}
        try:
            for p in active:
                print(f"  {p.name}（{p.model}）……", flush=True)
                results["providers"][p.name] = probe_provider(p, args, samples)
            for p in active:
                print(f"  {p.name}：等待空闲连接的结果（最多 {args.idle_max:.0f} 秒）……", flush=True)
                results["providers"][p.name]["9_idle_close"] = idle[p.name].result()
        except KeyboardInterrupt:
            print("已中断：写出已完成的部分", flush=True)
    if args.gateway and active:
        print("  经过网关……", flush=True)
        results["gateway"] = gateway_checks(args.gateway, active, args.self_test is not None and args.self_test)
    out = args.out or f"probe-out-{datetime.datetime.now():%Y%m%d-%H%M%S}"
    try:
        write(os.path.join(out, "report.json"), json.dumps(results, ensure_ascii=False, indent=2), secrets)
        write(os.path.join(out, "report.md"), report_md(results), secrets)
        index = {}
        for name, ex in samples.items():
            if ex.status == 0:  # 没有拿到响应（网络错误）：没有样本
                continue
            ext = "sse" if ex.events else "json"
            write(os.path.join(out, "samples", f"{name}.{ext}"), sanitize_body(ex.body), secrets)
            index[f"{name}.{ext}"] = {"status": ex.status, "content_type": ex.headers.get("content-type", "")}
        # 回归测试（tests/unit/stream/provider_samples_test.cpp）按它构造响应头
        write(os.path.join(out, "samples", "index.json"), json.dumps(index, ensure_ascii=False, indent=2), secrets)
    except SecretLeak as e:
        print("错误：" + str(e), file=sys.stderr)
        shutil.rmtree(out, ignore_errors=True)
        return 3
    print(f"完成：{out}/report.md（把整个 {out} 目录发回来）", flush=True)
    return 0


# ---------------------------------------------------------------- 自检（假上游）

def self_test(args) -> int:
    """在本机启动模拟三家行为的假上游，完整运行一遍，检查报告（任务 1.8 V1–V3）。"""
    import http.server

    keys = {"openai": "sk-selftest-openai-0123456789", "minimax": "mm-selftest-0123456789",
            "mimo": "tp-selftest-mimo-0123456789"}
    leak = {"on": False}

    def make_handler(style: str):
        class H(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"
            timeout = 3  # 空闲 3 秒后关闭连接（第 9 项应该测到 3–5 秒）
            inflight = {"n": 0}
            lock = threading.Lock()

            def log_message(self, *a):
                pass

            def send(self, status, obj=None, raw=None, ctype="application/json", extra=None):
                data = raw if raw is not None else json.dumps(obj).encode()
                self.send_response(status)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(data)))
                for k, v in (extra or {}).items():
                    self.send_header(k, v)
                self.end_headers()
                self.wfile.write(data)

            def do_GET(self):  # noqa: N802
                self.send(200, {"data": []})

            def do_POST(self):  # noqa: N802
                req = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                if self.headers.get("Authorization") != "Bearer " + keys[style]:
                    if style == "minimax":  # MiniMax 风格：HTTP 200 + base_resp（流式时放在第一个事件中）
                        err = {"base_resp": {"status_code": 1004, "status_msg": "login fail"}}
                        if req.get("stream"):
                            return self.send(200, raw=("data: " + json.dumps(err) + "\n\n").encode(),
                                             ctype="text/event-stream")
                        return self.send(200, err)
                    return self.send(401, {"error": {"message": "Incorrect API key", "code": "invalid_api_key"}})
                if style == "openai":
                    if "max_tokens" in req or "thinking" in req or "reasoning_split" in req or any(
                            "reasoning_content" in m for m in req["messages"]):
                        return self.send(400, {"error": {"message": "Unrecognized request argument"}})
                    # 同时处理的请求超过 5 个就限流：顺序的请求永远不会触发，第 5 项的并发请求一定会触发
                    with H.lock:
                        H.inflight["n"] += 1
                        busy = H.inflight["n"] > 5
                    try:
                        if busy:
                            return self.send(429, {"error": {"message": "Rate limit"}}, extra={"Retry-After": "2"})
                        time.sleep(0.2 if self.headers.get("X-Probe-Burst") else 0)
                    finally:
                        with H.lock:
                            H.inflight["n"] -= 1
                usage = {"prompt_tokens": 5, "completion_tokens": 3,
                         "completion_tokens_details": {"reasoning_tokens": 2 if style != "openai" else 0}}
                reasoning = "想一想" if style != "openai" else ""
                if req.get("stream"):
                    events = [{"id": "c1", "created": 1, "choices": [{"delta": {"reasoning_content": reasoning}}]},
                              {"id": "c1", "created": 1, "choices": [{"delta": {"content": "好"}}]}]
                    if (req.get("stream_options") or {}).get("include_usage") or style == "minimax":
                        events.append({"id": "c1", "choices": [], "usage": usage})
                    data = "".join("data: " + json.dumps(e, ensure_ascii=False) + "\n\n" for e in events)
                    return self.send(200, raw=(data + "data: [DONE]\n\n").encode(), ctype="text/event-stream")
                body = {"id": "c1", "created": 1, "usage": usage,
                        "choices": [{"message": {"content": "好", "reasoning_content": reasoning}}]}
                if leak["on"] and style == "mimo":
                    body["debug"] = keys["mimo"]
                extra = {"x-ratelimit-remaining-requests": "99"} if style == "openai" else {}
                return self.send(200, body, extra=extra)
        return H

    servers = {}
    class Server(http.server.ThreadingHTTPServer):
        request_queue_size = 256  # 第 5 项的并发连接不能被拒绝
        daemon_threads = True

    for style in keys:
        s = Server(("127.0.0.1", 0), make_handler(style))
        threading.Thread(target=s.serve_forever, daemon=True).start()
        servers[style] = s
    args.self_test_providers = [
        Provider(n, f"LLMGW_SELFTEST_{n.upper()}_KEY", f"http://127.0.0.1:{servers[n].server_address[1]}/v1",
                 f"{n}-model", style="minimax" if n == "minimax" else "openai") for n in keys]
    for p in args.self_test_providers:
        os.environ[p.key_env] = keys[p.name]
    failures = []

    def check(cond, what):
        if not cond:
            failures.append(what)

    keep = args.out  # --self-test --out DIR：保留第一次运行的输出（验证回归测试时使用）
    with tempfile.TemporaryDirectory() as d:
        args.out = keep or os.path.join(d, "out")
        args.idle_max, args.idle_step, args.ttft_runs, args.burst = 8, 0.5, 2, 45
        rc = run(args)
        check(rc == 0, f"探测返回 {rc}")
        if os.environ.get("LLMGW_PROBE_DEBUG"):
            print(open(os.path.join(args.out, "report.json"), encoding="utf-8").read()[:6000])
        rep = json.load(open(os.path.join(args.out, "report.json"), encoding="utf-8")) if rc == 0 else {}
        P = rep.get("providers", {})
        o, mm, mi = P.get("openai", {}), P.get("minimax", {}), P.get("mimo", {})
        check(o.get("1_basic", {}).get("stream", {}).get("done"), "1：流式没有 [DONE]")
        check(o.get("2_include_usage", {}).get("usage_returned"), "2：没有测到 usage")
        check(not o.get("2_include_usage", {}).get("usage_without_include_usage"), "2：openai 不带时不应有 usage")
        check(mm.get("2_include_usage", {}).get("usage_without_include_usage"), "2：minimax 不带时也有 usage")
        check(mi.get("3_reasoning", {}).get("reasoning_tokens_in_stream_usage") == 2, "3：推理 token")
        check(mi.get("3_reasoning", {}).get("reasoning_content_chars", 0) > 0, "3：reasoning_content")
        check(o.get("4_invalid_key", {}).get("nonstream", {}).get("status") == 401, "4：openai 401")
        check(mm.get("4_invalid_key", {}).get("http_200_on_error") is True, "4：minimax 出错时 HTTP 200")
        check(mm.get("4_invalid_key", {}).get("nonstream", {}).get("base_resp", {}).get("status_code") == 1004,
              "4：base_resp")
        check(o.get("6_foreign_fields", {}).get("reasoning_content", {}).get("status") == 400, "6：openai 400")
        check(mi.get("6_foreign_fields", {}).get("thinking", {}).get("status") == 200, "6：mimo 200")
        check(o.get("7_max_tokens", {}).get("max_tokens", {}).get("status") == 400, "7：max_tokens")
        check(o.get("8_ttft", {}).get("first_content_median_s") is not None, "8：首字延迟")
        idle_s = (o.get("9_idle_close") or {}).get("closed_after_s")
        check(idle_s is not None and 2.5 <= idle_s <= 6, f"9：空闲关闭时间 {idle_s}（假上游 3 秒）")
        check(o.get("11_quota_headers", {}).get("x-ratelimit-remaining-requests") == "99", "11：配额响应头")
        check(o.get("5_rate_limit", {}).get("limited", 0) > 0, "5：没有测到 429")
        check(o.get("5_rate_limit", {}).get("retry_after") == "2", "5：Retry-After")
        samples_dir = os.path.join(args.out, "samples")
        check(os.path.exists(os.path.join(samples_dir, "minimax", "basic-stream.sse")), "10：样本")
        if os.path.exists(os.path.join(samples_dir, "openai", "basic-nonstream.json")):
            check('"id":"REDACTED"' in open(os.path.join(samples_dir, "openai", "basic-nonstream.json")).read(),
                  "10：样本中的 id 没有替换")
        if args.gateway:
            g = rep.get("gateway") or {}
            for n in keys:
                gn = g.get(n, {})
                check(gn.get("nonstream", {}).get("ok"), f"网关 {n}：非流式")
                check(gn.get("stream_without_include_usage", {}).get("usage_chunk_hidden"),
                      f"网关 {n}：客户端没有要求时 usage 块应该被去掉")
                check(gn.get("stream_with_include_usage", {}).get("usage_kept"), f"网关 {n}：要求时保留")
                check(gn.get("failover_after_invalid_key", {}).get("ok"),
                      f"网关 {n}：无效 key 之后切换（minimax 依赖 error_mapping）：{gn.get('failover_after_invalid_key')}")
            check(g.get("_exit_code") == 0, f"网关退出码 {g.get('_exit_code')}")
        # V2：上游回显 key → 中止，不写文件
        print("V2：假上游在响应中回显 key，下面应该报错并中止（这是预期的）", flush=True)
        leak["on"] = True
        args.out = os.path.join(d, "leak")
        args.idle_max, args.burst, args.gateway = 1, 0, None
        rc = run(args)
        check(rc == 3, f"回显 key 时应该中止（返回 3），实际 {rc}")
        check(not os.path.exists(args.out), "回显 key 时不应该留下输出目录")
    for s in servers.values():
        s.shutdown()
    if failures:
        print("自检失败：\n  " + "\n  ".join(failures))
        return 1
    print("自检通过")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gateway", metavar="LLMGW", help="llmgw 可执行文件：再经过网关测一遍")
    ap.add_argument("--only", type=lambda s: s.split(","), help="只测这些上游，例如 openai,mimo")
    ap.add_argument("--burst", type=int, default=0, help="第 5 项：同时发 N 个请求触发限流（默认不做）")
    ap.add_argument("--idle-max", type=float, default=180, help="第 9 项最多等多少秒（默认 180）")
    ap.add_argument("--idle-step", type=float, default=5)
    ap.add_argument("--ttft-runs", type=int, default=5)
    ap.add_argument("--out", help="输出目录（默认 probe-out-<时间>）")
    ap.add_argument("--self-test", action="store_true", help="用本机的假上游完整运行一遍（不需要 key）")
    args = ap.parse_args()
    if args.self_test:
        return self_test(args)
    args.self_test = None
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
