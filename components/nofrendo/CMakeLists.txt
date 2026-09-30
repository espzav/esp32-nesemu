file(GLOB_RECURSE T_SOURCES *.c)
file(GLOB_RECURSE CPU_SOURCES cpu/*.c)
file(GLOB_RECURSE NSS_SOURCES libsnss/*.c)
file(GLOB_RECURSE NES_SOURCES nes/*.c)
file(GLOB_RECURSE SNDHRDW_SOURCES sndhrdw/*.c)
file(GLOB_RECURSE MAP_SOURCES mappers/*.c)

idf_component_register(SRCS ${T_SOURCES} ${CPU_SOURCES} ${NSS_SOURCES} ${NES_SOURCES} ${SNDHRDW_SOURCES} ${MAP_SOURCES} INCLUDE_DIRS "cpu" "libsnss" "nes" "sndhrdw" "." REQUIRES subor_keyboard)

# nofrendo 是 1998 年的第三方代码，用 gcc15/C23 编译会产生大量历史告警
# (如单字节字段上的 __packed 被忽略、-Werror=attributes 等)。
# 仅对本组件关闭“告警即错误”，保留真正的编译错误以暴露真实问题。
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error)
