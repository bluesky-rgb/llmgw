# 单元测试：目录与模块一一对应（tests/unit/<模块>/）。单元测试不访问网络。
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
llmgw_fetch_dependency(googletest)
include(GoogleTest)

add_executable(llmgw_unit_tests
  unit/core/version_test.cpp)
target_link_libraries(llmgw_unit_tests PRIVATE GTest::gtest_main llmgw::core)
target_compile_options(llmgw_unit_tests PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
  $<$<AND:$<BOOL:${LLMGW_WARNINGS_AS_ERRORS}>,$<CXX_COMPILER_ID:GNU,Clang>>:-Werror>)
gtest_discover_tests(llmgw_unit_tests)
