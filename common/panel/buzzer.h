#pragma once
#include <stdint.h>

typedef enum {
    UI_BUZZ_BOOT = 0,
    UI_BUZZ_BAND,
    UI_BUZZ_MODE,
    UI_BUZZ_CHANNEL,
    UI_BUZZ_TOP,
    UI_BUZZ_BOTTOM,
    UI_BUZZ_COUNTDOWN,
    UI_BUZZ_RESET,
    UI_BUZZ_RX_1,               /* RX 扫描提示：1 架无人机响 1 声 */
    UI_BUZZ_RX_2,               /* RX 扫描提示：2 架响 2 声 */
    UI_BUZZ_RX_3,               /* RX 扫描提示：3 架及以上响 3 声 */
    UI_BUZZ_COUNT
} ui_buzz_t;

void buzzer_init(void);
void buzzer_play(ui_buzz_t sound);
void buzzer_enable(bool enabled);
