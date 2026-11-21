#!/usr/bin/env python3
"""检查模块依赖规则，防止代码结构腐化。

规则来自 tools/module_deps.json：
  1. 规则本身：依赖关系不能有环，且每个依赖都必须位于更低的层
  2. 代码：src/<模块>/ 下的文件，只能 #include 允许依赖的模块的公开头文件
     （形如 "llmgw/<模块>/xxx.h"），不能用 "../" 跨目录引用
  3. 单个源文件不能超过 max_file_lines 行

用法：python3 tools/check_deps.py [仓库根目录]
发现问题时以非零状态退出，CI 会因此失败。
"""
import json
import re
import sys
from pathlib import Path

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')
SOURCE_SUFFIXES = {".h", ".hpp", ".cpp", ".cc", ".ipp"}


def check_rules(rules):
    errors = []
    modules = rules["modules"]
    layer_of = {m: i for i, layer in enumerate(rules["layers"]) for m in layer}
    for m in modules:
        if m not in layer_of:
            errors.append(f"规则错误：模块 {m} 没有出现在 layers 中")
    for m, deps in modules.items():
        for d in deps:
            if d not in modules:
                errors.append(f"规则错误：{m} 依赖了不存在的模块 {d}")
            elif m in layer_of and d in layer_of and layer_of[d] >= layer_of[m]:
                errors.append(f"规则错误：{m}（第 {layer_of[m]} 层）不能依赖同层或更高层的 {d}（第 {layer_of[d]} 层）")
    return errors


def check_sources(root, rules):
    errors, checked = [], 0
    prefix = rules["include_prefix"]
    modules = rules["modules"]
    src_root = root / rules["src_root"]
    for module, allowed in modules.items():
        mdir = src_root / module
        if not mdir.is_dir():
            continue
        for f in sorted(mdir.rglob("*")):
            if f.suffix not in SOURCE_SUFFIXES or not f.is_file():
                continue
            checked += 1
            rel = f.relative_to(root)
            lines = f.read_text(encoding="utf-8", errors="replace").splitlines()
            if len(lines) > rules["max_file_lines"]:
                errors.append(f"{rel}: 文件有 {len(lines)} 行，超过上限 {rules['max_file_lines']}，请拆分")
            for no, line in enumerate(lines, 1):
                m = INCLUDE_RE.match(line)
                if not m:
                    continue
                inc = m.group(1)
                if "../" in inc:
                    errors.append(f"{rel}:{no}: 不允许用 \"../\" 跨目录引用（{inc}），请改用 \"{prefix}/<模块>/...\"")
                    continue
                parts = inc.split("/")
                if len(parts) >= 3 and parts[0] == prefix:
                    target = parts[1]
                    if target == module:
                        continue
                    if target not in modules:
                        errors.append(f"{rel}:{no}: 引用了未知模块 {target}（{inc}）")
                    elif target not in allowed:
                        errors.append(
                            f"{rel}:{no}: 模块 {module} 不允许依赖 {target}（{inc}）。"
                            f"允许的依赖：{', '.join(allowed) or '无'}")
    return errors, checked


def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    rules = json.loads((root / "tools" / "module_deps.json").read_text(encoding="utf-8"))
    errors = check_rules(rules)
    src_errors, checked = check_sources(root, rules)
    errors += src_errors
    for e in errors:
        print("✗", e)
    print(f"检查了 {checked} 个源文件，发现 {len(errors)} 个问题")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
