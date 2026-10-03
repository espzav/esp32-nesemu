/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

/*******************************************************************************
* Includes
*******************************************************************************/

#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <ctype.h>

#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_partition.h"
#include "spi_flash_mmap.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/uart.h"

static const char *TAG = "OSD";

#define LOBYTE(u16)     ((uint8_t)(((uint16_t)(u16)) & 0xff))
#define HIBYTE(u16)     ((uint8_t)((((uint16_t)(u16))>>8) & 0xff))

//Nes stuff wants to define this as well...
#undef false
#undef true
#undef bool

#include "esp_log.h"
#include "noftypes.h"
#include "nofconfig.h"
#include "nofrendo.h"
#include "bitmap.h"
#include "osd.h"
#include "event.h"
#include "nesinput.h"
#include "bsp/esp-bsp.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "usb/hid_usage_mouse.h"
#include "subor_keyboard.h"   /* 小霸王/学习机 Subor 键盘 */

#define APP_DEFAULT_WIDTH  256
#define APP_DEFAULT_HEIGHT 240
#define APP_DEFAULT_SCALE  (BSP_LCD_V_RES/APP_DEFAULT_HEIGHT)

typedef struct {
    hid_host_device_handle_t hid_device_handle;
    hid_host_driver_event_t event;
    void *arg;
} app_usb_hid_event_t;

/* 当前已连接的 USB 键盘句柄（NULL 表示未连接）；用于把 BOOT 键盘报文
 * 正确路由到 Subor 学习机键盘矩阵，而不是误当作手柄。 */
static hid_host_device_handle_t s_keyboard_handle = NULL;

typedef struct
{
    bool    btn_select:1;
    bool    btn_start:1;
    bool    btn_up:1;
    bool    btn_down:1;
    bool    btn_right:1;
    bool    btn_left:1;

    bool    btn_a:1;
    bool    btn_b:1;
} gamepad_t;

/*******************************************************************************
* Function definitions
*******************************************************************************/
/* Video functions */
static int app_osd_video_init(int width, int height);
static void app_osd_video_shutdown(void);
static int app_osd_video_set_mode(int width, int height);
static void app_osd_video_set_palette(rgb_t *palette);
static void app_osd_video_clear(uint8 color);
static bitmap_t *app_osd_video_lock_write(void);
static void app_osd_video_free_write(int num_dirties, rect_t *dirty_rects);
static void app_osd_video_custom_blit(bitmap_t *primary, int num_dirties, rect_t *dirty_rects);
/* Audio functions */

/* USB functions */
static void app_hid_init(void);

/* Game selection / auto start */
static void app_lvgl_game_selection(void);
static bool app_auto_select_game(void);
static bool app_random_pick_game(void);
static bool app_uart_rom_upload(uint32_t first_idle_ms);
static void app_uart_wait_progress(const char *name, size_t got, size_t total);
static bool s_uart_wait_transferring = false;
static void app_autostart_begin(void);
static void app_autostart_cancel(void);
static void app_menu_destroy(void);
/* 串口上传在独立任务里跑（阻塞读 UART），避免冻结 LVGL 任务使界面卡住。
 * auto_sniff=true 为开机自动嗅探(短超时)，false 为按钮手动触发(长超时)。 */
static void app_uart_upload_task(void *arg);
/*******************************************************************************
* Local variables
*******************************************************************************/

static viddriver_t vid_driver = {
   .name = "Vid Driver",
   .init = app_osd_video_init,
   .shutdown = app_osd_video_shutdown,
   .set_mode = app_osd_video_set_mode,
   .set_palette = app_osd_video_set_palette,
   .clear = app_osd_video_clear,
   .lock_write = app_osd_video_lock_write,
   .free_write = app_osd_video_free_write,
   .custom_blit = app_osd_video_custom_blit,
   .invalidate = false,
};

static TimerHandle_t timer;

/* Video variables */
static uint16 colorPalette[256];
static bitmap_t *vidBitmap = NULL;
static lv_obj_t * lvgl_video_canvas = NULL;
static uint8_t * vid_buffer = NULL;

/* USB variables */
static QueueHandle_t hid_queue = NULL;
static gamepad_t app_gamepad;
static gamepad_t app_gamepad_tmp;
/* Joypad state split by input source so keyboard and a physical gamepad can
** be used together: gp_pad = USB gamepad reports, kb_pad = USB keyboard. */
static gamepad_t gp_pad;
static gamepad_t kb_pad;
/* Tracks the last Caps Lock LED state sent to the keyboard. */
static bool s_caps_led = false;

/* GAME */
/* 三个 ROM 分区（裸 .nes 直接烧录于此），启动界面动态扫描、仅显示含有效 NES 头的 ROM。
 * SD 卡中的 .nes 文件优先；分区作为回退。 */
#define MAX_GAMES 3
static const char * GAME_PARTS[MAX_GAMES] = {"nesrom1", "nesrom2", "nesrom3"};
/* 当 ROM 头内 16 字节名称字段为空/不可打印时使用的回退名。 */
static const char * GAME_FALLBACK[MAX_GAMES] = {"English Word Blaster", "Contra 2", "Game 3"};
/* Selected game: either an SD card path ("/sdcard/xxx.nes") or a flash partition label. */
static const char * app_selected_game = NULL;
static lv_obj_t * game_selection = NULL;
/* SD card mount state and the path buffer used for the currently selected SD ROM. */
static bool s_sd_mounted = false;
static char s_sel_path[256];
/* ROM 统一放在 SD 卡的 ROM 子目录下（/sdcard/ROM/），不占 SD 卡根目录。 */
#define ROM_DIR         BSP_SD_MOUNT_POINT "/ROM"
static void app_rom_dir_ensure(void);

/* 开机选单 + 倒计时自动启动：若用户不点任何按钮，AUTOSTART_SEC 秒后自动启动
** app_auto_select_game() 选中的游戏（SD-English 优先）。任一按钮点击都会取消倒计时。
** 选单上还提供"串口收 ROM"按钮，可在不擦除分区的前提下把 .nes 存进 /sdcard/ROM/。 */
#define AUTOSTART_SEC   8
static const char *s_auto_pick = NULL;   /* 自动启动候选：SD 路径(s_auto_path) 或分区标签 */
static char s_auto_path[256];            /* s_auto_pick 指向的 SD 路径缓冲 */
static lv_timer_t *s_autostart_timer = NULL;
static lv_obj_t *s_autostart_label = NULL;
static lv_obj_t *s_uart_btn = NULL;   /* 挂在屏幕根上的 UART ROM 按钮 */
static int s_autostart_left = 0;
/* 串口接收互斥：同一时刻只允许一个任务读 UART0 RX（开机嗅探 or 按钮触发），避免抢数据。 */
static volatile bool s_uart_rx_busy = false;
/* 用户已通过菜单选定游戏（点了某个游戏按钮）：嗅探超时后不再自动启动，直接进游戏。 */
static volatile bool s_user_picked_game = false;

/* 串口 ROM 上传参数（定义提前，供上传任务与接收函数共用）。 */
#define ROM_UPLOAD_BAUD     115200
#define ROM_UPLOAD_CHUNK    4096
#define ROM_UPLOAD_IDLE_MS  30000
/* 开机自动进入串口接收等待的时长：此窗口内收到 ROM 命令则上传，否则超时自动启动游戏。 */
#define BOOT_UART_WAIT_MS   8000
/* 已有游戏时的开机嗅探窗口：只给一个"热更新"机会，短一点以免开机白等太久。 */
#define BOOT_UART_HOT_WINDOW_MS  3000

/* Base pointer + ownership flag for the ROM buffer returned by osd_getromdata().
** nofrendo skips the 16-byte header before storing rominfo->rom, so rom_free()
** cannot free that shifted pointer; we free the original base instead. */
void *osd_rom_base = NULL;
bool osd_rom_owned = false;
/*******************************************************************************
* Public API functions
*******************************************************************************/

