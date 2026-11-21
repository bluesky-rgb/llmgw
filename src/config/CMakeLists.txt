# 模块 config：配置的加载与校验（任务 1.1）
llmgw_add_module(config
  SOURCES loader.cpp sections.cpp json_reader.cpp syntax.cpp
  DEPS core)

# nlohmann/json 只在本模块的 .cpp 中使用，不出现在公开头文件中（code-architecture.md 3.7）
llmgw_fetch_dependency(nlohmann_json)
target_include_directories(llmgw_config SYSTEM PRIVATE "${nlohmann_json_SOURCE_DIR}/single_include")
