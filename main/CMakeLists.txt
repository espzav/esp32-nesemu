idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_nes esp32_s31_korvo_1 nofrendo usb)

# esp_nes <-> nofrendo form a circular link dependency (esp_nes references
# nofrendo's emulation core; nofrendo references osd_main() in esp_nes/osd.c).
# On top of that, esp_nes (osd.c) references many other libraries
# (hid host, LVGL canvas, the BSP, the USB HID stack, subor_keyboard, ...).
# A plain linear -l order cannot satisfy these; wrap every library esp_nes /
# nofrendo depends on in a single linker group so the linker re-scans them
# until every symbol is resolved.
# We reference the libraries by their literal file path ($<TARGET_FILE:...>)
# because the idf::<comp> aliases are already pulled in via REQUIRES and would
# otherwise be de-duplicated out of the group.
target_link_libraries(${COMPONENT_LIB} PUBLIC
    -Wl,--start-group
    "$<TARGET_FILE:idf::nofrendo>"
    "$<TARGET_FILE:idf::esp_nes>"
    "$<TARGET_FILE:idf::hid>"
    "$<TARGET_FILE:idf::lvgl__lvgl>"
    "$<TARGET_FILE:idf::espressif__usb>"
    "$<TARGET_FILE:idf::subor_keyboard>"
    "$<TARGET_FILE:idf::esp32_s31_korvo_1>"
    "$<TARGET_FILE:idf::esp_lcd>"
    "$<TARGET_FILE:idf::driver>"
    "$<TARGET_FILE:lvgl_port_lib>"
    -Wl,--end-group)
