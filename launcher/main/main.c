/* ESP32-C5 双固件开机选择菜单
 *
 * 接线 / 5 个按键 / ST7789 屏完全沿用 esp-sdr tft 分支：
 *   SCK=6 MOSI=7 CS=10 DC=4 RST=5 BL=23
 *   LEFT=0 RIGHT=1 B=8 M=9 ROT=24 BEEP=25
 *
 * 按 ← 进入 SDR 频谱仪，按 → 进入 C-RID。B / M 也可作为快捷键。
 * 进入的固件会在启动时把 otadata 清回 factory，所以下次开机又回到这里。
 */
#include "dual_boot.h"
#include "panel_draw.h"
#include "buttons.h"
#include "buzzer.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "launcher";

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

    if (!panel_init()) {
        ESP_LOGE(TAG, "panel init failed - check the wiring");
        return;
    }
    /* lcd_init() leaves the PWM backlight at 0 %. Restore full brightness
     * now that the timer is really running (XTAL-clocked), otherwise the menu
     * would come up dark. */
    panel_backlight(100);
    draw_menu();
    buzzer_play(UI_BUZZ_BOOT);
    ESP_LOGI(TAG, "boot menu ready, running from '%s'", dualboot_running_label());

    for (;;) {
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
            show_booting(label, name);
            buzzer_play(UI_BUZZ_BOOT);
            vTaskDelay(pdMS_TO_TICKS(350));
            esp_err_t e = dualboot_boot_label(label);
            /* RID 有两个槽：A 槽镜像坏了就退到 B 槽。 */
            if (e != ESP_OK && strcmp(label, DUALBOOT_LABEL_RID_A) == 0) {
                e = dualboot_boot_label(DUALBOOT_LABEL_RID_B);
            }
            if (e != ESP_OK) {
                panel_printf(20, 200, COL_RED, COL_BG, false, 2,
                             "boot failed: %s", esp_err_to_name(e));
                vTaskDelay(pdMS_TO_TICKS(2500));
                draw_menu();
            } else {
                esp_restart();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}