/* Load the selected game from an SD card file into PSRAM and return its base.
** The returned pointer must stay valid for the whole emulation session. */
static char *osd_getromdata_from_sd(const char *path)
{
   FILE *fp = fopen(path, "rb");
   if (NULL == fp)
   {
      ESP_LOGE(TAG, "open %s failed", path);
      return NULL;
   }

   fseek(fp, 0, SEEK_END);
   long size = ftell(fp);
   fseek(fp, 0, SEEK_SET);
   if (size <= 16)
   {
      fclose(fp);
      ESP_LOGE(TAG, "%s too small", path);
      return NULL;
   }

   char *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (NULL == buf)
   {
      fclose(fp);
      ESP_LOGE(TAG, "oom for %s (%ld bytes)", path, size);
      return NULL;
   }

   size_t rd = fread(buf, 1, size, fp);
   fclose(fp);
   if (rd != (size_t)size)
   {
      free(buf);
      ESP_LOGE(TAG, "short read %s", path);
      return NULL;
   }

   osd_rom_base = buf;     /* rom_free() must free this exact pointer */
   osd_rom_owned = true;
   return buf;
}

char *osd_getromdata()
{
	/* Wait for select the game */
	while(app_selected_game == NULL)
	{
		vTaskDelay(pdMS_TO_TICKS(100));
	};

	char* romdata = NULL;

	/* SD card path (e.g. "/sdcard/xxx.nes") takes priority. */
	size_t mplen = strlen(BSP_SD_MOUNT_POINT);
	if (0 == strncmp(app_selected_game, BSP_SD_MOUNT_POINT, mplen))
	{
		romdata = osd_getromdata_from_sd(app_selected_game);
		if (NULL == romdata)
			ESP_LOGE(TAG, "load from SD failed: %s", app_selected_game);
	}
	else
	{
		/* Fallback: raw .nes flashed into a data partition. */
		const esp_partition_t* part;
		spi_flash_mmap_handle_t hrom;
		esp_err_t err;
		part=esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, app_selected_game);
		if (part==0) printf("Couldn't find rom part!\n");
		else {
			err=esp_partition_mmap(part, 0, part->size, SPI_FLASH_MMAP_DATA, (const void**)&romdata, &hrom);
			if (err!=ESP_OK) printf("Couldn't map rom part!\n");
		}
		osd_rom_base = NULL;
		osd_rom_owned = false;
	}

	printf("Initialized. ROM@%p\n", romdata);
   return (char*)romdata;
}

void osd_getvideoinfo(vidinfo_t *info)
{
   info->default_width = APP_DEFAULT_WIDTH;
   info->default_height = APP_DEFAULT_HEIGHT;
   info->driver = &vid_driver;
}

void osd_getsoundinfo(sndinfo_t *info)
{
}

void osd_setsound(void (*playfunc)(void *buffer, int size))
{
}

int osd_init(void)
{
   uint32_t buf_size = APP_DEFAULT_SCALE * APP_DEFAULT_WIDTH * APP_DEFAULT_SCALE * APP_DEFAULT_HEIGHT * sizeof(uint16_t);
   vid_buffer = heap_caps_malloc(buf_size, MALLOC_CAP_DEFAULT);
   assert(vid_buffer);
   memset(vid_buffer, 0x00, buf_size);

   app_hid_init();

   /* Mount SD card so games can be loaded from /sdcard. Non-fatal if absent. */
   esp_err_t sd_err = bsp_sdcard_mount();
   if (sd_err == ESP_OK) {
      s_sd_mounted = true;
      app_rom_dir_ensure();
      ESP_LOGI(TAG, "SD card mounted at %s (ROM dir: %s)", BSP_SD_MOUNT_POINT, ROM_DIR);
   } else {
      ESP_LOGW(TAG, "SD card mount failed (%s) - using flashed partitions only",
               esp_err_to_name(sd_err));
   }

   bsp_display_lock(0);
   lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);
   lvgl_video_canvas = lv_canvas_create(lv_scr_act());
   lv_canvas_set_buffer(lvgl_video_canvas, vid_buffer, APP_DEFAULT_SCALE*APP_DEFAULT_WIDTH, APP_DEFAULT_SCALE*APP_DEFAULT_HEIGHT, LV_COLOR_FORMAT_RGB565);
   lv_obj_center(lvgl_video_canvas);
   bsp_display_unlock();

	/* 开机流程：显示选单菜单后，只要SD 已挂载就启动"开机嗅探"任务：
	** 开机自动进入串口接收等待(BOOT_UART_WAIT_MS)，无需人工点按钮。
	**   - 窗口内收到 PC 的 ROM 命令 -> 上传 -> 重启 -> 自动加载新 ROM；
	**   - 窗口超时无指令 -> 若有候选游戏则自动启动(SD-English优先)，开机即玩；
	**     若一个 ROM 都没有，则停在选单(保留 UART ROM 按钮)等待人工/再次上传。
	**   - 用户在菜单点了某个游戏 -> 标记 s_user_picked_game，嗅探超时也不覆盖。
	** 无 SD 时无法嗅探/上传：有候选则倒计时自动启动，无候选则停在选单。 */
	bool have_auto = app_auto_select_game();
	app_lvgl_game_selection();
	if (s_sd_mounted) {
		/* 关键：只要 SD 在就自动嗅探，覆盖"首次传 ROM"场景。
		 * 有无候选都启动——没候选时嗅探超时不会自动启动任何东西，
		 * 只是回到菜单等待，符合预期。 */
		xTaskCreate(app_uart_upload_task, "uart_sniff", 4096, (void *)1, 4, NULL);
		if (!have_auto) {
			ESP_LOGI(TAG, "No ROM yet. Waiting for UART ROM upload (auto sniff) ...");
		}
	} else if (have_auto) {
		/* 无 SD(无法嗅探/上传)：直接进入倒计时自动启动。 */
		app_autostart_begin();
	}

	return 0;
}

void osd_shutdown(void)
{
}

/* This is os-specific part of main() */
int osd_main(int argc, char *argv[])
{
   config.filename = "config.cfg";
   return main_loop("rom", system_autodetect);
}

int osd_installtimer(int frequency, void *func, int funcsize, void *counter, int countersize)
{
	printf("Timer install, freq=%d\n", frequency);
	timer=xTimerCreate("nes",configTICK_RATE_HZ/frequency, pdTRUE, NULL, func);
	xTimerStart(timer, 0);
   return 0;
}

/* File system interface */
void osd_fullname(char *fullname, const char *shortname)
{
   strncpy(fullname, shortname, PATH_MAX);
}

/* This gives filenames for storage of saves */
char *osd_newextension(char *string, char *ext)
{
   return string;
}

