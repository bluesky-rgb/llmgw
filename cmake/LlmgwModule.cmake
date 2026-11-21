# llmgw_add_module(<name> SOURCES <files...> [DEPS <modules...>])
#
# 每个模块都用这个函数创建，统一约定：
#   - 目标名 llmgw_<name>，别名 llmgw::<name>
#   - 公开头文件放在 src/<name>/include/llmgw/<name>/，只有它们对其他模块可见
#   - 其余头文件只在模块内部可见（PRIVATE），别的模块 #include 不到
#   - DEPS 必须是 tools/module_deps.json 中允许的依赖，否则 CMake 配置阶段直接报错
#   - 每个公开头文件都必须能单独编译（不依赖包含顺序）：为每个头文件生成一个只包含它的
#     .cpp，以 -Werror 编译进 llmgw_<name>_header_check。新增头文件需要重新运行 CMake 配置
include_guard(GLOBAL)

file(READ "${CMAKE_SOURCE_DIR}/tools/module_deps.json" LLMGW_MODULE_RULES)

function(llmgw_add_module name)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})

  string(JSON allowed_json ERROR_VARIABLE err GET "${LLMGW_MODULE_RULES}" modules ${name})
  if(err)
    message(FATAL_ERROR "模块 ${name} 没有在 tools/module_deps.json 中登记")
  endif()
  string(JSON n LENGTH "${allowed_json}")
  set(allowed "")
  if(n GREATER 0)
    math(EXPR last "${n} - 1")
    foreach(i RANGE ${last})
      string(JSON dep GET "${allowed_json}" ${i})
      list(APPEND allowed ${dep})
    endforeach()
  endif()
  foreach(dep IN LISTS ARG_DEPS)
    if(NOT dep IN_LIST allowed)
      message(FATAL_ERROR "模块 ${name} 不允许依赖 ${dep}。允许的依赖：${allowed}")
    endif()
  endforeach()

  set(dir "${CMAKE_SOURCE_DIR}/src/${name}")
  if(ARG_SOURCES)
    add_library(llmgw_${name} STATIC ${ARG_SOURCES})
    target_include_directories(llmgw_${name} PUBLIC "${dir}/include" PRIVATE "${dir}")
    set(scope PUBLIC)
  else()
    add_library(llmgw_${name} INTERFACE)   # 只有头文件的模块，比如 core 的早期阶段
    target_include_directories(llmgw_${name} INTERFACE "${dir}/include")
    set(scope INTERFACE)
  endif()
  add_library(llmgw::${name} ALIAS llmgw_${name})
  foreach(dep IN LISTS ARG_DEPS)
    target_link_libraries(llmgw_${name} ${scope} llmgw::${dep})
  endforeach()
  llmgw_add_header_check(${name})
  if(ARG_SOURCES)
    target_compile_options(llmgw_${name} PRIVATE
      $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
      $<$<AND:$<BOOL:${LLMGW_WARNINGS_AS_ERRORS}>,$<CXX_COMPILER_ID:GNU,Clang>>:-Werror>)
  endif()
endfunction()

# 为模块的每个公开头文件生成一个只包含它的 .cpp，验证头文件能单独编译。
function(llmgw_add_header_check name)
  set(dir "${CMAKE_SOURCE_DIR}/src/${name}/include")
  file(GLOB_RECURSE headers CONFIGURE_DEPENDS RELATIVE "${dir}" "${dir}/*.h")
  if(NOT headers)
    return()
  endif()
  set(sources "")
  foreach(h IN LISTS headers)
    string(MAKE_C_IDENTIFIER "${h}" id)
    set(f "${CMAKE_CURRENT_BINARY_DIR}/header_check/${id}.cpp")
    set(content "// 自动生成：验证 ${h} 能单独编译\n#include <${h}>\n")
    if(EXISTS "${f}")
      file(READ "${f}" old)
    else()
      set(old "")
    endif()
    if(NOT old STREQUAL content)
      file(WRITE "${f}" "${content}")
    endif()
    list(APPEND sources "${f}")
  endforeach()
  add_library(llmgw_${name}_header_check OBJECT ${sources})
  target_link_libraries(llmgw_${name}_header_check PRIVATE llmgw::${name})
  target_compile_options(llmgw_${name}_header_check PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<AND:$<BOOL:${LLMGW_WARNINGS_AS_ERRORS}>,$<CXX_COMPILER_ID:GNU,Clang>>:-Werror>)
endfunction()
