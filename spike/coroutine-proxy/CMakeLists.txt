# 协程 demo（任务 0.6 的技术验证）。不属于产品代码，默认不构建：-DLLMGW_BUILD_SPIKE=ON
find_package(Threads REQUIRED)
llmgw_fetch_dependency(boost)

add_executable(coroutine-proxy main.cpp)
target_link_libraries(coroutine-proxy PRIVATE Boost::asio Boost::beast Threads::Threads)
target_compile_definitions(coroutine-proxy PRIVATE BOOST_ASIO_NO_DEPRECATED)
target_compile_options(coroutine-proxy PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)
