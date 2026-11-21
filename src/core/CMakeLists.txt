# 模块 core
llmgw_add_module(core SOURCES violation.cpp)
configure_file(version.h.in "${CMAKE_CURRENT_BINARY_DIR}/include/llmgw/core/version.h" @ONLY)
target_include_directories(llmgw_core PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/include")
