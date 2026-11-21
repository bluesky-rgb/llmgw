#!/usr/bin/env bash
# 对 src/ 下参与编译的源文件运行 clang-tidy（需要先配置出 compile_commands.json）。
# 同时检查 llmgw_add_module 为每个公开头文件生成的 header_check/*.cpp：
# 只有头文件的模块（比如 core）没有自己的 .cpp，不这样做的话它的头文件永远不会被检查。
# 用法：tools/run_clang_tidy.sh <构建目录>
set -euo pipefail
build_dir="${1:-build}"
tidy="${CLANG_TIDY:-clang-tidy}"
files=$(python3 - "$build_dir" <<'PY'
import json, sys, os
db = json.load(open(os.path.join(sys.argv[1], "compile_commands.json")))
root = os.getcwd() + "/src/"
print("\n".join(sorted({e["file"] for e in db
                         if e["file"].startswith(root) or "/header_check/" in e["file"]})))
PY
)
if [ -z "$files" ]; then
  echo "src/ 下还没有参与编译的源文件或公开头文件，跳过 clang-tidy"
  exit 0
fi
echo "$files" | xargs -r "$tidy" -p "$build_dir" --quiet
