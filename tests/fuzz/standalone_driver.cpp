// 没有 libFuzzer
// 时使用的简单驱动：从语料出发做随机变异（翻转、插入、删除、复制字节，以及纯随机输入）， 调用同一个
// LLVMFuzzerTestOneInput。没有覆盖率引导，但足以发现崩溃和 sanitizer 报告。 用法：driver <语料目录>
// <秒数> [种子]
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "用法: %s <语料目录> <秒数> [种子]\n", argv[0]);
    return 2;
  }
  std::vector<std::string> corpus;
  for (const auto& e : std::filesystem::directory_iterator(argv[1])) {
    std::ifstream in(e.path(), std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    corpus.push_back(ss.str());
  }
  const auto seconds = std::atoi(argv[2]);
  std::mt19937_64 rng(argc > 3 ? std::strtoull(argv[3], nullptr, 10) : std::random_device{}());
  auto pick = [&](std::size_t n) { return n == 0 ? 0 : static_cast<std::size_t>(rng() % n); };
  const std::string interesting[] = {
      "\"",       "\\",   "{",    "}",     "[",  "]",       ":",       ",",  "\0", "\r\n", "\xff",
      "\xe4\xb8", "null", "true", "1e999", "-0", "\\u0000", "\\ud800", "[]", ".",  "://",  "@"};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  std::uint64_t runs = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    std::string s = corpus.empty() || rng() % 16 == 0 ? std::string() : corpus[pick(corpus.size())];
    const int mutations = 1 + static_cast<int>(rng() % 8);
    for (int m = 0; m < mutations; ++m) {
      switch (rng() % 6) {
        case 0:  // 翻转一个字节
          if (!s.empty()) s[pick(s.size())] ^= static_cast<char>(1U << (rng() % 8));
          break;
        case 1:  // 插入随机字节
          s.insert(pick(s.size() + 1), 1, static_cast<char>(rng()));
          break;
        case 2:  // 删除一段
          if (!s.empty()) {
            const auto at = pick(s.size());
            s.erase(at, 1 + pick(std::min<std::size_t>(16, s.size() - at)));
          }
          break;
        case 3:  // 插入一个“有意思”的片段
          s.insert(pick(s.size() + 1), interesting[pick(std::size(interesting))]);
          break;
        case 4:  // 复制一段（制造深嵌套、超长值）
          if (!s.empty()) {
            const auto at = pick(s.size());
            const auto len = 1 + pick(std::min<std::size_t>(64, s.size() - at));
            const auto piece = s.substr(at, len);
            for (int k = 0, n = 1 + static_cast<int>(rng() % 50); k < n; ++k) s.insert(at, piece);
          }
          break;
        default:  // 截断
          if (!s.empty()) s.resize(pick(s.size()));
          break;
      }
    }
    if (s.size() > (1U << 20)) s.resize(1U << 20);
    LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
    ++runs;
  }
  std::printf("完成 %llu 次（%d 秒），没有发现问题\n", static_cast<unsigned long long>(runs),
              seconds);
  return 0;
}
