/* ESP32-C5 双固件开机选择菜单
 *
 * 接线 / 5 个按键 / ST7789 屏完全沿用 esp-sdr tft 分支：
 *   SCK=6 MOSI=7 CS=10 DC=4 RST=5 BL=23
 *   LEFT=0 RIGHT=1 B=8 M=9 ROT=24 BEEP=25
 *
 * 按 ← 进入 SDR 频谱仪，按 → 进入 C-RID。B / M 也可作为快捷键。
 * 进入的固件会在启动时把 otadata 清回 factory，所以下次开机又回到这里。
 *
 * 没有屏幕的核心板：开机 5 秒内按板载 BOOT（GPIO28）进入无屏选择，
 * 每次按键在两个固件之间切换，板载 RGB 灯（GPIO27）闪两下，
 * 红色 = SDR 频谱，蓝色 = C-RID；最后一次按键 5 秒后启动。
 * 有屏幕的设备不按 BOOT 就一直停在菜单。 */
#include "dual_boot.h"
#include "panel_draw.h"
#include "buttons.h"
#include "buzzer.h"
#include "status_led.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "launcher";

/* ESP32-C5-DevKitC-1：BOOT 键 GPIO28（低有效、内部上拉），RGB 灯 GPIO27。 */
#define BOOT_BTN_GPIO 28
#define BOOT_DEBOUNCE_US 30000
#define HEADLESS_WINDOW_US (5LL * 1000 * 1000)
#define MODE_RED_R 200
#define MODE_BLUE_B 200

static bool s_panel_ok;

static void draw_menu(void) {
    panel_fill(COL_BG);

    /* 标题条 */
    panel_rect(0, 0, PANEL_W, 34, COL_PANEL);
    panel_hline(0, 33, PANEL_W, COL_ACCENT);
    panel_text(10, 8, "ESP32-C5  DUAL BOOT", COL_TEXT, COL_PANEL, false, 2);
    panel_text(264, 12, "16MB", COL_ACCENT, COL_PANEL, false, 2);

    /* 选项卡片：大标题只放短名，说明放到下一行小字，
     * 这样 320 宽的屏不会被长标题截断（7x12 放大 2 倍是 16px/字符）。 */
    panel_rect(10, 42, 300, 78, COL_PANEL);
    panel_rect(10, 42, 4, 78, COL_ACCENT);
    panel_text(24, 52, "< SDR", COL_ACCENT, COL_PANEL, true, 2);
    panel_text(24, 80, "SPECTRUM ANALYZER", COL_DIM, COL_PANEL, false, 2);
    panel_text(24, 100, "2.4G / 5.8G   5 keys + ST7789", COL_DIM, COL_PANEL, false, 1);

    panel_rect(10, 126, 300, 78, COL_PANEL);
    panel_rect(10, 126, 4, 78, COL_AMBER);
    panel_text(24, 136, "> C-RID", COL_AMBER, COL_PANEL, true, 2);
    panel_text(24, 164, "REMOTE ID  TX/RX", COL_DIM, COL_PANEL, false, 2);
    panel_text(24, 184, "GB42590 / GB46750", COL_DIM, COL_PANEL, false, 1);

    panel_hline(0, 210, PANEL_W, COL_GRID);
    panel_text(10, 216, "LEFT/B: SDR   RIGHT/M: RID   ROT: light", COL_DIM, COL_BG, false, 1);
}

static void show_booting(const char *label, const char *name) {
    panel_fill(COL_BG);
    panel_text(20, 60, "BOOTING", COL_TEXT, COL_BG, true, 3);
    panel_text(20, 110, name, COL_ACCENT, COL_BG, true, 2);
    panel_printf(20, 160, COL_DIM, COL_BG, false, 2, "partition: %s", label);
    panel_text(20, 200, "Return to this menu: B + M hold 1.5s", COL_AMBER, COL_BG, false, 1);
}

/* 选中固件后统一走这里：上屏提示、切分区、重启；失败就留在菜单。 */
static void boot_selected(const char *label, const char *name) {
    if (s_panel_ok) show_booting(label, name);
    buzzer_play(UI_BUZZ_BOOT);
    vTaskDelay(pdMS_TO_TICKS(350));
    esp_err_t e = dualboot_boot_label(label);
    /* RID 有两个槽：A 槽镜像坏了就退到 B 槽。 */
    if (e != ESP_OK && strcmp(label, DUALBOOT_LABEL_RID_A) == 0) {
        e = dualboot_boot_label(DUALBOOT_LABEL_RID_B);
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "boot %s failed: %s", label, esp_err_to_name(e));
        if (s_panel_ok) {
            panel_printf(20, 200, COL_RED, COL_BG, false, 2,
                         "boot failed: %s", esp_err_to_name(e));
            vTaskDelay(pdMS_TO_TICKS(2500));
            draw_menu();
        }
        return;
    }
    esp_restart();
}

