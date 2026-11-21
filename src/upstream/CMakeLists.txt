# 模块 upstream：到上游的 HTTP/1.1 客户端（任务 1.4）
llmgw_add_module(upstream
  SOURCES client.cpp connection.cpp dns.cpp establish.cpp message.cpp pool.cpp tls.cpp
  DEPS core config)

find_package(Threads REQUIRED)
find_package(OpenSSL 3 REQUIRED)
llmgw_fetch_dependency(boost)
target_link_libraries(llmgw_upstream
  PUBLIC Boost::asio
  PRIVATE Boost::beast OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
