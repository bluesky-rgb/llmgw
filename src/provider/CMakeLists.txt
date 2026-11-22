# 模块 provider：OpenAI 兼容适配、请求改写（任务 1.6）
llmgw_add_module(provider
  SOURCES provider.cpp rewrite.cpp
  DEPS core config observability upstream stream)

llmgw_fetch_dependency(simdjson)
target_link_libraries(llmgw_provider PRIVATE simdjson::simdjson)
