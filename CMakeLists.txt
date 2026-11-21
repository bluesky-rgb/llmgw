cmake_minimum_required(VERSION 3.20)
project(llm_gateway VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LLMGW_BUILD_TESTS "Build tests" ON)

find_package(Threads REQUIRED)
find_package(OpenSSL 3.0 REQUIRED)

# ---- 第三方依赖（纯头文件库）----------------------------------------------
# 离线构建时可以指定本地源码目录，例如：
#   -DFETCHCONTENT_SOURCE_DIR_HTTPLIB=/path/to/cpp-httplib
#   -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=/path/to/json
include(FetchContent)

FetchContent_Declare(httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG        v0.67.0
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  _header_only_do_not_add_subdirectory)

FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  _header_only_do_not_add_subdirectory)

FetchContent_MakeAvailable(httplib nlohmann_json)

add_library(third_party INTERFACE)
target_include_directories(third_party SYSTEM INTERFACE
  ${httplib_SOURCE_DIR}
  ${nlohmann_json_SOURCE_DIR}/single_include)
target_compile_definitions(third_party INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT)
target_link_libraries(third_party INTERFACE OpenSSL::SSL OpenSSL::Crypto Threads::Threads)

# ---- 网关核心库 ------------------------------------------------------------
add_library(llmgw_core
  src/config.cpp
  src/url.cpp
  src/sse.cpp
  src/metrics.cpp
  src/logger.cpp
  src/gateway.cpp)
target_include_directories(llmgw_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(llmgw_core PUBLIC third_party)
target_compile_options(llmgw_core PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)

add_executable(llmgw src/main.cpp)
target_link_libraries(llmgw PRIVATE llmgw_core)

# ---- 测试 ------------------------------------------------------------------
if(LLMGW_BUILD_TESTS)
  enable_testing()
  add_executable(llmgw_tests tests/test_main.cpp)
  target_link_libraries(llmgw_tests PRIVATE llmgw_core)
  add_test(NAME llmgw_tests COMMAND llmgw_tests)
endif()
