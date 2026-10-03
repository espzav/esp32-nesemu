#ifndef SUBOR_KEYMAP_H
#define SUBOR_KEYMAP_H

#include <stdint.h>

/* subor_matrix[scan_no][b_out][bit]
 *   scan_no : 0..13   由 $4016 写操作选择的行
 *   b_out   : 0 = 前半, 1 = 后半
 *   bit     : 0..3  -> 串行位 0x02, 0x04, 0x08, 0x10（低电平有效）
 * 每个单元存放一个 HID 键盘 Usage ID（page 0x07）；0xFF 表示无键。
 *
 * 完整移植自 fceux-bbk-yuxing/Simulate/KeyBoardSubor.h，
 * 原 DIK_* 扫描码已替换为对应的 HID 用法码（例如 DIK_A->0x04, DIK_LCONTROL->0xE0,
 * DIK_NUMPAD1->0x59, DIK_F2->0x3B）。第 10 行（scan_no=10）保留，恒返回 0xFF。
 *
 * 常用 HID 用法码速查（page 0x07）：
 *   a=0x04 b=0x05 ... z=0x1D ; 1=0x1E ... 0=0x27
 *   Enter=0x28 Esc=0x29 Bksp=0x2A Tab=0x2B Space=0x2C
 *   -=0x2D ==0x2E [=0x2F ]=0x30 \=0x31 ;=0x33 '=0x34 `=0x35
 *   ,=0x36 .=0x37 /=0x38 Caps=0x39
 *   F1=0x3A ... F12=0x45 ; Home=0x4A End=0x4D L=0x50 R=0x4F U=0x52 D=0x51
 *   NumLk=0x53 NUM(/=0x54 *=0x55 -=0x56 +=0x57 Enter=0x58
 *   1=0x59 2=0x5A 3=0x5B 4=0x5C 5=0x5D 6=0x5E 7=0x5F 8=0x60 9=0x61 0=0x62 .=0x63
 *   修饰键：L Ctrl=0xE0 L Shift=0xE1 L Alt=0xE2 R Shift=0xE5
 */
extern const uint8_t subor_matrix[14][2][4];

typedef struct {
    uint8_t hid;   /* 映射到同一单元的另一 HID 用法码 */
    uint8_t sn;    /* scan_no */
    uint8_t bo;    /* b_out (0/1) */
    uint8_t bit;   /* bit 索引 0..3 */
} subor_alias_t;

/* 原实现中多个键 OR 进同一单元的别名（例如 RETURN 与 NUMPAD ENTER 共用一格）。 */
extern const subor_alias_t subor_aliases[];
extern const int subor_aliases_count;

#endif /* SUBOR_KEYMAP_H */