void osd_getinput(void)
{
	event_t evh = NULL;
   int pressed = 0;

   /* Merge keyboard- and gamepad-derived joypad state. */
   app_gamepad.btn_select = gp_pad.btn_select | kb_pad.btn_select;
   app_gamepad.btn_start  = gp_pad.btn_start  | kb_pad.btn_start;
   app_gamepad.btn_up     = gp_pad.btn_up     | kb_pad.btn_up;
   app_gamepad.btn_down   = gp_pad.btn_down   | kb_pad.btn_down;
   app_gamepad.btn_right  = gp_pad.btn_right  | kb_pad.btn_right;
   app_gamepad.btn_left   = gp_pad.btn_left   | kb_pad.btn_left;
   app_gamepad.btn_a      = gp_pad.btn_a      | kb_pad.btn_a;
   app_gamepad.btn_b      = gp_pad.btn_b      | kb_pad.btn_b;

   if(app_gamepad_tmp.btn_select != app_gamepad.btn_select)
   {
      pressed = (app_gamepad.btn_select ? INP_STATE_MAKE : INP_STATE_BREAK);
      evh = event_get(event_joypad1_select);
      app_gamepad_tmp.btn_select = app_gamepad.btn_select;

      if(evh) {
         evh(pressed);
      }
   }

   if(app_gamepad_tmp.btn_start != app_gamepad.btn_start)
   {
      pressed = (app_gamepad.btn_start ? INP_STATE_MAKE : INP_STATE_BREAK);
      evh = event_get(event_joypad1_start);
      app_gamepad_tmp.btn_start = app_gamepad.btn_start;

      if(evh) {
         evh(pressed);
      }
   }

   if(app_gamepad_tmp.btn_up != app_gamepad.btn_up)
   {
      pressed = (app_gamepad.btn_up ? INP_STATE_MAKE : INP_STATE_BREAK);
      evh = event_get(event_joypad1_up);
      app_gamepad_tmp.btn_up = app_gamepad.btn_up;

      if(evh) {
         evh(pressed);
      }
   }

   if(app_gamepad_tmp.btn_down != app_gamepad.btn_down)
   {
      pressed = (app_gamepad.btn_down ? INP_STATE_MAKE : INP_STATE_BREAK);
      evh = event_get(event_joypad1_down);
      app_gamepad_tmp.btn_down = app_gamepad.btn_down;

      if(evh) {
         evh(pressed);
      }
   }

   if(app_gamepad_tmp.btn_right != app_gamepad.btn_right)
   {
      pressed = (app_gamepad.btn_right ? INP_STATE_MAKE : INP_STATE_BREAK);
      evh = event_get(event_joypad1_right);
      app_gamepad_tmp.btn_right = app_gamepad.btn_right;

      if(evh) {
         evh(pressed);
      }
   }

   if(app_gamepad_tmp.btn_left != app_gamepad.btn_left)
   {
      pressed = (app_gamepad.btn_left ? INP_STATE_MAKE : INP_STATE_BREAK);
      evh = event_get(event_joypad1_left);
      app_gamepad_tmp.btn_left = app_gamepad.btn_left;

      if(evh) {
         evh(pressed);
      }
   }

   if(app_gamepad_tmp.btn_a != app_gamepad.btn_a)
   {
      pressed = (app_gamepad.btn_a ? INP_STATE_MAKE : INP_STATE_BREAK);
      evh = event_get(event_joypad1_a);
      app_gamepad_tmp.btn_a = app_gamepad.btn_a;

      if(evh) {
         evh(pressed);
      }
   }

   if(app_gamepad_tmp.btn_b != app_gamepad.btn_b)
   {
      pressed = (app_gamepad.btn_b ? INP_STATE_MAKE : INP_STATE_BREAK);
      evh = event_get(event_joypad1_b);
      app_gamepad_tmp.btn_b = app_gamepad.btn_b;

      if(evh) {
         evh(pressed);
      }
   }
}

void osd_getmouse(int *x, int *y, int *button)
{
}

/* This gives filenames for storage of PCX snapshots */
int osd_makesnapname(char *filename, int len)
{
   return -1;
}

/*******************************************************************************
* Private API functions
*******************************************************************************/

/* 删除整个选单界面（含挂在屏幕根上的 UART 按钮与倒计时标签）。
 * 可在 LVGL 任务（按钮回调/定时器回调，锁内）调用。 */
static void app_menu_destroy(void)
{
   if (game_selection) {
      lv_obj_del(game_selection);
      game_selection = NULL;
   }
   if (s_uart_btn) {
      lv_obj_del(s_uart_btn);
      s_uart_btn = NULL;
   }
   /* 倒计时标签由 app_autostart_cancel() 负责，这里兜底防泄漏 */
   if (s_autostart_label) {
      lv_obj_del(s_autostart_label);
      s_autostart_label = NULL;
   }
}

static void app_lvgl_btn_supermario(lv_event_t *e)
{
   s_user_picked_game = true;
   app_autostart_cancel();
   app_menu_destroy();
   app_selected_game = "nesrom1";
}

static void app_lvgl_btn_donkeykong(lv_event_t *e)
{
   s_user_picked_game = true;
   app_autostart_cancel();
   app_menu_destroy();
   app_selected_game = "nesrom2";
}

static void app_lvgl_btn_qbert(lv_event_t *e)
{
   s_user_picked_game = true;
   app_autostart_cancel();
   app_menu_destroy();
   app_selected_game = "nesrom3";
}

static void app_lvgl_btn_sd(lv_event_t *e)
{
   s_user_picked_game = true;
   app_autostart_cancel();
   const char *fn = (const char *)lv_event_get_user_data(e);
   snprintf(s_sel_path, sizeof(s_sel_path), "%s/%s", ROM_DIR, fn);
   app_selected_game = s_sel_path;
   app_menu_destroy();
}

/* "串口收 ROM"按钮：在独立任务里阻塞收文件并写入 /sdcard/ROM/，完成后重启自动加载。
 * arg=NULL -> 手动触发模式（长超时 30s 等用户发指令）。 */
static void app_lvgl_btn_uart_upload(lv_event_t *e)
{
   s_user_picked_game = true;   /* 手动收 ROM，不再走开机自动启动 */
   app_autostart_cancel();
   ESP_LOGI(TAG, "UART ROM upload requested from menu");
   app_menu_destroy();
   xTaskCreate(app_uart_upload_task, "uart_rom", 4096, NULL, 4, NULL);
}

static void app_lvgl_btn_reset(lv_event_t *e)
{
   esp_restart();
}

/* Create a selectable game button inside the `games` container. */
static void app_lvgl_add_game_btn(lv_obj_t *games, lv_group_t *main_group,
                                  const char *label, lv_event_cb_t cb, void *user_data)
{
   lv_obj_t *btn = lv_btn_create(games);
   lv_obj_set_size(btn, 230, 90);
   lv_obj_set_style_radius(btn, 8, 0);
   lv_obj_set_style_bg_color(btn, lv_color_make(0x1f, 0x3a, 0x5f), 0);
   lv_obj_t *bl = lv_label_create(btn);
   lv_obj_set_style_text_color(bl, lv_color_white(), 0);
   lv_label_set_text(bl, label);
   lv_obj_center(bl);
   lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
   if (main_group) {
      lv_group_add_obj(main_group, btn);
   }
}

/* ---- 倒计时自动启动 -------------------------------------------------------- */

/* 倒计时到点或用户取消时调用：真正设置 app_selected_game 让 osd_getromdata() 放行。 */
static void app_autostart_launch(void)
{
   if (app_selected_game == NULL && s_auto_pick != NULL) {
      app_selected_game = s_auto_pick;
   }
   if (game_selection) {
      lv_obj_del(game_selection);
      game_selection = NULL;
   }
   if (s_uart_btn) {
      lv_obj_del(s_uart_btn);
      s_uart_btn = NULL;
   }
}

/* LVGL 定时器回调（LVGL 任务、锁内）：刷新倒计时文案，到 0 则自动启动。 */
static void app_autostart_timer_cb(lv_timer_t *t)
{
   if (--s_autostart_left > 0) {
      if (s_autostart_label) {
         char buf[48];
         snprintf(buf, sizeof(buf), "Auto-start in %d s", s_autostart_left);
         lv_label_set_text(s_autostart_label, buf);
      }
      return;
   }
   /* 到点：删掉倒计时定时器与标签，然后自动启动候选游戏 */
   lv_timer_del(t);
   s_autostart_timer = NULL;
   if (s_autostart_label) {
      lv_obj_del(s_autostart_label);
      s_autostart_label = NULL;
   }
   ESP_LOGI(TAG, "Auto-start countdown finished -> %s", s_auto_pick ? s_auto_pick : "(none)");
   app_autostart_launch();
}

/* 取消倒计时（用户点击了某个按钮）。可在 LVGL 任务（锁内）调用。 */
static void app_autostart_cancel(void)
{
   if (s_autostart_timer) {
      lv_timer_del(s_autostart_timer);
      s_autostart_timer = NULL;
   }
   if (s_autostart_label) {
      lv_obj_del(s_autostart_label);
      s_autostart_label = NULL;
   }
   s_autostart_left = 0;
}

