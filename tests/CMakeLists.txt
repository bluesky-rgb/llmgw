# 单元测试：目录与模块一一对应（tests/unit/<模块>/）。单元测试不访问网络。
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
llmgw_fetch_dependency(googletest)
include(GoogleTest)

add_executable(llmgw_unit_tests
  unit/core/version_test.cpp
  unit/core/error_test.cpp
  unit/core/result_test.cpp
  unit/core/request_context_test.cpp
  unit/core/response_stream_test.cpp
  unit/core/metrics_test.cpp
  unit/core/task_test.cpp
  unit/config/syntax_test.cpp
  unit/config/json_reader_test.cpp
  unit/config/loader_test.cpp
  unit/config/pipeline_test.cpp
  unit/config/keys_test.cpp
  unit/server/http_util_test.cpp
  unit/stream/sse_parser_test.cpp
  unit/stream/inspect_test.cpp
  unit/stream/stream_test.cpp
  unit/stream/review_test.cpp
  unit/provider/rewrite_test.cpp
  unit/router/router_test.cpp
  unit/middleware/chain_test.cpp
  unit/middleware/auth_test.cpp
  unit/executor/classify_test.cpp
  unit/stream/provider_samples_test.cpp)
target_link_libraries(llmgw_unit_tests PRIVATE GTest::gtest_main llmgw::core llmgw::config llmgw::server
                      llmgw::stream llmgw::provider llmgw::router llmgw::executor llmgw::middleware)
# 单元测试可以测试模块内部的组件（比如 config 的 json_reader.h），所以能看到 src/<模块>/ 下的私有头文件
target_include_directories(llmgw_unit_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/config" "${CMAKE_SOURCE_DIR}/src/server"
                           "${CMAKE_SOURCE_DIR}/src/stream" "${CMAKE_SOURCE_DIR}/src/provider"
                           "${CMAKE_SOURCE_DIR}/src/executor")
