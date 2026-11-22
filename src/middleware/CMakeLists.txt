# 模块 middleware：中间件接口、插件注册表、中间件链（任务 2.1）；内置插件 auth（2.2），
# ratelimit、budget 在 2.3、2.4 中加入
llmgw_add_module(middleware
  SOURCES registry.cpp chain.cpp auth.cpp
  DEPS core config)

# auth：SHA-256、随机数、常量时间比较。OpenSSL 已经是 upstream 的依赖（TLS），不新增第三方依赖
find_package(OpenSSL 3 REQUIRED)
target_link_libraries(llmgw_middleware PRIVATE OpenSSL::Crypto)
# nlohmann/json：解析插件自己的配置（只在启动时），只在 .cpp 中使用
llmgw_fetch_dependency(nlohmann_json)
target_include_directories(llmgw_middleware SYSTEM PRIVATE "${nlohmann_json_SOURCE_DIR}/single_include")