/* 开始倒计时自动启动（menu 已显示、有 auto 候选时调用）。
 * 可能从 app_main 任务（osd_init）或上传任务调用，故自行持有 LVGL 锁；lvgl 锁是
 * 递归互斥量，重入安全。 */
static void app_autostart_begin(void)
{
   if (!s_auto_pick) {
      return;
   }
   bsp_display_lock(0);
   s_autostart_left = AUTOSTART_SEC;
   s_autostart_timer = lv_timer_create(app_autostart_timer_cb, 1000, NULL);
   bsp_display_unlock();
}

/* ---- 串口 ROM 上传任务 ---------------------------------------------------- */

/* 串口接收任务。sniff=true：开机自动嗅探(短超时 BOOT_UART_WAIT_MS)，超时后若用户
 * 没选游戏则自动启动候选游戏；sniff=false：按钮手动触发(长超时)，超时回菜单。
 * 两种模式都收到文件后重启，让固件按 SD-English 优先规则自动加载。 */
static void app_uart_upload_task(void *arg)
{
   bool sniff = (arg != NULL);
   if (s_uart_rx_busy) {
      /* 已有接收任务在跑（如嗅探中），放弃本次。 */
      vTaskDelete(NULL);
      return;
   }
   s_uart_rx_busy = true;

   ESP_LOGI(TAG, "UART ROM upload task started (sniff=%d)", sniff);
   /* 嗅探窗口自适应：已有游戏 -> 短热更新窗口(BOOT_UART_HOT_WINDOW_MS)快速自动启动；
    * 无游戏 -> 长窗口(BOOT_UART_WAIT_MS)等待首次上传。手动触发固定用长超时。 */
   uint32_t first_idle;
   if (!sniff) {
      first_idle = ROM_UPLOAD_IDLE_MS;
   } else {
      first_idle = (s_auto_pick != NULL) ? BOOT_UART_HOT_WINDOW_MS : BOOT_UART_WAIT_MS;
   }
   bool got = app_uart_rom_upload(first_idle);
   s_uart_rx_busy = false;

   if (got) {
      ESP_LOGI(TAG, "UART ROM upload done, rebooting");
      vTaskDelay(pdMS_TO_TICKS(300));
      esp_restart();
   }

   if (sniff) {
      /* 开机嗅探窗口内没有收到任何 ROM 命令：
       * - 用户已在菜单点了某个游戏 -> 尊重用户选择，直接进游戏；
       * - 否则自动启动候选游戏(SD-English 优先)；若无候选则随机挑一个；
       * - 一个 ROM 都没有 -> 停在选单(保留 UART ROM 按钮)提示上传。 */
      if (!s_user_picked_game) {
         if (s_auto_pick == NULL) {
            /* 没有 English/首选候选，随机挑一个可玩到的游戏。 */
            app_random_pick_game();
         }
         if (s_auto_pick != NULL) {
            ESP_LOGI(TAG, "Boot sniff idle -> auto-start %s", s_auto_pick);
            app_selected_game = s_auto_pick;
         } else {
            ESP_LOGW(TAG, "Boot sniff idle: no ROM available, stay in menu for upload");
         }
      }
      vTaskDelete(NULL);
      return;
   }

   ESP_LOGW(TAG, "UART ROM upload idle timeout, back to menu");
   /* 按钮触发超时：回到选单界面，让用户可再试或选游戏；若有候选游戏则恢复倒计时。
    * app_lvgl_game_selection() 内部自行处理 LVGL 锁，此处不重复加锁。 */
   app_lvgl_game_selection();
   if (s_auto_pick) {
      app_autostart_begin();
   }
   vTaskDelete(NULL);
}

/* 从 NES 头（偏移 0x10 起 16 字节）取 ROM 名称字段；若全部可打印则作为显示名，
 * 否则回退到该槽位的默认名。 */
static const char *app_rom_display_name(const uint8_t *hdr, int slot)
{
    static char name[32];
    int n = 0;
    for (int i = 0; i < 16; i++) {
        uint8_t c = hdr[16 + i];
        if (c >= 0x20 && c <= 0x7E) {
            name[n++] = (char)c;
        } else if (c == 0) {
            break;
        } else {
            /* 不可打印字符 -> 视为名称无效，用默认名 */
            n = 0;
            break;
        }
    }
    if (n == 0) {
        return GAME_FALLBACK[slot];
    }
    name[n] = 0;
    return name;
}

/*******************************************************************************
* 自动选择 ROM（开机直接进入游戏，不再显示点选界面）
*******************************************************************************/

#define MAX_ROM_FILES   32
#define ROM_NAME_LEN    128

/* 确保 /sdcard/ROM 存在，不存在则创建。 */
static void app_rom_dir_ensure(void)
{
   struct stat st;
   if (stat(ROM_DIR, &st) != 0) {
      if (mkdir(ROM_DIR, 0777) != 0) {
         ESP_LOGW(TAG, "mkdir %s failed", ROM_DIR);
      }
   }
}

/* newlib 不保证提供 strcasestr，这里自带大小写不敏感的子串匹配。 */
static bool app_ci_contains(const char *hay, const char *needle)
{
   if (!*needle) {
      return true;
   }
   for (const char *p = hay; *p; p++) {
      if (tolower((unsigned char)*p) != tolower((unsigned char)*needle)) {
         continue;
      }
      const char *a = p + 1;
      const char *b = needle + 1;
      while (*b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
         a++;
         b++;
      }
      if (!*b) {
         return true;
      }
   }
   return false;
}

/* 枚举 SD 卡 ROM 目录下的 *.nes 并按字母序排序，返回数量。 */
static int app_scan_sd_roms(char names[MAX_ROM_FILES][ROM_NAME_LEN])
{
   int n = 0;
   DIR *d = opendir(ROM_DIR);
   if (!d) {
      return 0;
   }
   struct dirent *ent;
   while ((ent = readdir(d)) != NULL && n < MAX_ROM_FILES) {
      const char *nm = ent->d_name;
      size_t l = strlen(nm);
      if (l < 5 || l >= ROM_NAME_LEN) {
         continue;
      }
      if (strcasecmp(nm + l - 4, ".nes") != 0) {
         continue;
      }
      /* 上面已保证 l < ROM_NAME_LEN，故含 NUL 的拷贝不会越界。 */
      memcpy(names[n], nm, l + 1);
      n++;
   }
   closedir(d);

   /* 插入排序：readdir 顺序取决于 FAT 目录项，排序后选择结果才稳定。 */
   for (int i = 1; i < n; i++) {
      char tmp[ROM_NAME_LEN];
      memcpy(tmp, names[i], ROM_NAME_LEN);
      int j = i - 1;
      while (j >= 0 && strcasecmp(names[j], tmp) > 0) {
         memcpy(names[j + 1], names[j], ROM_NAME_LEN);
         j--;
      }
      memcpy(names[j + 1], tmp, ROM_NAME_LEN);
   }
   return n;
}

/* 随机回退：从 SD 卡里所有 .nes 中随机挑一个作为自动启动候选。
 * 分区里一般不存 ROM，故这里只读 SD（与 app_auto_select_game 的扫描范围一致）。
 * 用于"SD 为空且上传窗口超时"等场景——让设备总能玩到某个游戏。
 * 找到返回 true 并写 s_auto_pick，否则返回 false（SD 上一个 ROM 都没有）。 */
static bool app_random_pick_game(void)
{
   static char names[MAX_ROM_FILES][ROM_NAME_LEN];
   static char paths[MAX_ROM_FILES][ROM_NAME_LEN + 32];
   int n = 0;

   if (s_sd_mounted) {
      n = app_scan_sd_roms(names);
   }

   if (n == 0) {
      s_auto_pick = NULL;
      return false;
   }

   /* 用时间低位做伪随机种子，在候选里随机挑一个。 */
   int pick = (int)(esp_timer_get_time() % n);
   snprintf(paths[pick], sizeof(paths[pick]), "%s/%s", ROM_DIR, names[pick]);
   s_auto_pick = paths[pick];
   ESP_LOGI(TAG, "Random pick: %s (from %d SD candidate(s))", s_auto_pick, n);
   return true;
}

