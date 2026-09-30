file(GLOB_RECURSE SRC src/*.c)
# NES 模拟器不需要摄像头，排除依赖 esp_video 的 bsp_camera.c 以避免引入超深路径组件
list(FILTER SRC EXCLUDE REGEX "src/bsp_camera\\.c$")
# 注：bsp_storage.c 此前因 S3 目标 GPIO 枚举缺失被排除；本工程目标为 esp32s31
# (RISC-V preview)，其 gpio_num_t 含 GPIO_NUM_20..25，SD 卡功能可正常编译/使用，
# 故不再排除。

set(REQ esp_driver_gpio esp_driver_i2c esp_driver_i2s spiffs fatfs vfs esp_driver_sdspi esp_driver_sdmmc )
set(PRIV_REQ esp_lcd esp_driver_ledc)

idf_component_register(
    SRCS ${SRC}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES ${REQ}
    PRIV_REQUIRES esp_psram ${PRIV_REQ}
)

# Force-link the SD host drivers. IDF places these IDF-builtin libs *before* the
# link group while this BSP (which references sdmmc_host_*/sdspi_host_*) sits inside
# the group, so in a single-pass link their objects are never pulled. Re-referencing
# them here by full relative path makes CMake append them *after* this BSP (still
# inside the group), so the references resolve. Paths are relative to CMAKE_BINARY_DIR.
# Force-link the SD host drivers and their transitive deps. IDF links these
# IDF-builtin libs *before* the link group while this BSP's bsp_storage.c (referencing
# sdmmc_host_*/sdspi_host_*, spi_device_*, esp_vfs_fat_sdmmc_mount, sdmmc_card_*) is
# only pulled in from inside the group (by esp_nes). In a single-pass link the SD
# objects are never pulled, causing undefined references. Wrapping them in
# --whole-archive (single string so CMake does not split the flags from the libs)
# guarantees every object is included regardless of link order. These libs are not
# referenced before the group, so whole-archive here does not cause duplicate symbols.
target_link_libraries(${COMPONENT_TARGET} INTERFACE
    "-Wl,--allow-multiple-definition -Wl,--whole-archive ${CMAKE_BINARY_DIR}/esp-idf/esp_driver_sdmmc/libesp_driver_sdmmc.a ${CMAKE_BINARY_DIR}/esp-idf/esp_driver_sdspi/libesp_driver_sdspi.a ${CMAKE_BINARY_DIR}/esp-idf/esp_driver_spi/libesp_driver_spi.a ${CMAKE_BINARY_DIR}/esp-idf/esp_driver_sd_intf/libesp_driver_sd_intf.a ${CMAKE_BINARY_DIR}/esp-idf/sdmmc/libsdmmc.a ${CMAKE_BINARY_DIR}/esp-idf/esp_hal_sd/libesp_hal_sd.a ${CMAKE_BINARY_DIR}/esp-idf/esp_blockdev_util/libesp_blockdev_util.a ${CMAKE_BINARY_DIR}/esp-idf/fatfs/libfatfs.a -Wl,--no-whole-archive")
