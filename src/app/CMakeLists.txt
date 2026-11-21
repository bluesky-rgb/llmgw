# llmgw 可执行文件（模块 app）。可执行文件不用 llmgw_add_module，依赖按 tools/module_deps.json 手工列出。
add_executable(llmgw_app main.cpp)
set_target_properties(llmgw_app PROPERTIES OUTPUT_NAME llmgw)
target_link_libraries(llmgw_app PRIVATE llmgw::core llmgw::config)
target_compile_options(llmgw_app PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
  $<$<AND:$<BOOL:${LLMGW_WARNINGS_AS_ERRORS}>,$<CXX_COMPILER_ID:GNU,Clang>>:-Werror>)
