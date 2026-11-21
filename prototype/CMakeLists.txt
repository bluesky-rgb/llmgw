# v0.1 原型：基于 cpp-httplib 的线程池阻塞模型。
# 只作为性能对照组保留，不再演进；不受新模块的 -Werror、clang-tidy 约束。
find_package(Threads REQUIRED)
find_package(OpenSSL 3.0 REQUIRED)

llmgw_fetch_dependency(cpp_httplib)
llmgw_fetch_dependency(nlohmann_json)

add_library(prototype_third_party INTERFACE)
target_include_directories(prototype_third_party SYSTEM INTERFACE
  ${cpp_httplib_SOURCE_DIR}
  ${nlohmann_json_SOURCE_DIR}/single_include)
target_compile_definitions(prototype_third_party INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT)
target_link_libraries(prototype_third_party INTERFACE OpenSSL::SSL OpenSSL::Crypto Threads::Threads)

add_library(prototype_core
  src/config.cpp
  src/url.cpp
  src/sse.cpp
  src/metrics.cpp
  src/logger.cpp
  src/gateway.cpp)
target_include_directories(prototype_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(prototype_core PUBLIC prototype_third_party)
set_target_properties(prototype_core PROPERTIES CXX_STANDARD 17)
target_compile_options(prototype_core PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)

# 可执行文件仍然叫 llmgw，输出到 build/prototype/llmgw；任务 1.9 之后由新架构取代
add_executable(llmgw src/main.cpp)
target_link_libraries(llmgw PRIVATE prototype_core)
set_target_properties(llmgw PROPERTIES CXX_STANDARD 17)

if(LLMGW_BUILD_TESTS)
  add_executable(prototype_tests tests/test_main.cpp)
  target_link_libraries(prototype_tests PRIVATE prototype_core)
  set_target_properties(prototype_tests PROPERTIES CXX_STANDARD 17)
  add_test(NAME prototype_tests COMMAND prototype_tests)
endif()
