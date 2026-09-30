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

#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_partition.h"
#include "spi_flash_mmap.h"
#include "esp_heap_caps.h"

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

/* Game selection */
static void app_lvgl_game_selection(void);
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
      ESP_LOGI(TAG, "SD card mounted at %s", BSP_SD_MOUNT_POINT);
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

	/* Show selection of the game */
	app_lvgl_game_selection();

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

static void app_lvgl_btn_supermario(lv_event_t *e)
{
   lv_obj_del(game_selection);
   app_selected_game = "nesrom1";
}

static void app_lvgl_btn_donkeykong(lv_event_t *e)
{
   lv_obj_del(game_selection);
   app_selected_game = "nesrom2";
}

static void app_lvgl_btn_qbert(lv_event_t *e)
{
   lv_obj_del(game_selection);
   app_selected_game = "nesrom3";
}

static void app_lvgl_btn_sd(lv_event_t *e)
{
   const char *fn = (const char *)lv_event_get_user_data(e);
   snprintf(s_sel_path, sizeof(s_sel_path), "%s/%s", BSP_SD_MOUNT_POINT, fn);
   app_selected_game = s_sel_path;
   lv_obj_del(game_selection);
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

/* 三个按钮回调按分区顺序固定对应 nesrom1/2/3 */
static void (*const game_cbs[MAX_GAMES])(lv_event_t *) = {
    app_lvgl_btn_supermario, app_lvgl_btn_donkeykong, app_lvgl_btn_qbert
};

static void app_lvgl_game_selection(void)
{
    bsp_display_lock(0);
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
        DIR *d = opendir(BSP_SD_MOUNT_POINT);
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
        lv_label_set_text_static(none, "NO ROM FOUND\nPut .nes files on SD (/sdcard)\nor flash into nesrom1/2/3");
    } else if (main_group) {
        lv_indev_set_group(indev, main_group);
        ESP_LOGI(TAG, "Game selection ready (%d ROMs).", found);
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
