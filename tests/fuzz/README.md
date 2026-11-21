# 模糊测试

用随机生成的畸形输入测试各个解析入口：任何输入都不能导致崩溃或 sanitizer 报告。2026-11-21 排查时加入，第一次运行就发现了一个 bug（配置中的 `1e999` 会让 `llmgw check` 崩溃）。

| 目标 | 测试什么 | 额外检查的性质 |
|---|---|---|
| `config` | `config::parse()`：整个配置文件 | 配置无效时必须至少给出一个错误 |
| `syntax` | URL、监听地址、字段路径 | 字段路径转回文本后再解析，结果不变 |
| `chat_fields` | 从请求体中取 `model`/`stream`（simdjson）、请求 ID、路径 | 成功时 `model` 不为空 |

`corpus/` 下是初始语料（合法的输入），变异从它们开始。

## 运行

**CI**（`.github/workflows/ci.yml` 的 `fuzz` 任务）：Clang + libFuzzer（覆盖率引导），每个目标 60 秒。发现问题时，触发问题的输入作为构件上传。

**本机，有 libFuzzer**（需要 `libclang-rt-18-dev`）：

```bash
cmake -S . -B build-fuzz -G Ninja -DCMAKE_CXX_COMPILER=clang++-18 -DLLMGW_BUILD_FUZZ=ON \
      -DCMAKE_CXX_FLAGS="-fsanitize=fuzzer-no-link,address,undefined" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-fuzz --target llmgw_fuzz_config
build-fuzz/tests/fuzz/llmgw_fuzz_config tests/fuzz/corpus/config -max_total_time=60
```

**本机，没有 libFuzzer**（GCC）：同样的目标会链接 `standalone_driver.cpp`，做随机变异（翻转、插入、删除、复制字节，以及“有意思”的片段），没有覆盖率引导：

```bash
cmake -S . -B build-fuzz -G Ninja -DLLMGW_BUILD_FUZZ=ON \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all"
cmake --build build-fuzz --target llmgw_fuzz_config
build-fuzz/tests/fuzz/llmgw_fuzz_config tests/fuzz/corpus/config 60   # 秒数；可选第三个参数为随机种子
```

## 发现问题之后

1. 把触发问题的输入写成一个单元测试（放到对应模块的 `tests/unit/` 下），确认它在修复前失败
2. 修复
3. 把这个输入加入 `corpus/`，以后的模糊测试从它开始变异
