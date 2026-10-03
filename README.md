# ESP32-NESEMU, a Nintendo Entertainment System emulator for the ESP32 (IDF v5)
====================================================================

This is a quick and dirty port of Nofrendo, a Nintendo Entertainment System emulator. It lacks sound, but can emulate a NES at close to full speed, albeit with some framedrop due to the way the display is driven. 

The portation of NES emulator is based on [ESP-BSP](https://github.com/espressif/esp-bsp) code and it allows to change the board quickly. 

|  Selection of the game  | Super Mario | Donkey Kong | qBert |
| :----------: | :-----------: | :----------: | :----------: |
| <img src="doc/images/gameselection.jpg"> | <img src="doc/images/supermario.jpg"> | <img src="doc/images/donkeykong.jpg"> | <img src="doc/images/qbert.jpg"> |

## Warning
-------

This is a proof-of-concept (only for demo) and not an official application note. As such, this code is entirely unsupported by Espressif.

## USB HID Controller
---------

The games are controlled by USB HID SNES GamePad. 

<img src="doc/images/controller.jpg">

## Build and flash
---------

```
idf.py -p COMx flash monitor
```

## Board selection
---------

For change the board, please edit the [`components/esp_nes/idf_component.yml`](components/esp_nes/idf_component.yml) file and change these lines to another BSP component:

```
ws_7inch:
    version: "bsp/7inch_update"
    path: "SquareLine/boards/custom_waveshare_7inch/components/ws_7inch/"
    git: https://github.com/espressif/esp-bsp.git
```

For example, you can use this for ESP-BOX:

```
esp-box: "*"
```

## ROM
---
This NES emulator does not come with a ROM. This demo allows to select between three games, each game must be flashed to right flash region:

1. Game: `0x00100000`
2. Game: `0x00200000`
3. Game: `0x00300000`

You can use this command for flash the game into right flash region:

```
python esptool.py --chip esp32s3 --port "COM3" --baud $((230400*4)) write_flash -fs 4MB 0x100000 .\games\supermario.nes
```

## Copyright
---------

Code in this repository is Copyright (C) 2016 Espressif Systems, licensed under the Apache License 2.0 as described in the file LICENSE. Code in the
components/nofrendo is Copyright (c) 1998-2000 Matthew Conte (matt@conte.com) and licensed under the GPLv2.

## 本分支改动（2026 自研 fork）
本分支面向 **ESP32-S31-Korvo-1（RISC-V preview）** 开发板做移植与新功能，与上游 WROVER/DevKit 方向不同。以下为相对上游 `2f7e166` 的改动。

### 硬件与移植
- 移植到 ESP32-S31-Korvo-1（RISC-V 内核 preview），引入 `components/esp32_s31_korvo_1` BSP 组件。
- 调整 `sdkconfig.defaults` / `partitions.csv` / `main/CMakeLists.txt` 适配新硬件。

### 开机与选单（显示层）
- LVGL 选单开机即显示；8 秒倒计时自动启动，任一按钮点击即取消。
- 自动选游戏优先级：SD 卡含 "english" 的 ROM > 第一个 `.nes` > 内置分区；支持随机候选。
- 开机 UART 嗅探：SD 挂载即在后台监听串口，收到 PC 指令即收 ROM，无需手动点按钮。

### 串口传 ROM（免烧录）
- 独立任务接收 UART 数据，不卡 UI；带互斥锁防止嗅探/按钮两条路径抢数据；屏幕显示上传进度。
- PC 端工具：`tools/uart_send_rom.ps1`。
- 接收协议：`"ROM名 大小" -> READY -> 原始字节 -> OK`，写入 `/sdcard/ROM/`，并校验文件名。

### 输入
- 新增 Subor 小霸王键盘组件（`components/subor_keyboard/`）。

### 游戏兼容性
- nofrendo：新增 mapper 167，修复 `nes_ppu.c` / `nes_rom.c`。
- 测试 ROM 见 `roms/`（注意版权，见下）。

### 许可证状态与注意事项
- `components/nofrendo`：**源码头声明为 LGPL v2**（GNU Library General Public License），但本仓库早期 README 第 67 行误写为 "GPLv2"，二者存在冲突。以源码头为准按 LGPL v2 处理；为稳妥，分发固件时建议整体保持开源、源码可获取。
- `components/esp32_s31_korvo_1`：**Apache-2.0**（乐鑫官方 BSP）。
- `components/esp_nes/osd.c`（显示层）：**CC0-1.0**。
- 其余自研组件（如 `components/subor_keyboard`）未显式声明协议，建议补充。
- 本仓库根目录目前无统一 LICENSE 文件，建议补充。

### ROM 版权
仓库内 `roms/*.nes` 为第三方游戏 ROM，**受版权保护**，仅可用于个人学习/测试；**不要**随开源项目公开发布，也**不要**纳入向上游的 PR。

