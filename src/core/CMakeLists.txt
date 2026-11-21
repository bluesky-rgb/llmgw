# 模块 core：公共类型与接口（只有头文件）。版本号由项目版本生成，只有一个来源（根 CMakeLists.txt 的 project(VERSION)）
llmgw_add_module(core)
configure_file(version.h.in "${CMAKE_CURRENT_BINARY_DIR}/include/llmgw/core/version.h" @ONLY)
target_include_directories(llmgw_core INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/include")

# 第三方依赖（ADR-007）：
#   Boost.Asio  —— 只使用 awaitable，集中在 task.h
#   tl::expected —— Result<T> 的实现，集中在 result.h
llmgw_fetch_dependency(boost)
llmgw_fetch_dependency(tl_expected)
add_library(llmgw_tl_expected INTERFACE)
target_include_directories(llmgw_tl_expected SYSTEM INTERFACE "${tl_expected_SOURCE_DIR}/include")
target_link_libraries(llmgw_core INTERFACE Boost::asio llmgw_tl_expected)