/* 选出一个 ROM 作为"自动启动候选"，写入 s_auto_pick（SD 路径或分区标签）。
 * 优先级：SD 上文件名含 "english" 的 .nes > SD 上字母序第一个 .nes > 含有效 NES 头的分区。
 * 不直接设置 app_selected_game —— 由倒计时定时器或用户点击来最终启动。 */
static bool app_auto_select_game(void)
{
   static char names[MAX_ROM_FILES][ROM_NAME_LEN];

   s_auto_pick = NULL;

   if (s_sd_mounted) {
      int n = app_scan_sd_roms(names);
      if (n > 0) {
         int pick = 0;
         for (int i = 0; i < n; i++) {
            if (app_ci_contains(names[i], "english")) {
               pick = i;
               break;
            }
         }
         snprintf(s_auto_path, sizeof(s_auto_path), "%s/%s", ROM_DIR, names[pick]);
         s_auto_pick = s_auto_path;
         ESP_LOGI(TAG, "Auto-start candidate: %s (%d ROM(s) on SD)", s_auto_path, n);
         return true;
      }
   }

   /* 回退：烧录在 data 分区里的裸 .nes */
   for (int i = 0; i < MAX_GAMES; i++) {
      const esp_partition_t *part = esp_partition_find_first(
          ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, GAME_PARTS[i]);
      if (!part) {
         continue;
      }
      uint8_t hdr[4];
      if (esp_partition_read(part, 0, hdr, sizeof(hdr)) != ESP_OK) {
         continue;
      }
      if (!(hdr[0] == 'N' && hdr[1] == 'E' && hdr[2] == 'S' && hdr[3] == 0x1A)) {
         continue;
      }
      s_auto_pick = GAME_PARTS[i];
      ESP_LOGI(TAG, "Auto-start candidate: partition %s", GAME_PARTS[i]);
      return true;
   }

   return false;
}

/*******************************************************************************
* 串口 ROM 上传（SD 卡上没有任何 ROM 时，通过 UART0 写入 SD 卡）
*
* 协议（PC 端 -> 设备，行以 \n 结尾）：
*   "ROM <name> <size>\n"  -> 设备回 "READY <name>\n" -> PC 发送 <size> 字节裸数据
*                          -> 设备回 "OK <name> <size>\n"
*   "DONE\n"               -> 设备回 "REBOOT\n" 并重启
*******************************************************************************/

/* 读一行（不含换行符）；超时或读到空行返回 false。 */
static bool app_uart_read_line(char *line, size_t max, uint32_t timeout_ms)
{
   size_t n = 0;
   uint8_t c;
   while (n + 1 < max) {
      if (uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(timeout_ms)) != 1) {
         line[n] = 0;
         return false;
      }
      if (c == '\n' || c == '\r') {
         line[n] = 0;
         return n > 0;
      }
      line[n++] = (char)c;
   }
   line[n] = 0;
   return true;
}

/* 接收单个文件并写入 /sdcard/<name>。 */
static bool app_uart_recv_one(const char *name, size_t size)
{
   /* 只接受根目录下的简单文件名，避免路径穿越。 */
   for (const char *p = name; *p; p++) {
      char c = *p;
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-' || c == '.' || c == ' ')) {
         printf("ERR bad name\n");
         return false;
      }
   }
   if (size == 0 || size > (64UL * 1024 * 1024)) {
      printf("ERR bad size %u\n", (unsigned)size);
      return false;
   }

   char path[ROM_NAME_LEN + 24];
   snprintf(path, sizeof(path), "%s/%s", ROM_DIR, name);

   FILE *fp = fopen(path, "wb");
   if (!fp) {
      printf("ERR open %s\n", path);
      return false;
   }

   uint8_t *buf = heap_caps_malloc(ROM_UPLOAD_CHUNK, MALLOC_CAP_8BIT);
   if (!buf) {
      fclose(fp);
      printf("ERR oom\n");
      return false;
   }

   size_t remain = size;
   bool ok = true;
   /* 开始接收：切换等待窗口到"传输中"状态并刷新初始进度。 */
   s_uart_wait_transferring = true;
   app_uart_wait_progress(name, 0, size);
   while (remain > 0) {
      size_t want = remain > ROM_UPLOAD_CHUNK ? ROM_UPLOAD_CHUNK : remain;
      int rd = uart_read_bytes(UART_NUM_0, buf, want, pdMS_TO_TICKS(ROM_UPLOAD_IDLE_MS));
      if (rd <= 0) {
         printf("ERR timeout (%u bytes left)\n", (unsigned)remain);
         ok = false;
         break;
      }
      if (fwrite(buf, 1, rd, fp) != (size_t)rd) {
         printf("ERR write sd\n");
         ok = false;
         break;
      }
      remain -= rd;
      app_uart_wait_progress(name, size - remain, size);
   }
   s_uart_wait_transferring = false;

   free(buf);
   fclose(fp);

   if (ok) {
      printf("OK %s %u\n", name, (unsigned)size);
   } else {
      remove(path);
   }
   return ok;
}

/* 串口收 ROM 等待期间的全屏提示窗口：替代原先"只有串口文本提示、屏上无反馈"的
 * 局面，让用户在屏上直接看到"正在等上传"。进入窗口调用 app_uart_wait_show()，
 * 退出(收到文件/超时)调用 app_uart_wait_hide()。mode 区分首次上传与热更新文案。 */
static lv_obj_t *s_uart_wait_win = NULL;
static lv_obj_t *s_uart_wait_dots_lbl = NULL;
static lv_obj_t *s_uart_wait_bar = NULL;
static lv_obj_t *s_uart_wait_prog_lbl = NULL;
static lv_timer_t *s_uart_wait_timer = NULL;
static int s_uart_wait_dots = 0;

static void app_uart_wait_anim_cb(lv_timer_t *t)
{
   (void)t;
   if (!s_uart_wait_dots_lbl) {
      return;
   }
   /* 传输进行中由 app_uart_wait_progress 接管状态行，动画让位。 */
   if (s_uart_wait_transferring) {
      return;
   }
   /* 旋转的点，提示界面在等待而非卡死。 */
   static const char *frames[4] = { "   ", ".  ", ".. ", "..." };
   char buf[64];
   snprintf(buf, sizeof(buf), "Waiting for UART ROM%s", frames[s_uart_wait_dots]);
   s_uart_wait_dots = (s_uart_wait_dots + 1) & 3;
   lv_label_set_text(s_uart_wait_dots_lbl, buf);
}

