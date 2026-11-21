# 模块 core：公共类型与接口。版本号由项目版本生成，只有一个来源（根 CMakeLists.txt 的 project(VERSION)）
llmgw_add_module(core)
configure_file(version.h.in "${CMAKE_CURRENT_BINARY_DIR}/include/llmgw/core/version.h" @ONLY)
target_include_directories(llmgw_core INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/include")
