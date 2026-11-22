#!/usr/bin/env python3
"""tools/third_party_licenses.py 的回归测试。

编进程序的依赖 = src/ 下的 CMakeLists.txt 中 llmgw_fetch_dependency() 的那些；它们的许可证都必须
出现在输出中，只用于测试的依赖（googletest）不能出现。nlohmann_json 曾经因为依赖清单中的说明
过时（写着只用于原型）而被漏掉（任务 2.1 前发现）。

用法：third_party_licenses_test.py <构建目录>
"""

import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def fetched_by_src() -> set[str]:
    names = set()
    for cmake in (ROOT / "src").rglob("CMakeLists.txt"):
        names.update(re.findall(r"^\s*llmgw_fetch_dependency\((\w+)\)", cmake.read_text(), re.M))
    return names


def main() -> int:
    build_dir = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "THIRD_PARTY_LICENSES"
        subprocess.run([sys.executable, str(ROOT / "tools/third_party_licenses.py"), build_dir, str(out)],
                       check=True)
        listed = set(re.findall(r"^={78}\n(\w+) ", out.read_text(), re.M))  # 每段许可证的标题行
    expected = fetched_by_src()
    # 防止 fetched_by_src 自己出错（比如正则写错得到空集合）而让测试总是通过
    assert {"boost", "simdjson", "tl_expected", "nlohmann_json"} <= expected, expected
    problems = []
    if missing := expected - listed:
        problems.append(f"编进程序但没有许可证：{sorted(missing)}")
    if extra := listed - expected:
        problems.append(f"没有编进程序却列出了许可证：{sorted(extra)}")
    for p in problems:
        print(p)
    if not problems:
        print(f"通过：{sorted(listed)}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
