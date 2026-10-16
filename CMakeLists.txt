# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

# 本地测试沿用当前正式版号；仅正式发布或明确的 OTA 验证指定新版本。
# Local tests keep the current release version; override only for publication or explicit OTA validation.
if(DEFINED ENV{READLL_PROJECT_VER} AND NOT "$ENV{READLL_PROJECT_VER}" STREQUAL "")
    set(PROJECT_VER "$ENV{READLL_PROJECT_VER}")
else()
    set(PROJECT_VER "0.3.3-rc95")
endif()
project(readll)

# 每次实际构建检查上游，不自动更改源码；离线跳过必须显式指定。
# Check upstream on every build without changing sources; offline skipping is explicit.
option(READLL_SKIP_UPSTREAM_CHECK "Explicitly skip the online upstream check" OFF)
option(READLL_REQUIRE_CURRENT_UPSTREAM "Reject unreviewed upstream changes" OFF)
if(READLL_SKIP_UPSTREAM_CHECK AND READLL_REQUIRE_CURRENT_UPSTREAM)
    message(FATAL_ERROR "Required upstream review cannot be skipped")
endif()
if(NOT READLL_SKIP_UPSTREAM_CHECK)
    set(upstream_check_args "")
    if(READLL_REQUIRE_CURRENT_UPSTREAM)
        list(APPEND upstream_check_args --require-current)
    endif()
    add_custom_target(readll_upstream_check ALL
        COMMAND "${PYTHON}" "${CMAKE_SOURCE_DIR}/tools/check_upstream.py" ${upstream_check_args}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        VERBATIM)
    add_dependencies(${PROJECT_NAME}.elf readll_upstream_check)
endif()

# 固件描述时间统一为 UTC，与概览页标识一致；保留已有编译缓存启动器。
# Compile app-description timestamps in UTC to match Overview; preserve any compiler-cache launcher.
idf_component_get_property(app_desc_lib esp_app_format COMPONENT_LIB)
get_target_property(app_desc_launcher ${app_desc_lib} C_COMPILER_LAUNCHER)
if(NOT app_desc_launcher)
    set(app_desc_launcher "")
endif()
set(app_desc_utc_launcher "${CMAKE_COMMAND}" -E env TZ=UTC)
if(app_desc_launcher)
    list(APPEND app_desc_utc_launcher ${app_desc_launcher})
endif()
set_property(TARGET ${app_desc_lib} PROPERTY C_COMPILER_LAUNCHER "${app_desc_utc_launcher}")

# panic 包装器的链接选项在 main/CMakeLists.txt 里挂到 main 组件上：写在 project() 之后
# 用 idf_build_set_property 设全局链接选项已经太晚，链接器看不到 --wrap。
# The panic wrapper's link option hangs off the main component in main/CMakeLists.txt: setting a
# global link option after project() is too late for the linker to see --wrap.
