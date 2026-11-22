#!/usr/bin/env python3
"""检查提交区间内的每个提交都有 DCO 声明（Signed-off-by），且声明人与提交作者的邮箱一致。

用法：python3 tools/check_dco.py <base> <head>
     检查 base..head 中不是合并提交的所有提交。CI 中只对来自 fork 的 PR 运行（见 CONTRIBUTING.md）。
"""

import re
import subprocess
import sys

SIGNOFF = re.compile(r"^Signed-off-by:\s*(.+?)\s*<([^<>]+)>\s*$", re.MULTILINE)


def git(*args: str) -> str:
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True).stdout


def check(base: str, head: str) -> list[str]:
    problems = []
    for sha in git("rev-list", "--no-merges", f"{base}..{head}").split():
        email = git("show", "-s", "--format=%ae", sha).strip()
        message = git("show", "-s", "--format=%B", sha)
        subject = message.splitlines()[0] if message else ""
        signers = [m.group(2).strip().lower() for m in SIGNOFF.finditer(message)]
        if not signers:
            problems.append(f"{sha[:10]} {subject}：缺少 Signed-off-by")
        elif email.lower() not in signers:
            problems.append(f"{sha[:10]} {subject}：Signed-off-by 的邮箱与作者 <{email}> 不一致")
    return problems


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    problems = check(sys.argv[1], sys.argv[2])
    for p in problems:
        print(p)
    if problems:
        print("\n请用 `git commit -s` 提交；补签已有的提交：`git rebase --signoff <base>`（见 CONTRIBUTING.md）")
        return 1
    print("DCO 检查通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
