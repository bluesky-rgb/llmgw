#!/usr/bin/env python3
"""汇总编进 llmgw 程序的第三方依赖的许可证全文，随二进制和镜像一起发布（任务 1.10）。

用法：python3 tools/third_party_licenses.py <构建目录> <输出文件>
     依赖清单来自 cmake/dependencies.json；只用于测试或原型的依赖不包括在内。
     源码目录按 <构建目录>/CMakeCache.txt 中 FetchContent 的设置确定。找不到许可证文件时失败。
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NOT_SHIPPED = {"tests", "prototype"}
LICENSE_FILE = re.compile(r"^(LICEN[CS]E|COPYING)([-_.].*)?$", re.IGNORECASE)


def source_dir(cache: dict[str, str], name: str) -> Path:
    """与 FetchContent 的规则相同：FETCHCONTENT_SOURCE_DIR_<名称> 优先，否则是 <FETCHCONTENT_BASE_DIR>/<名称>-src。"""
    override = cache.get(f"FETCHCONTENT_SOURCE_DIR_{name.upper()}", "")
    if override:
        return Path(override)
    return Path(cache.get("FETCHCONTENT_BASE_DIR", "")) / f"{name.lower()}-src"


def read_cache(build_dir: Path) -> dict[str, str]:
    cache = {}
    for line in (build_dir / "CMakeCache.txt").read_text().splitlines():
        m = re.match(r"^([A-Za-z0-9_]+):[A-Z]+=(.*)$", line)
        if m:
            cache[m.group(1)] = m.group(2)
    return cache


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    build_dir, out = Path(sys.argv[1]), Path(sys.argv[2])
    deps = json.loads((ROOT / "cmake/dependencies.json").read_text())["dependencies"]
    cache = read_cache(build_dir)
    count = 0
    parts = ["llmgw 包含以下第三方软件。/ llmgw includes the following third-party software.\n"]
    for name, info in sorted(deps.items()):
        if info["used_by"] in NOT_SHIPPED:
            continue
        src = source_dir(cache, name)
        if not src.is_dir():
            print(f"{name}：源码目录 {src} 不存在", file=sys.stderr)
            return 1
        files = sorted(p for p in src.iterdir() if p.is_file() and LICENSE_FILE.match(p.name))
        if not files:
            print(f"{name}：{src} 中没有许可证文件", file=sys.stderr)
            return 1
        version = info.get("tag", "")
        for f in files:
            parts.append(f"{'=' * 78}\n{name} {version} ({info['license']}) — {f.name}\n{info['repo']}\n{'=' * 78}\n")
            parts.append(f.read_text(errors="replace").rstrip() + "\n")
            count += 1
    out.write_text("\n".join(parts))
    print(f"写入 {out}：{count} 个许可证文件")
    return 0


if __name__ == "__main__":
    sys.exit(main())
