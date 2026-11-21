# 模块 server：接入层（任务 1.3）
llmgw_add_module(server
  SOURCES server.cpp worker.cpp session.cpp relay.cpp control_plane.cpp http_util.cpp
  DEPS core config)

find_package(Threads REQUIRED)
llmgw_fetch_dependency(boost)
target_link_libraries(llmgw_server PUBLIC Boost::asio PRIVATE Boost::beast Threads::Threads)
target_compile_definitions(llmgw_server PRIVATE BOOST_ASIO_NO_DEPRECATED)
# simdjson：热路径上从请求体中取 model 和 stream（任务 1.3 S20）
llmgw_fetch_dependency(simdjson)
target_link_libraries(llmgw_server PRIVATE simdjson::simdjson)
# nlohmann/json：构造错误响应体和模型列表（不在热路径上），只在 .cpp 中使用
llmgw_fetch_dependency(nlohmann_json)
target_include_directories(llmgw_server SYSTEM PRIVATE "${nlohmann_json_SOURCE_DIR}/single_include")
