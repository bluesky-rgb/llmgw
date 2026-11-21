#!/usr/bin/env python3
"""按提交哈希查询 OSV 漏洞数据库，检查第三方依赖是否有已知漏洞。

依赖来自 cmake/dependencies.json（唯一来源）。对每个依赖：
  1. 用 git ls-remote 把版本标签解析成提交哈希
  2. 用提交哈希查询 https://api.osv.dev/v1/query
  3. 发现漏洞时，除非在 tools/vuln_allowlist.json 中登记且未过期，否则失败

局限：OSV 对 C/C++ 库的收录不完整，发布前仍需人工检查各依赖的安全公告。
用法：python3 tools/check_vulns.py [仓库根目录]
"""
import datetime as dt
import json
import subprocess
import sys
import urllib.request
from pathlib import Path

OSV_URL = "https://api.osv.dev/v1/query"


def resolve_commit(repo, tag):
    out = subprocess.run(["git", "ls-remote", repo, f"refs/tags/{tag}", f"refs/tags/{tag}^{{}}"],
                         capture_output=True, text=True, check=True, timeout=60).stdout
    refs = dict(line.split("\t")[::-1] for line in out.strip().splitlines())
    # 附注标签要取 ^{} 指向的真实提交
    return refs.get(f"refs/tags/{tag}^{{}}") or refs.get(f"refs/tags/{tag}")


def query_osv(commit):
    req = urllib.request.Request(OSV_URL, data=json.dumps({"commit": commit}).encode(),
                                 headers={"Content-Type": "application/json"})
    for attempt in range(3):
        try:
            with urllib.request.urlopen(req, timeout=30) as resp:
                return json.load(resp).get("vulns", [])
        except OSError:
            if attempt == 2:
                raise
    return []


def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    deps = json.loads((root / "cmake" / "dependencies.json").read_text())["dependencies"]
    allow_path = root / "tools" / "vuln_allowlist.json"
    allow = json.loads(allow_path.read_text()).get("allow", []) if allow_path.exists() else []
    today = dt.date.today()
    allowed = {a["id"]: a for a in allow if dt.date.fromisoformat(a["expires"]) >= today}

    failures = 0
    for name, d in deps.items():
        commit = resolve_commit(d["repo"], d["tag"])
        if not commit:
            print(f"✗ {name} {d['tag']}: 无法解析版本标签")
            failures += 1
            continue
        vulns = query_osv(commit)
        blocking = [v for v in vulns if v["id"] not in allowed and
                    not any(a in allowed for a in v.get("aliases", []))]
        status = "✓" if not blocking else "✗"
        print(f"{status} {name} {d['tag']} ({commit[:12]}): {len(vulns)} 个已知漏洞，"
              f"{len(vulns) - len(blocking)} 个已登记豁免")
        for v in blocking:
            print(f"    {v['id']} {v.get('summary', '')}  https://osv.dev/vulnerability/{v['id']}")
        failures += len(blocking)
    print(f"共 {len(deps)} 个依赖，{failures} 个未处理的问题")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
