# 模块 executor：按目标列表执行请求、重试规则表（任务 1.7）
llmgw_add_module(executor SOURCES executor.cpp classify.cpp
                 DEPS core config observability upstream provider)