llmgw_fetch_dependency(nlohmann_json)
target_include_directories(llmgw_unit_tests SYSTEM PRIVATE "${nlohmann_json_SOURCE_DIR}/single_include")
target_compile_definitions(llmgw_unit_tests PRIVATE LLMGW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(llmgw_unit_tests PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
  $<$<AND:$<BOOL:${LLMGW_WARNINGS_AS_ERRORS}>,$<CXX_COMPILER_ID:GNU,Clang>>:-Werror>)
gtest_discover_tests(llmgw_unit_tests)

# llmgw check 的命令行行为（验证项 F7、F8）。都通过 expect_exit.cmake 运行，同时检查退出码和输出，
# 并用 REQUIRE 确认处理的确实是指定的文件（避免参数被悄悄丢掉、碰巧得到期望的退出码）
set(llmgw_bin $<TARGET_FILE:llmgw_app>)
set(expect_exit ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/expect_exit.cmake)
function(llmgw_cli_test name)
  cmake_parse_arguments(T "" "ARGS;EXPECT;REQUIRE;FORBID;ENV" "" ${ARGN})
  set(defs -DCMD=${llmgw_bin} "-DARGS=${T_ARGS}" -DEXPECT=${T_EXPECT})
  if(T_REQUIRE)
    list(APPEND defs "-DREQUIRE=${T_REQUIRE}")
  endif()
  if(T_FORBID)
    list(APPEND defs "-DFORBID=${T_FORBID}")
  endif()
  add_test(NAME cli.${name}
           COMMAND ${CMAKE_COMMAND} -E env ${T_ENV} ${CMAKE_COMMAND} ${defs}
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/expect_exit.cmake)
endfunction()

set(example ${CMAKE_SOURCE_DIR}/config/llmgw.example.json)
set(bad ${CMAKE_CURRENT_SOURCE_DIR}/cli/bad.json)
llmgw_cli_test(check_example_is_valid ARGS "check|-c|${example}|--skip-env" EXPECT 0
               REQUIRE "llmgw.example.json：配置有效")
llmgw_cli_test(check_bad_config_exits_1 ARGS "check|-c|${bad}|--skip-env" EXPECT 1
               REQUIRE "bad.json：1 个错误.*是不是 \"openai\"")
llmgw_cli_test(check_missing_file_exits_2 ARGS "check|-c|/nonexistent/llmgw.json" EXPECT 2
               REQUIRE "/nonexistent/llmgw.json")
llmgw_cli_test(unknown_argument_exits_2 ARGS "check|--bogus" EXPECT 2 REQUIRE "未知参数")
# pipeline 中的插件名不存在（任务 2.1 M6）：check 和 serve 都拒绝，并说明可用的插件
set(unknown_plugin ${CMAKE_CURRENT_SOURCE_DIR}/cli/unknown_plugin.json)
llmgw_cli_test(check_unknown_plugin_exits_1 ARGS "check|-c|${unknown_plugin}|--skip-env" EXPECT 1
               REQUIRE "unknown_plugin.json：1 个错误.*pipeline\\[0\\]：没有名为 \"auht\" 的插件（是不是 \"auth\"？）")
llmgw_cli_test(serve_unknown_plugin_exits_1 ARGS "serve|-c|${unknown_plugin}" EXPECT 1
               REQUIRE "没有名为 \"auht\" 的插件" ENV "OPENAI_API_KEY=x")
llmgw_cli_test(plugins_lists_registry ARGS "plugins" EXPECT 0 REQUIRE "auth")
# 任务 2.2：keygen；配置中不能出现明文 key（并且错误信息中不回显它）；auth 与 keys 的交叉检查
llmgw_cli_test(keygen_prints_key_and_hash ARGS "keygen|--id|app-chatbot" EXPECT 0
               REQUIRE "sk-gw-[A-Za-z0-9_-]+.*\"id\": \"app-chatbot\", \"hash\": \"sha256:[0-9a-f]+\"")
llmgw_cli_test(keygen_requires_id ARGS "keygen" EXPECT 2 REQUIRE "--id")
llmgw_cli_test(keygen_rejects_bad_id ARGS "keygen|--id|has space" EXPECT 2 REQUIRE "只能包含")
set(cli_dir ${CMAKE_CURRENT_SOURCE_DIR}/cli)
llmgw_cli_test(check_rejects_plaintext_key ARGS "check|-c|${cli_dir}/plaintext_key.json|--skip-env"
               EXPECT 1 REQUIRE "keys\\[0\\]\\.hash：这是 key 本身" FORBID "PLAINTEXT_KEY")
llmgw_cli_test(serve_rejects_plaintext_key ARGS "serve|-c|${cli_dir}/plaintext_key.json" EXPECT 1
               REQUIRE "这是 key 本身" FORBID "PLAINTEXT_KEY" ENV "OPENAI_API_KEY=x")
# JSON 语法错误的提示不能带出出错位置附近的文本（nlohmann 的 "last read: '...'" 会把 key 打印出来，
# 任务 2.2 独立审查 #1）
llmgw_cli_test(check_syntax_error_does_not_echo_text ARGS "check|-c|${cli_dir}/syntax_error_with_key.json|--skip-env"
               EXPECT 1 REQUIRE "JSON 语法错误" FORBID "SYNTAX_ERROR_SECRET")
llmgw_cli_test(check_syntax_error_trailing_does_not_echo_text ARGS "check|-c|${cli_dir}/syntax_error_trailing.json|--skip-env"
               EXPECT 1 REQUIRE "JSON 语法错误" FORBID "TRAILING_SECRET")
llmgw_cli_test(check_auth_without_keys ARGS "check|-c|${cli_dir}/auth_without_keys.json|--skip-env"
               EXPECT 1 REQUIRE "启用了 auth 但没有配置 keys")
llmgw_cli_test(check_keys_without_auth_warns ARGS "check|-c|${cli_dir}/keys_without_auth.json|--skip-env"
               EXPECT 0 REQUIRE "1 个警告.*不会校验 key")
# 密钥不能出现在 check 的任何输出中（配置有错误时也一样）
llmgw_cli_test(check_never_prints_api_key ARGS "check|-c|${bad}" EXPECT 1 REQUIRE "bad.json"
               FORBID "sk-VERY-SECRET" ENV "OPENAI_API_KEY=sk-VERY-SECRET-1;MINIMAX_API_KEY=sk-VERY-SECRET-2")

# 真实上游的响应样本（任务 1.8）
target_compile_definitions(llmgw_unit_tests PRIVATE
  LLMGW_PROVIDER_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/tests/data/providers")

# llmgw serve 的进程级冒烟测试（任务 1.7 X19）：真实进程 + Python 写的假上游
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME cli.serve_smoke
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli/serve_smoke.py $<TARGET_FILE:llmgw_app>)
  set_tests_properties(cli.serve_smoke PROPERTIES TIMEOUT 60)
  add_test(NAME cli.provider_probe_self_test
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/provider-probe/probe.py --self-test
                   --gateway $<TARGET_FILE:llmgw_app>)
  # 发布物中的第三方许可证覆盖所有编进程序的依赖（任务 1.10 的遗漏，2.1 前发现）
  add_test(NAME tools.third_party_licenses
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli/third_party_licenses_test.py
                   ${CMAKE_BINARY_DIR})
  # 假上游在 127.0.0.1 上：不能经过开发环境中的代理
  set_tests_properties(cli.provider_probe_self_test PROPERTIES TIMEOUT 180
                       ENVIRONMENT "HTTPS_PROXY=;https_proxy=;HTTP_PROXY=;http_proxy=")
endif()

# 集成测试：在进程内启动 Server（端口 0），用测试替身代替上游，不访问网络
add_executable(llmgw_integration_tests
  integration/server/fixture.cpp
  integration/server/basic_test.cpp
  integration/server/limits_test.cpp
  integration/server/security_test.cpp
  integration/server/review_test.cpp
  integration/server/middleware_test.cpp)
target_link_libraries(llmgw_integration_tests PRIVATE GTest::gtest_main llmgw::server Boost::beast)
target_compile_options(llmgw_integration_tests PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
  $<$<AND:$<BOOL:${LLMGW_WARNINGS_AS_ERRORS}>,$<CXX_COMPILER_ID:GNU,Clang>>:-Werror>)
gtest_discover_tests(llmgw_integration_tests PROPERTIES TIMEOUT 30)

# upstream 的集成测试：进程内的测试上游（HTTP/HTTPS）和测试代理，证书在测试启动时生成（任务 1.4）
find_package(OpenSSL 3 REQUIRED)
add_executable(llmgw_upstream_tests
  integration/upstream/tls_certs.cpp
  integration/upstream/test_servers.cpp
  integration/upstream/pool_test.cpp
  integration/upstream/failure_test.cpp
  integration/upstream/stress_test.cpp
  integration/upstream/perf_test.cpp
  integration/upstream/review_test.cpp
  integration/provider/attempt_test.cpp
  integration/executor/executor_test.cpp
  integration/app/gateway_test.cpp
  integration/app/prototype_parity_test.cpp
  integration/app/auth_test.cpp)
# 审查用例（review_test.cpp）直接测试内部组件（连接池、报文、TLS 身份设置）
target_include_directories(llmgw_upstream_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/upstream")
target_link_libraries(llmgw_upstream_tests PRIVATE GTest::gtest_main llmgw::upstream llmgw::provider llmgw::executor llmgw::config llmgw_app_lib Boost::beast
                      OpenSSL::SSL OpenSSL::Crypto)
# provider 的串联测试（任务 1.6 P6）复用这里的测试上游
target_include_directories(llmgw_upstream_tests SYSTEM PRIVATE "${nlohmann_json_SOURCE_DIR}/single_include")
# GCC 对省略了字段的指定初始化器（{.tls = ...}）也报 missing-field-initializers；测试中大量使用这种写法。
# mismatched-new-delete：GCC 开优化时对 Asio 协程帧（自带 operator new/delete）组合 && 的误报
target_compile_options(llmgw_upstream_tests PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wno-missing-field-initializers>
  $<$<CXX_COMPILER_ID:GNU>:-Wno-mismatched-new-delete>
  $<$<AND:$<BOOL:${LLMGW_WARNINGS_AS_ERRORS}>,$<CXX_COMPILER_ID:GNU,Clang>>:-Werror>)
gtest_discover_tests(llmgw_upstream_tests PROPERTIES TIMEOUT 60)
