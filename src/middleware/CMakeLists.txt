# 模块 middleware：中间件接口、插件注册表、中间件链（任务 2.1）；内置插件在 2.2–2.4 中加入
llmgw_add_module(middleware
  SOURCES registry.cpp chain.cpp
  DEPS core config)
