/*
** subor_keyboard.c - 小霸王/学习机 Subor 键盘外设的可移植实现
**
** 移植自 fceux-bbk-yuxing/Simulate/KeyBoardSubor.h，剥离 Windows/DirectInput，
** 改用 HID 键盘 Usage ID 作为键位索引，使桌面端和 ESP32(USB HID) 可共用同一逻辑。
**
** 协议简述：
**   写 $4016:
**     0x05 -> 锁存: b_out=FALSE, scan_no=0
**     0x04 -> scan_no=(scan_no+1)%14, b_out=!b_out
**     0x06 -> b_out=!b_out
**   读 $4017: 返回 0xFF，按当前 (scan_no,b_out) 查 14x2x4 矩阵，
**            某键按下则清除对应位 (0x02/0x04/0x08/0x10，低电平有效)。
**   第 10 行保留，恒返回 0xFF。
*/

#include "subor_keyboard.h"
#include "keymap.h"
#include <string.h>

const uint8_t subor_matrix[14][2][4] = {
  /* scan 0 - 未使用 */
  {{0xFF,0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF,0xFF}},
  /* scan 1  : F2,E,5,V | 4,G,F,C */
  {{0x3B,0x08,0x22,0x19}, {0x21,0x0A,0x09,0x06}},
  /* scan 2  : F1,W,3,X | 2,D,S,END */
  {{0x3A,0x1A,0x20,0x1B}, {0x1F,0x07,0x16,0x4D}},
  /* scan 3  : F8,PgUp,Del,Home | Ins,Bksp,PgDn,Right */
  {{0x41,0x4B,0x4C,0x4A}, {0x49,0x2A,0x4E,0x4F}},
  /* scan 4  : F5,O,0,. | 9,I,L,Comma */
  {{0x3E,0x12,0x27,0x37}, {0x26,0x0C,0x0F,0x36}},
  /* scan 5  : F7,[,\,Down | ],RETURN,Up,Left  (NUMPAD ENTER 别名见下) */
  {{0x40,0x2F,0x31,0x51}, {0x30,0x28,0x52,0x50}},
  /* scan 6  : Esc,A,1,LCtrl | Q,Caps,Z,Pause */
  {{0x29,0x04,0x1E,0xE0}, {0x14,0x39,0x1D,0x48}},
  /* scan 7  : F4,U,8,J | 7,Y,K,M */
  {{0x3D,0x18,0x25,0x0D}, {0x24,0x1C,0x0E,0x10}},
  /* scan 8  : F6,P,=,LShift | -,;,',/  (RShift 别名见下) */
  {{0x3F,0x13,0x2E,0xE1}, {0x2D,0x33,0x34,0x38}},
  /* scan 9  : F3,R,6,B | T,H,N,Space */
  {{0x3C,0x15,0x23,0x05}, {0x17,0x0B,0x11,0x2C}},
  /* scan 10 - 保留，恒返回 0xFF */
  {{0xFF,0xFF,0xFF,0xFF},{0xFF,0xFF,0xFF,0xFF}},
  /* scan 11 : F12,Num1,Num2,Num8 | (无),Num4,Num7,F11 */
  {{0x45,0x59,0x5A,0x60}, {0xFF,0x5C,0x5F,0x44}},
  /* scan 12 : F10,Num5,Num/,NumLk | Num-,Num+,Num*,Num9 */
  {{0x43,0x5D,0x54,0x53}, {0x56,0x57,0x55,0x61}},
  /* scan 13 : F9,Num3,Num.,Num0 | `,Num6,Alt,Tab */
  {{0x42,0x5B,0x63,0x62}, {0x35,0x5E,0xE2,0x2B}},
};

const subor_alias_t subor_aliases[] = {
    {0x58, 5, 1, 1},   /* NUMPAD ENTER -> 与 RETURN(0x28) 同格 */
    {0xE5, 8, 0, 3},   /* RIGHT SHIFT  -> 与 LEFT SHIFT(0xE1) 同格 */
};
const int subor_aliases_count = (int)(sizeof(subor_aliases) / sizeof(subor_aliases[0]));

