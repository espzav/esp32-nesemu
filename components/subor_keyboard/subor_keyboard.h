#ifndef SUBOR_KEYBOARD_H
#define SUBOR_KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Subor / 小霸王学习机键盘外设的可移植实现。
 *
 * 学习机键盘接在 2P 手柄口，ROM 通过 $4016(写控制)/$4017(读数据) 以
 * 14 行 x 2 半 x 4 位的串行矩阵（低电平有效）读取按键。
 *
 * 本实现与平台无关：按键状态通过 HID 键盘 Usage ID（page 0x07）喂入，
 * 因此桌面端（SDL/Win32 键码转换后）与 ESP32（USB HID BOOT 报文）均可复用。
 *
 * 移植自 fceux-bbk-yuxing/Simulate/KeyBoardSubor.h，剥离了 Windows/DirectInput。
 */

void subor_kb_init(void);
void subor_kb_enable(bool on);
bool subor_kb_is_enabled(void);

/* 挂到 CPU 内存映射：$4016 写 / $4017 读。未启用时 write 忽略、read 返回 0xFF。 */
void subor_kb_write(uint16_t addr, uint8_t val);
uint8_t subor_kb_read(uint16_t addr);

/* 事件式（桌面端）：按 HID 键盘 Usage ID 设置单个键。 */
void subor_kb_set_key(uint8_t hid_usage, bool pressed);

/* USB HID BOOT 键盘报文：修饰键字节 + 最多 6 个 Usage ID。 */
void subor_kb_set_report(uint8_t modifier, const uint8_t keycodes[6]);

/* 返回 Caps Lock 当前锁定状态（供 HID 输出报文点亮键盘 LED）。 */
bool subor_kb_get_caps_lock(void);

#ifdef __cplusplus
}
#endif

#endif /* SUBOR_KEYBOARD_H */
