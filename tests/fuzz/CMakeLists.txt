# 模糊测试（任务 1.3 之后的排查中加入）。
#
#   Clang：使用 libFuzzer（覆盖率引导）。整个构建应该带 -fsanitize=fuzzer-no-link,address,undefined，
#          让被测的库代码也被插桩；fuzz 目标本身链接 -fsanitize=fuzzer
#   其他编译器：使用 standalone_driver.cpp（随机变异，没有覆盖率引导），用于本机快速检查
#
# 运行：llmgw_fuzz_<名字> <语料目录> -max_total_time=60（libFuzzer）
#       llmgw_fuzz_<名字> <语料目录> 60（standalone）
function(llmgw_fuzz_target name)
  cmake_parse_arguments(F "" "" "SOURCES;LIBS" ${ARGN})
  add_executable(llmgw_fuzz_${name} ${F_SOURCES})
  target_link_libraries(llmgw_fuzz_${name} PRIVATE ${F_LIBS})
  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_link_options(llmgw_fuzz_${name} PRIVATE -fsanitize=fuzzer)
  else()
    target_sources(llmgw_fuzz_${name} PRIVATE standalone_driver.cpp)
  endif()
endfunction()

llmgw_fuzz_target(config SOURCES config_fuzz.cpp LIBS llmgw::config)
llmgw_fuzz_target(syntax SOURCES syntax_fuzz.cpp LIBS llmgw::config)
llmgw_fuzz_target(chat_fields SOURCES chat_fields_fuzz.cpp LIBS llmgw::server)
# chat_fields 测试的是 server 的内部组件 http_util
target_include_directories(llmgw_fuzz_chat_fields PRIVATE "${CMAKE_SOURCE_DIR}/src/server")
find_package(OpenSSL 3 REQUIRED)
# 上游的响应（任务 1.4）：测试 upstream 的内部组件（响应头、响应体的读取）
llmgw_fuzz_target(upstream_response SOURCES upstream_response_fuzz.cpp LIBS llmgw::upstream Boost::beast OpenSSL::SSL)
target_include_directories(llmgw_fuzz_upstream_response PRIVATE "${CMAKE_SOURCE_DIR}/src/upstream")
# SSE 解析器（任务 1.5）：切块方式不影响结果
llmgw_fuzz_target(sse SOURCES sse_fuzz.cpp LIBS llmgw::stream)
target_include_directories(llmgw_fuzz_sse PRIVATE "${CMAKE_SOURCE_DIR}/src/stream")
# 请求体改写（任务 1.6）：输出是合法的 JSON，并且满足每一条规则；用 simdjson DOM 独立检查
llmgw_fuzz_target(rewrite SOURCES rewrite_fuzz.cpp LIBS llmgw::provider llmgw::config simdjson::simdjson)