typedef struct {
    uint8_t keys[256];   /* 按 HID 用法码索引的当前按键状态 */
    uint8_t scan_no;     /* 0..13 */
    bool    b_out;
    bool    enabled;
} subor_kb_t;

static subor_kb_t st;

/* Caps Lock 拨动开关的持久状态，供上层（HID LED 控制）查询。 */
static bool caps_lock = false;

bool subor_kb_get_caps_lock(void) { return caps_lock; }

void subor_kb_init(void)
{
    memset(&st, 0, sizeof(st));
    st.enabled = false;
    caps_lock = false;
}

void subor_kb_enable(bool on) { st.enabled = on; }
bool subor_kb_is_enabled(void) { return st.enabled; }

void subor_kb_set_key(uint8_t hid_usage, bool pressed)
{
    /* hid_usage 为 uint8_t，必然落在 0..255 范围内，无需再做上界判断 */
    st.keys[hid_usage] = pressed;
}

void subor_kb_set_report(uint8_t modifier, const uint8_t keycodes[6])
{
    static const uint8_t mod_usage[8] = {0xE0,0xE1,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7};
    static bool caps_key_down = false;

    memset(st.keys, 0, sizeof(st.keys));
    for (int i = 0; i < 8; i++)
        if (modifier & (1u << i))
            st.keys[mod_usage[i]] = true;

    bool caps_in_report = false;
    for (int i = 0; i < 6; i++) {
        uint8_t k = keycodes[i];
        if (k) {   /* k 为 uint8_t，恒 < 256；仅跳过 0(无键) */
            st.keys[k] = true;
            if (k == 0x39)   /* KEY_CAPS_LOCK */
                caps_in_report = true;
        }
    }

    /* USB HID 仅在 Caps Lock 按下当帧报告 0x39（非持久态）。这里把它模拟成真实键盘的
     * “大小写锁定”拨动开关：每次按下沿翻转一次，使大小写状态持续保持。 */
    if (caps_in_report && !caps_key_down) {
        caps_lock = !caps_lock;
    }
    caps_key_down = caps_in_report;

    if (caps_lock) {
        /* 锁定开启时，同时把 Caps 格(scan6/bOut1/bit0x04) 与 Shift 格(scan8) 置位，
         * 让学习机 ROM 以大写方式读取字母键（兼容两种大小写判定实现）。 */
        st.keys[0x39] = true;   /* Caps */
        st.keys[0xE1] = true;   /* LShift -> 大写 */
    }
}

void subor_kb_write(uint16_t addr, uint8_t val)
{
    if (!st.enabled)
        return;
    if (addr != 0x4016)
        return;

    if (val == 0x05) {
        st.b_out = false;
        st.scan_no = 0;
    } else if (val == 0x04) {
        if (++st.scan_no > 13)
            st.scan_no = 0;
        st.b_out = !st.b_out;
    } else if (val == 0x06) {
        st.b_out = !st.b_out;
    }
}

static bool cell_pressed(int sn, int bo, int bit)
{
    uint8_t hid = subor_matrix[sn][bo][bit];
    if (hid != 0xFF && st.keys[hid])
        return true;
    for (int i = 0; i < subor_aliases_count; i++) {
        const subor_alias_t *a = &subor_aliases[i];
        if (a->sn == sn && a->bo == bo && a->bit == bit && st.keys[a->hid])
            return true;
    }
    return false;
}

uint8_t subor_kb_read(uint16_t addr)
{
    if (!st.enabled)
        return 0xFF;
    if (addr != 0x4017)
        return 0xFF;

    uint8_t data = 0xFF;
    int sn = st.scan_no > 13 ? 13 : st.scan_no;
    int bo = st.b_out ? 1 : 0;

    if (cell_pressed(sn, bo, 0)) data &= (uint8_t)~0x02;
    if (cell_pressed(sn, bo, 1)) data &= (uint8_t)~0x04;
    if (cell_pressed(sn, bo, 2)) data &= (uint8_t)~0x08;
    if (cell_pressed(sn, bo, 3)) data &= (uint8_t)~0x10;

    return data;
}