static void app_uart_wait_show(const char *mode)
{
   bsp_display_lock(0);
   /* 清理现有菜单/按钮，独占整屏。(s_autostart_label 不动——自动启动倒计时定时器
    * 仍可能引用它，删除会悬空；它会被下方全屏窗口盖住，菜单重建时再处理。) */
   if (game_selection) {
      lv_obj_del(game_selection);
      game_selection = NULL;
   }
   if (s_uart_btn) {
      lv_obj_del(s_uart_btn);
      s_uart_btn = NULL;
   }
   lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);

   s_uart_wait_win = lv_obj_create(lv_scr_act());
   lv_obj_set_size(s_uart_wait_win, BSP_LCD_H_RES, BSP_LCD_V_RES);
   lv_obj_set_style_bg_color(s_uart_wait_win, lv_color_black(), 0);
   lv_obj_set_style_border_width(s_uart_wait_win, 0, 0);
   lv_obj_clear_flag(s_uart_wait_win, LV_OBJ_FLAG_SCROLLABLE);
   lv_obj_align(s_uart_wait_win, LV_ALIGN_CENTER, 0, 0);

   lv_obj_t *cont = lv_obj_create(s_uart_wait_win);
   lv_obj_set_style_bg_color(cont, lv_color_black(), 0);
   lv_obj_set_style_border_width(cont, 0, 0);
   lv_obj_set_size(cont, BSP_LCD_H_RES - 80, BSP_LCD_V_RES - 80);
   lv_obj_center(cont);
   lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
   lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
   lv_obj_set_style_pad_row(cont, 14, 0);

   lv_obj_t *title = lv_label_create(cont);
   lv_obj_set_style_text_color(title, lv_color_white(), 0);
   lv_label_set_text_static(title, "UART ROM UPLOAD");

   s_uart_wait_dots_lbl = lv_label_create(cont);
   lv_obj_set_style_text_color(s_uart_wait_dots_lbl, lv_color_white(), 0);
   lv_label_set_text_static(s_uart_wait_dots_lbl, "Waiting for UART ROM ...");

   s_uart_wait_bar = lv_bar_create(cont);
   lv_obj_set_size(s_uart_wait_bar, 480, 22);
   lv_bar_set_range(s_uart_wait_bar, 0, 100);
   lv_bar_set_value(s_uart_wait_bar, 0, LV_ANIM_OFF);

   s_uart_wait_prog_lbl = lv_label_create(cont);
   lv_obj_set_style_text_color(s_uart_wait_prog_lbl, lv_color_make(0x5f, 0xd0, 0xff), 0);
   lv_label_set_text_static(s_uart_wait_prog_lbl, "");

   lv_obj_t *hint = lv_label_create(cont);
   lv_obj_set_style_text_color(hint, lv_color_make(0x9a, 0x9a, 0x9a), 0);
   lv_label_set_text_static(hint, mode);

   lv_obj_t *cmd = lv_label_create(cont);
   lv_obj_set_style_text_color(cmd, lv_color_make(0x5f, 0xd0, 0xff), 0);
   lv_label_set_text_static(cmd,
                           "PC:  ROM <name> <size>\\n"
                           "     wait READY, send raw bytes\\n"
                           "     DONE to reboot");

   s_uart_wait_timer = lv_timer_create(app_uart_wait_anim_cb, 400, NULL);
   bsp_display_unlock();
}

static void app_uart_wait_hide(void)
{
   bsp_display_lock(0);
   s_uart_wait_transferring = false;
   if (s_uart_wait_timer) {
      lv_timer_del(s_uart_wait_timer);
      s_uart_wait_timer = NULL;
   }
   if (s_uart_wait_win) {
      lv_obj_del(s_uart_wait_win);
      s_uart_wait_win = NULL;
   }
   s_uart_wait_dots_lbl = NULL;
   s_uart_wait_bar = NULL;
   s_uart_wait_prog_lbl = NULL;
   bsp_display_unlock();
}

/* 传输进行中更新等待窗口：显示正在传的 ROM 名称与进度(百分比 + 字节数)。
 * 由 app_uart_recv_one 在接收循环里周期性调用。 */
static void app_uart_wait_progress(const char *name, size_t got, size_t total)
{
   bsp_display_lock(0);
   if (s_uart_wait_dots_lbl) {
      char s[80];
      snprintf(s, sizeof(s), "Receiving: %s", name);
      lv_label_set_text(s_uart_wait_dots_lbl, s);
   }
   if (s_uart_wait_bar) {
      int pct = total ? (int)((uint64_t)got * 100 / total) : 0;
      lv_bar_set_value(s_uart_wait_bar, pct, LV_ANIM_OFF);
   }
   if (s_uart_wait_prog_lbl) {
      char s[80];
      snprintf(s, sizeof(s), "%u / %u bytes (%d%%)",
               (unsigned)got, (unsigned)total,
               total ? (int)((uint64_t)got * 100 / total) : 0);
      lv_label_set_text(s_uart_wait_prog_lbl, s);
   }
   bsp_display_unlock();
}

/* SD 卡已挂载时可通过串口接收 ROM。first_idle_ms 为等待第一个 ROM 命令的超时：
 * 开机自动嗅探传 BOOT_UART_WAIT_MS(短)，手动按钮触发传 ROM_UPLOAD_IDLE_MS(长)。
 * 收到并传完文件返回 true，由调用方重启。 */
static bool app_uart_rom_upload(uint32_t first_idle_ms)
{
   uart_config_t cfg = {
      .baud_rate = ROM_UPLOAD_BAUD,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
   };

   /* tx_buffer_size = 0：TX 继续由 printf 直接使用，driver 只接管 RX。 */
   esp_err_t err = uart_driver_install(UART_NUM_0, 16384, 0, 0, NULL, 0);
   if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      ESP_LOGE(TAG, "uart driver install failed: %s", esp_err_to_name(err));
      return false;
   }
   uart_param_config(UART_NUM_0, &cfg);
   uart_set_pin(UART_NUM_0, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
   uart_flush_input(UART_NUM_0);
   app_rom_dir_ensure();

   /* 横幅按"是否已有游戏"区分措辞：已有游戏时这是热更新窗口，否则是首次上传窗口。 */
   if (s_auto_pick != NULL) {
      printf("\n=== UART ROM WINDOW (已有游戏 %s，留可热更新) ===\n", s_auto_pick);
   } else {
      printf("\n=== NO ROM IN %s (等待上传) ===\n", ROM_DIR);
   }
   printf("Send \"ROM <name> <size>\\n\", wait for \"READY\", then <size> raw bytes.\n");
   printf("Send \"DONE\\n\" to finish and reboot.\n");
   printf("READY-FOR-UART-ROM\n");
   fflush(stdout);
   vTaskDelay(pdMS_TO_TICKS(200));

   /* 屏上显示等待窗口（替代单纯串口文本），让用户在屏上看到"正在等上传"。 */
   char wait_mode[96];
   if (s_auto_pick != NULL) {
      snprintf(wait_mode, sizeof(wait_mode),
               "Hot-update: SD already has a game.\nSend a .nes to add / replace.");
   } else {
      snprintf(wait_mode, sizeof(wait_mode),
               "No ROM yet. Send a .nes file to start playing.");
   }
   app_uart_wait_show(wait_mode);

   int got = 0;
   char line[ROM_NAME_LEN + 32];
   /* first_idle_ms：等待"第一个 ROM 命令"的超时。开机自动嗅探用短超时(BOOT_UART_WAIT_MS)，
    * 超时即返回 false 让主流程自动启动游戏；按钮手动触发用长超时(ROM_UPLOAD_IDLE_MS)。
    * 一旦收到第一个 ROM 命令并开始接收，后续每包都用 ROM_UPLOAD_IDLE_MS(30s)，
    * 保证 1MB ROM 的长时间传输不会被打断。 */
   uint32_t idle = first_idle_ms;
   while (app_uart_read_line(line, sizeof(line), idle)) {
      if (0 == strncmp(line, "DONE", 4)) {
         printf("REBOOT\n");
         fflush(stdout);
         vTaskDelay(pdMS_TO_TICKS(300));
         esp_restart();
      }
      if (0 == strncmp(line, "ROM ", 4)) {
         char name[ROM_NAME_LEN];
         unsigned long size = 0;
         if (2 != sscanf(line + 4, "%127s %lu", name, &size)) {
            printf("ERR bad cmd: %s\n", line);
            continue;
         }
         printf("READY %s\n", name);
         fflush(stdout);
         /* 开始传输：切换到长超时，保证大文件传输不被打断 */
         idle = ROM_UPLOAD_IDLE_MS;
         if (app_uart_recv_one(name, (size_t)size)) {
            got++;
         }
         continue;
      }
      /* 未知行忽略（例如 PC 端残留的输出） */
   }

   if (got > 0) {
      ESP_LOGW(TAG, "UART ROM upload done (%d file(s))", got);
   } else {
      ESP_LOGW(TAG, "UART ROM upload: no command within idle window");
   }
   app_uart_wait_hide();
   return got > 0;
}

/* 三个按钮回调按分区顺序固定对应 nesrom1/2/3 */
static void (*const game_cbs[MAX_GAMES])(lv_event_t *) = {
    app_lvgl_btn_supermario, app_lvgl_btn_donkeykong, app_lvgl_btn_qbert
};