/* 红灯 = SDR 频谱，蓝灯 = C-RID；切换后闪两下。 */
static void blink_mode(bool sdr) {
    uint8_t r = sdr ? MODE_RED_R : 0;
    uint8_t b = sdr ? 0 : MODE_BLUE_B;
    for (int i = 0; i < 2; i++) {
        status_led_set(r, 0, b);
        vTaskDelay(pdMS_TO_TICKS(140));
        status_led_set(0, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(160));
    }
}

/* BOOT 键 30 ms 消抖，按住只上报一次。 */
static bool boot_button_pressed(void) {
    static bool down, fired;
    static int64_t down_at;
    int64_t now = esp_timer_get_time();
    if (gpio_get_level(BOOT_BTN_GPIO) == 0) {
        if (!down) {
            down = true;
            down_at = now;
        } else if (!fired && now - down_at >= BOOT_DEBOUNCE_US) {
            fired = true;
            return true;
        }
    } else {
        down = false;
        fired = false;
    }
    return false;
}

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* launcher 自己从 factory 启动，通常没有待确认状态；保留此调用是为了
     * 万一 bootloader 因其它 OTA 流程把它当成待确认镜像。 */
    dualboot_confirm_running();

    gpio_config_t boot_cfg = {
        .pin_bit_mask = 1ULL << BOOT_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&boot_cfg);
    status_led_init();
    status_led_set(0, 0, 0);

    s_panel_ok = panel_init();
    if (s_panel_ok) {
        /* lcd_init() leaves the PWM backlight at 0 %. Restore full brightness
         * now that the timer is really running (XTAL-clocked), otherwise the menu
         * would come up dark. */
        panel_backlight(100);
        draw_menu();
    }
    buzzer_play(UI_BUZZ_BOOT);
    ESP_LOGI(TAG, "boot menu ready, running from '%s' (panel %s)",
             dualboot_running_label(), s_panel_ok ? "ok" : "absent");

    const int64_t boot_us = esp_timer_get_time();
    bool headless = false;
    const char *sel_label = NULL;
    const char *sel_name = NULL;
    int64_t last_press_us = 0;

    for (;;) {
        int64_t now = esp_timer_get_time();

        if (!headless && s_panel_ok) {
            ui_button_event_t ev;
            buttons_poll(&ev);
            const char *label = NULL;
            const char *name = NULL;
            if (ev.short_press & (1u << UI_BTN_LEFT)) {
                label = DUALBOOT_LABEL_SDR;
                name = "SDR SPECTRUM";
            } else if (ev.short_press & (1u << UI_BTN_BAND)) {
                label = DUALBOOT_LABEL_SDR;
                name = "SDR SPECTRUM";
            } else if (ev.short_press & (1u << UI_BTN_RIGHT)) {
                label = DUALBOOT_LABEL_RID_A;
                name = "C-RID REMOTE ID";
            } else if (ev.short_press & (1u << UI_BTN_MODE)) {
                label = DUALBOOT_LABEL_RID_A;
                name = "C-RID REMOTE ID";
            } else if (ev.short_press & (1u << UI_BTN_ROT)) {
                static uint8_t bright = 100;
                /* Compare against the value: `bright ? 10 : 100` would stick at 10
                 * because 10 is itself truthy. */
                bright = (bright == 100) ? 10 : 100;
                panel_backlight(bright);
                buzzer_play(UI_BUZZ_BAND);
            }
            if (label) {
                boot_selected(label, name);
                continue;
            }
        }

        /* 无屏幕核心板：开机 5 秒内按 BOOT 进入选择；之后每按一次换一个
         * 固件，最后一次按键 5 秒后启动。有屏幕时不按 BOOT 就一直留在菜单。 */
        bool window = headless || (now - boot_us <= HEADLESS_WINDOW_US);
        if (window && boot_button_pressed()) {
            if (!headless) {
                headless = true;
                sel_label = DUALBOOT_LABEL_SDR;
                sel_name = "SDR SPECTRUM";
            } else if (strcmp(sel_label, DUALBOOT_LABEL_SDR) == 0) {
                sel_label = DUALBOOT_LABEL_RID_A;
                sel_name = "C-RID REMOTE ID";
            } else {
                sel_label = DUALBOOT_LABEL_SDR;
                sel_name = "SDR SPECTRUM";
            }
            ESP_LOGI(TAG, "headless select: %s", sel_label);
            blink_mode(strcmp(sel_label, DUALBOOT_LABEL_SDR) == 0);
            last_press_us = esp_timer_get_time();
        }

        if (headless && esp_timer_get_time() - last_press_us >= HEADLESS_WINDOW_US) {
            boot_selected(sel_label, sel_name);
            /* 启动失败：再等 5 秒重试，避免刷屏。 */
            last_press_us = esp_timer_get_time();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
