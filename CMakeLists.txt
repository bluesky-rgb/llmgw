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
option(LLMGW_BUILD_SPIKE "构建 spike/（技术验证代码，默认关闭，避免 CI 每次下载 Boost）" OFF)
option(LLMGW_BUILD_FUZZ "构建 tests/fuzz/（模糊测试，默认关闭；Clang 下使用 libFuzzer）" OFF)

# 必须对所有目标统一定义（不能只在某个模块里 PRIVATE 定义）：这个宏会改变 Asio 一些成员函数的返回类型
# （比如 basic_socket::close(ec) 返回 void 还是 error_code），而类模板成员函数的返回类型不参与名字修饰，
# 不一致时链接器只保留一个版本，另一些调用方按错误的调用约定调用它（ODR 违规，任务 1.4 验证时发现：
# UBSan 报告 this = 0x2）。core/task.h 中检查了这一点
add_compile_definitions(BOOST_ASIO_NO_DEPRECATED)

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

if(LLMGW_BUILD_FUZZ)
  add_subdirectory(tests/fuzz)
endif()

if(LLMGW_BUILD_SPIKE)
  add_subdirectory(spike/coroutine-proxy)
endif()
