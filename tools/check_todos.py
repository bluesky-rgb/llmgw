#!/usr/bin/env python3
"""检查 TODO 的格式：必须写成 TODO(#<Issue 编号>): 说明，关联到一个 Issue。

不关联 Issue 的 TODO 往往会被永远遗忘，变成看不见的技术债。
用法：python3 tools/check_todos.py [仓库根目录]
"""
import re
import sys
from pathlib import Path

SCAN_DIRS = ["src", "tests/unit"]
SUFFIXES = {".h", ".hpp", ".cpp", ".cc", ".ipp", ".in", ".cmake"}
TODO_RE = re.compile(r"\b(TODO|FIXME|XXX|HACK)\b")
VALID_RE = re.compile(r"\bTODO\(#\d+\):\s*\S")


def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    errors, checked = [], 0
    for d in SCAN_DIRS:
        base = root / d
        if not base.is_dir():
            continue
        for f in sorted(base.rglob("*")):
            if not f.is_file() or (f.suffix not in SUFFIXES and f.name != "CMakeLists.txt"):
                continue
            checked += 1
            for no, line in enumerate(f.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                if TODO_RE.search(line) and not VALID_RE.search(line):
                    errors.append(f"{f.relative_to(root)}:{no}: TODO 必须写成 \"TODO(#Issue编号): 说明\"：{line.strip()}")
    for e in errors:
        print("✗", e)
    print(f"检查了 {checked} 个文件，发现 {len(errors)} 个不合格的 TODO")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
