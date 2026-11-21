# 模块 server：目前还没有源文件，依赖关系已经由 llmgw_add_module 校验
llmgw_add_module(server DEPS core config observability middleware router executor)
