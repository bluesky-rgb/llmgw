# llmgw 可执行文件（模块 app）。可执行文件不用 llmgw_add_module，依赖按 tools/module_deps.json 手工列出。
# Gateway、serve 编译成静态库，可执行文件和测试都链接它（任务 1.7）
add_library(llmgw_app_lib STATIC gateway.cpp serve.cpp plugins.cpp)
target_include_directories(llmgw_app_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(llmgw_app_lib PUBLIC llmgw::core llmgw::config llmgw::upstream llmgw::router
                      llmgw::executor llmgw::middleware llmgw::server)
add_executable(llmgw_app main.cpp)
set_target_properties(llmgw_app PROPERTIES OUTPUT_NAME llmgw)
target_link_libraries(llmgw_app PRIVATE llmgw_app_lib)
foreach(t llmgw_app_lib llmgw_app)
  target_compile_options(${t} PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<AND:$<BOOL:${LLMGW_WARNINGS_AS_ERRORS}>,$<CXX_COMPILER_ID:GNU,Clang>>:-Werror>)
endforeach()
