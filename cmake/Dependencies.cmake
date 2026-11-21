# 按 cmake/dependencies.json 声明第三方依赖。
#
#   llmgw_fetch_dependency(<名称>)
#
# 版本只写在 dependencies.json 一个地方。纯头文件库只下载源码、不执行它自带的
# CMakeLists（避免引入额外的选项和目标），源码目录通过 <名称>_SOURCE_DIR 使用。
#
# 离线构建：-DFETCHCONTENT_SOURCE_DIR_<名称大写>=/本地/源码目录
include_guard(GLOBAL)
include(FetchContent)

file(READ "${CMAKE_SOURCE_DIR}/cmake/dependencies.json" LLMGW_DEPENDENCIES_JSON)

function(llmgw_fetch_dependency name)
  string(JSON repo ERROR_VARIABLE err GET "${LLMGW_DEPENDENCIES_JSON}" dependencies ${name} repo)
  if(err)
    message(FATAL_ERROR "依赖 ${name} 没有在 cmake/dependencies.json 中登记")
  endif()
  string(JSON tag GET "${LLMGW_DEPENDENCIES_JSON}" dependencies ${name} tag)
  string(JSON header_only GET "${LLMGW_DEPENDENCIES_JSON}" dependencies ${name} header_only)
  string(JSON url ERROR_VARIABLE no_url GET "${LLMGW_DEPENDENCIES_JSON}" dependencies ${name} url)

  if(NOT no_url)
    # 按 URL 下载发布包，并校验 SHA-256，防止下载内容被篡改
    string(JSON sha256 GET "${LLMGW_DEPENDENCIES_JSON}" dependencies ${name} sha256)
    string(JSON libs ERROR_VARIABLE no_libs GET "${LLMGW_DEPENDENCIES_JSON}" dependencies ${name} libraries)
    if(NOT no_libs AND name STREQUAL "boost")
      # 只配置需要的 Boost 库（Asio、Beast 都是纯头文件库）
      string(JSON n LENGTH "${libs}")
      math(EXPR last "${n} - 1")
      set(boost_libs "")
      foreach(i RANGE ${last})
        string(JSON lib GET "${libs}" ${i})
        list(APPEND boost_libs ${lib})
      endforeach()
      set(BOOST_INCLUDE_LIBRARIES ${boost_libs} CACHE STRING "" FORCE)
      set(BOOST_ENABLE_CMAKE ON CACHE BOOL "" FORCE)
    endif()
    FetchContent_Declare(${name}
      URL      ${url}
      URL_HASH SHA256=${sha256}
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  elseif(header_only)
    FetchContent_Declare(${name}
      GIT_REPOSITORY ${repo}
      GIT_TAG        ${tag}
      GIT_SHALLOW    TRUE
      SOURCE_SUBDIR  _llmgw_header_only_do_not_add_subdirectory)
  else()
    FetchContent_Declare(${name}
      GIT_REPOSITORY ${repo}
      GIT_TAG        ${tag}
      GIT_SHALLOW    TRUE)
  endif()
  FetchContent_MakeAvailable(${name})
  set(${name}_SOURCE_DIR "${${name}_SOURCE_DIR}" PARENT_SCOPE)
endfunction()
