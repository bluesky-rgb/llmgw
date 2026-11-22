#!/usr/bin/env python3
"""汇总编进 llmgw 程序的第三方依赖的许可证全文，随二进制和镜像一起发布（任务 1.10）。

用法：python3 tools/third_party_licenses.py <构建目录> <输出文件>
     编进程序的依赖 = src/ 下的 CMakeLists.txt 中 llmgw_fetch_dependency() 的那些（以构建脚本为准，
     不看依赖清单中的 used_by 说明：说明会过时，nlohmann_json 曾因此被漏掉）。版本、许可证、仓库地址
     来自 cmake/dependencies.json。
     源码目录按 <构建目录>/CMakeCache.txt 中 FetchContent 的设置确定。找不到许可证文件时失败。
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LICENSE_FILE = re.compile(r"^(LICEN[CS]E|COPYING)([-_.].*)?$", re.IGNORECASE)


def source_dir(cache: dict[str, str], name: str) -> Path:
    """与 FetchContent 的规则相同：FETCHCONTENT_SOURCE_DIR_<名称> 优先，否则是 <FETCHCONTENT_BASE_DIR>/<名称>-src。"""
    override = cache.get(f"FETCHCONTENT_SOURCE_DIR_{name.upper()}", "")
    if override:
        return Path(override)
    return Path(cache.get("FETCHCONTENT_BASE_DIR", "")) / f"{name.lower()}-src"


def shipped_dependencies() -> set[str]:
    names = set()
    for cmake in (ROOT / "src").rglob("CMakeLists.txt"):
        names.update(re.findall(r"^\s*llmgw_fetch_dependency\((\w+)\)", cmake.read_text(), re.M))
    return names


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
    shipped = shipped_dependencies()
    if unknown := shipped - deps.keys():
        print(f"src/ 中使用了依赖清单中没有的依赖：{sorted(unknown)}", file=sys.stderr)
        return 1
    for name, info in sorted(deps.items()):
        if name not in shipped:
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