static void app_lvgl_game_selection(void)
{
    bsp_display_lock(0);
    /* 若已存在旧菜单（例如上传超时后重建），先删除避免泄漏/重复。
     * 重建前先取消倒计时，防止旧定时器指向已删除的标签。 */
    app_autostart_cancel();
    if (game_selection) {
        lv_obj_del(game_selection);
        game_selection = NULL;
    }
    if (s_uart_btn) {
        lv_obj_del(s_uart_btn);
        s_uart_btn = NULL;
    }
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);

    /* 复位按钮（左上角） */
    lv_obj_t * btn_rst = lv_btn_create(lv_scr_act());
    lv_obj_t * rlbl = lv_label_create(btn_rst);
    lv_label_set_text_static(rlbl, LV_SYMBOL_PREV" ");
    lv_obj_add_event_cb(btn_rst, app_lvgl_btn_reset, LV_EVENT_CLICKED, NULL);
    lv_obj_align(btn_rst, LV_ALIGN_TOP_LEFT, 10, 10);

    game_selection = lv_obj_create(lv_scr_act());
    lv_obj_set_size(game_selection, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_flex_flow(game_selection, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(game_selection, lv_color_black(), 0);
    lv_obj_set_style_border_color(game_selection, lv_color_black(), 0);
    lv_obj_align(game_selection, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(game_selection, 10, 0);
    lv_obj_set_flex_align(game_selection, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_move_foreground(game_selection);

    lv_obj_t * title = lv_label_create(game_selection);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text_static(title, "SELECT GAME");

    lv_obj_t * games = lv_obj_create(game_selection);
    lv_obj_set_style_bg_color(games, lv_color_black(), 0);
    lv_obj_set_style_border_color(games, lv_color_black(), 0);
    lv_obj_set_size(games, BSP_LCD_H_RES - 40, BSP_LCD_V_RES/2 + 40);
    lv_obj_align(games, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_flex_flow(games, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_all(games, 10, 0);
    lv_obj_set_flex_align(games, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    int found = 0;
    lv_group_t *main_group = NULL;
    lv_indev_t *indev = bsp_display_get_input_dev();
    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
        main_group = lv_group_create();
    }

    /* Primary: list *.nes files from the SD card. */
    if (s_sd_mounted) {
        DIR *d = opendir(ROM_DIR);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                const char *nm = ent->d_name;
                size_t l = strlen(nm);
                if (l < 4)
                    continue;
                const char *ext = nm + l - 4;
                if (strcasecmp(ext, ".nes") != 0)
                    continue;

                char *ud = strdup(nm);
                if (!ud)
                    continue;
                app_lvgl_add_game_btn(games, main_group, nm, app_lvgl_btn_sd, ud);
                found++;
            }
            closedir(d);
        }
        if (found > 0) {
            ESP_LOGI(TAG, "Found %d ROM(s) on SD card", found);
        }
    }

    /* Fallback: raw .nes images flashed into data partitions. */
    if (found == 0) {
        for (int i = 0; i < MAX_GAMES; i++) {
            const esp_partition_t *part = esp_partition_find_first(
                ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, GAME_PARTS[i]);
            if (!part) {
                ESP_LOGW(TAG, "partition %s not found", GAME_PARTS[i]);
                continue;
            }
            uint8_t hdr[32];
            if (esp_partition_read(part, 0, hdr, sizeof(hdr)) != ESP_OK) {
                ESP_LOGW(TAG, "read %s header failed", GAME_PARTS[i]);
                continue;
            }
            if (!(hdr[0] == 'N' && hdr[1] == 'E' && hdr[2] == 'S' && hdr[3] == 0x1A)) {
                ESP_LOGW(TAG, "partition %s has no valid NES header -> skipped", GAME_PARTS[i]);
                continue;
            }

            const char *disp = app_rom_display_name(hdr, i);
            app_lvgl_add_game_btn(games, main_group, disp, game_cbs[i], NULL);
            found++;
        }
    }

    if (found == 0) {
        lv_obj_t *none = lv_label_create(games);
        lv_obj_set_style_text_color(none, lv_color_white(), 0);
        lv_label_set_text_static(none, "NO ROM FOUND\nPut .nes files in " ROM_DIR " on SD\nor flash into nesrom1/2/3");
    } else if (main_group) {
        lv_indev_set_group(indev, main_group);
        ESP_LOGI(TAG, "Game selection ready (%d ROMs).", found);
    }

    /* 串口收 ROM 入口（仅 SD 已挂载时可用）：不擦除分区也能把 .nes 存进 SD。
     * 挂在屏幕根上并绝对定位到底部：避开 game_selection 的 flex 布局与
     * 480px 高度裁剪，确保按钮始终可见、可点。 */
    if (s_sd_mounted) {
        lv_obj_t *up = lv_btn_create(lv_scr_act());
        lv_obj_set_size(up, 300, 60);
        lv_obj_set_style_radius(up, 8, 0);
        lv_obj_set_style_bg_color(up, lv_color_make(0x5f, 0x1f, 0x3a), 0);
        lv_obj_t *upl = lv_label_create(up);
        lv_obj_set_style_text_color(upl, lv_color_white(), 0);
        lv_label_set_text_static(upl, "UART ROM -> " ROM_DIR);
        lv_obj_center(upl);
        lv_obj_add_event_cb(up, app_lvgl_btn_uart_upload, LV_EVENT_CLICKED, NULL);
        lv_obj_align(up, LV_ALIGN_BOTTOM_MID, 0, -24);
        lv_obj_move_foreground(up);
        s_uart_btn = up;
        if (main_group) {
            lv_group_add_obj(main_group, up);
        }
    }

    /* 倒计时提示标签（app_autostart_begin 会把剩余秒数写进来）。 */
    if (s_auto_pick) {
        char buf[48];
        snprintf(buf, sizeof(buf), "Auto-start in %d s", AUTOSTART_SEC);
        s_autostart_label = lv_label_create(lv_scr_act());
        lv_obj_set_style_text_color(s_autostart_label, lv_color_white(), 0);
        lv_label_set_text(s_autostart_label, buf);
        lv_obj_align(s_autostart_label, LV_ALIGN_BOTTOM_MID, 0, -96);
        lv_obj_move_foreground(s_autostart_label);
    }

    bsp_display_unlock();
}

static int app_osd_video_init(int width, int height)
{
	return 0;
}

static void app_osd_video_shutdown(void)
{

}

/* set a video mode */
static int app_osd_video_set_mode(int width, int height)
{
   return 0;
}

/* copy nes palette over to hardware */
static void app_osd_video_set_palette(rgb_t *palette)
{
	uint16 c;

   for (int i = 0; i < 256; i++)
   {
      c=(palette[i].b>>3)+((palette[i].g>>2)<<5)+((palette[i].r>>3)<<11);
      colorPalette[i]=c;
   }
}

/* clear all frames to a particular color */
static void app_osd_video_clear(uint8 color)
{
   bsp_display_lock(0);

   /* Write the palette entry as a native (little-endian) uint16 RGB565 value.
   ** ESP32 + LVGL RGB565 framebuffers expect the low byte first; the previous
   ** HIBYTE/LOBYTE swap stored it big-endian and swapped red/blue. */
   uint16_t c = colorPalette[color];
   uint16_t *buf = (uint16_t *)vid_buffer;
   int npix = APP_DEFAULT_SCALE * APP_DEFAULT_WIDTH * APP_DEFAULT_SCALE * APP_DEFAULT_HEIGHT;
   for (int i = 0; i < npix; i++)
      buf[i] = c;

   lv_obj_invalidate(lvgl_video_canvas);
   bsp_display_unlock();
}

/* acquire the directbuffer for writing */
static bitmap_t *app_osd_video_lock_write(void)
{
   vidBitmap = bmp_create(APP_DEFAULT_WIDTH, APP_DEFAULT_HEIGHT, APP_DEFAULT_WIDTH*sizeof(uint16));
   return vidBitmap;
}

static void app_osd_video_free_write(int num_dirties, rect_t *dirty_rects)
{
   bmp_destroy(&vidBitmap);
   vidBitmap = NULL;
}

static void app_osd_video_custom_blit(bitmap_t *primary, int num_dirties, rect_t *dirty_rects)
{
   const uint8_t *data = primary->data;
   int sw = APP_DEFAULT_WIDTH;
   int sh = APP_DEFAULT_HEIGHT;
   int scale = APP_DEFAULT_SCALE;
   int dw = scale * sw;          /* destination width  in pixels */
   int dh = scale * sh;          /* destination height in pixels */
   uint16_t *buf = (uint16_t *)vid_buffer;

   bsp_display_lock(0);

   /* Nearest-neighbour upscale. Each destination pixel is written as a native
   ** (little-endian) uint16 RGB565 value so colours are correct on the LCD. */
   for (int y = 0; y < dh; y++) {
      int sy = y / scale;
      for (int x = 0; x < dw; x++) {
         int sx = x / scale;
         buf[y * dw + x] = colorPalette[data[sy * sw + sx]];
      }
   }

   lv_obj_invalidate(lvgl_video_canvas);
   bsp_display_unlock();
}

/* Map a USB keyboard report onto the NES joypad so standard NES games respond
** to the keyboard. The Subor matrix is handled separately by subor_kb_set_report. */
static void app_gamepad_from_kb(uint8_t modifier, const uint8_t keycodes[6])
{
   memset(&kb_pad, 0, sizeof(kb_pad));

   for (int i = 0; i < 6; i++) {
      uint8_t k = keycodes[i];
      switch (k) {
         case 0x52: kb_pad.btn_up     = true; break;  /* Up arrow    */
         case 0x51: kb_pad.btn_down   = true; break;  /* Down arrow  */
         case 0x50: kb_pad.btn_left   = true; break;  /* Left arrow  */
         case 0x4F: kb_pad.btn_right  = true; break;  /* Right arrow */
         case 0x1A: kb_pad.btn_a      = true; break;  /* Z -> A      */
         case 0x1D: kb_pad.btn_b      = true; break;  /* X -> B      */
         case 0x28: kb_pad.btn_start  = true; break;  /* Enter -> Start */
         case 0x2A: kb_pad.btn_select = true; break;  /* Backspace -> Select */
         default: break;
      }
   }
}

static void app_usb_hid_host_interface_callback(hid_host_device_handle_t hid_device_handle, const hid_host_interface_event_t event, void *arg)
{
   hid_host_dev_params_t *dev = hid_host_device_get_params(hid_device_handle);
   uint8_t *data = NULL;
   unsigned int data_length = 0;

   assert(dev != NULL);

   switch (event) {
   case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
      data = hid_host_device_get_data(hid_device_handle, &data_length);
      if (data == NULL)
         break;

      /* USB 标准键盘：BOOT 报文为 8 字节（byte0=修饰键, byte2..7=最多6个键码）。
       * 同时驱动学习机 Subor 键盘矩阵与 NES 手柄（供标准 NES 游戏使用）。 */
      if (hid_device_handle == s_keyboard_handle && data_length >= 8) {
         subor_kb_set_report(data[0], &data[2]);
         app_gamepad_from_kb(data[0], &data[2]);

         /* Caps Lock LED: keep the keyboard's LED in sync with lock state */
         bool caps = subor_kb_get_caps_lock();
         if (caps != s_caps_led) {
            s_caps_led = caps;
            uint8_t led = caps ? 0x02 : 0x00;   /* bit1 = Caps Lock */
            hid_class_request_set_report(s_keyboard_handle, HID_REPORT_TYPE_OUTPUT, 0, &led, 1);
         }
         osd_getinput();
         break;
      }

      /* 其它 HID 设备（手柄/游戏手柄）沿用原映射逻辑 */
      if (data_length < 8) {
            break;
      }

      gp_pad.btn_up = (data[1] == 0x00 ? true : false);
      gp_pad.btn_down = (data[1] == 0xFF ? true : false);
      gp_pad.btn_left = (data[0] == 0x00 ? true : false);
      gp_pad.btn_right = (data[0] == 0xFF ? true : false);

      gp_pad.btn_select = (data[6]&0x10 ? true : false);
      gp_pad.btn_start = (data[6]&0x20 ? true : false);
      gp_pad.btn_a = (data[5]&0x20 ? true : false);
      gp_pad.btn_b = (data[5]&0x40 ? true : false);

      osd_getinput();
      break;
   case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
      break;
   case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
      if (hid_device_handle == s_keyboard_handle) {
         s_keyboard_handle = NULL;
         subor_kb_enable(false);
         s_caps_led = false;
         memset(&kb_pad, 0, sizeof(kb_pad));
      }
      hid_host_device_close(hid_device_handle);
      break;
   default:
      break;
   }
}

static void app_usb_hid_task(void *arg)
{
    hid_host_device_handle_t hid_device_handle = NULL;
    app_usb_hid_event_t msg;

    while (1) {
        if (xQueueReceive(hid_queue, &msg, pdMS_TO_TICKS(50))) {
            hid_device_handle = msg.hid_device_handle;
            hid_host_dev_params_t *dev = hid_host_device_get_params(hid_device_handle);

            assert(dev != NULL);

            switch (msg.event) {
            case HID_HOST_DRIVER_EVENT_CONNECTED:
               /* 打开设备并设为 BOOT 协议，键盘/手柄均可工作 */
               {
                  const hid_host_device_config_t dev_config = {
                     .callback = app_usb_hid_host_interface_callback,
                  };

                  ESP_ERROR_CHECK( hid_host_device_open(hid_device_handle, &dev_config) );
                  ESP_ERROR_CHECK( hid_class_request_set_idle(hid_device_handle, 0, 0) );
                  ESP_ERROR_CHECK( hid_class_request_set_protocol(hid_device_handle, HID_REPORT_PROTOCOL_BOOT) );
                  ESP_ERROR_CHECK( hid_host_device_start(hid_device_handle) );

                  if (dev->proto == HID_PROTOCOL_KEYBOARD) {
                     s_keyboard_handle = hid_device_handle;
                     subor_kb_enable(true);   /* 学习机 Subor 键盘启用 */
                     ESP_LOGI(TAG, "USB 键盘已连接 -> 学习机 Subor 键盘启用");
                  } else {
                     ESP_LOGI(TAG, "HID 设备已连接 (proto=%d)", dev->proto);
                  }
               }
               break;
            default:
               ESP_LOGE(TAG, "Not handled HID event!");
               break;
            }
        }
    }

    xQueueReset(hid_queue);
    vQueueDelete(hid_queue);

    hid_host_uninstall();

    vTaskDelete(NULL);
}

static void app_usb_hid_callback(hid_host_device_handle_t hid_device_handle, const hid_host_driver_event_t event, void *arg)
{
    const app_usb_hid_event_t msg = {
        .hid_device_handle = hid_device_handle,
        .event = event,
        .arg = arg
    };

    xQueueSend(hid_queue, &msg, 0);
}

static void app_hid_init(void)
{
   esp_err_t ret;

   subor_kb_init();   /* 初始化 Subor 学习机键盘状态（默认禁用，接键盘后启用） */

   /* USB HID host driver config */
   const hid_host_driver_config_t hid_host_driver_config = {
      .create_background_task = true,
      .task_priority = 5,
      .stack_size = 4096,
      .core_id = 0,
      .callback = app_usb_hid_callback,
   };

   ret = hid_host_install(&hid_host_driver_config);
   if (ret != ESP_OK) {
      ESP_LOGE(TAG, "USB HID install failed!");
      assert(0);
   }

   hid_queue = xQueueCreate(10, sizeof(app_usb_hid_event_t));
   xTaskCreate(&app_usb_hid_task, "hid_task", 4 * 1024, NULL, 2, NULL);
}
