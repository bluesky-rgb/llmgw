cmake_minimum_required(VERSION 3.24)
project(llmgw VERSION 0.1.0 LANGUAGES CXX)

# 新代码使用 C++20（协程）；原型在 prototype/ 中单独设置为 C++17
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LLMGW_BUILD_TESTS "构建 tests/" ON)
option(LLMGW_BUILD_PROTOTYPE "构建 prototype/（v0.1 原型，作为性能对照组）" ON)
option(LLMGW_WARNINGS_AS_ERRORS "新模块把编译警告视为错误" ON)

include(cmake/Dependencies.cmake)
include(cmake/LlmgwModule.cmake)

if(LLMGW_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(src)

if(LLMGW_BUILD_TESTS)
  add_subdirectory(tests)
endif()

if(LLMGW_BUILD_PROTOTYPE)
  add_subdirectory(prototype)
endif()
