# 模块 stream：SSE 解析、usage 提取、ResponseStream（任务 1.5）
llmgw_add_module(stream
  SOURCES sse_parser.cpp inspect.cpp stream.cpp
  DEPS core observability)

llmgw_fetch_dependency(simdjson)
target_link_libraries(llmgw_stream PRIVATE simdjson::simdjson)
