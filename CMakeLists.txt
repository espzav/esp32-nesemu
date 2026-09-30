# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
add_compile_options("-Wstringop-truncation")
project(nesemu)

# gcc 15.2.0 / C23 对段属性(section)的检查更严格，会把 LVGL 等第三方组件中
# IRAM_ATTR / 段属性声明误判为 -Werror=attributes 编译错误。这里仅将该条告警
# 降级为警告（保留其余 -Werror），让旧代码在 gcc15 下可继续编译。
# 规范做法：遍历组件名(BUILD_COMPONENTS) -> 取各组件的 COMPONENT_LIB 目标再追加选项。
idf_build_get_property(_comp_names BUILD_COMPONENTS)
foreach(_name ${_comp_names})
    idf_component_get_property(_lib ${_name} COMPONENT_LIB)
    if(_lib AND TARGET ${_lib})
        get_target_property(_type ${_lib} TYPE)
        get_target_property(_imp ${_lib} IMPORTED)
        # INTERFACE / IMPORTED 目标只允许设置 INTERFACE 属性，跳过
        if(NOT _imp AND NOT _type STREQUAL "INTERFACE_LIBRARY")
            target_compile_options(${_lib} PRIVATE -Wno-error=attributes)
        endif()
    endif()
endforeach()
# 保险：LVGL 是已知的 gcc15 重灾区，直接点名其 lib 目标
if(TARGET __idf_lvgl__lvgl)
    target_compile_options(__idf_lvgl__lvgl PRIVATE -Wno-error=attributes)
endif()