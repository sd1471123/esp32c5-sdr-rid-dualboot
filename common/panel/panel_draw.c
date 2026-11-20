/* Shared panel helpers.  The LCD/keys/buzzer drivers are copied verbatim from
 * esp-sdr's tft branch so the wiring and key timings stay identical. */
#include "panel_draw.h"

#include "board_c5_lcd.h"
#include "st7789.h"
#include "buttons.h"
#include "buzzer.h"
#include "font5x7.h"
#include "font7x12.h"

#include "esp_heap_caps.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* One scaled font row, at most PANEL_W pixels, doubled at scale 3. */
#define ROW_PIXELS (PANEL_W * 3)
static uint16_t *s_row;

bool panel_init(void) {
    if (!s_row) {
        s_row = heap_caps_malloc(ROW_PIXELS * sizeof(uint16_t), MALLOC_CAP_8BIT);
        if (!s_row) return false;
    }
    if (!lcd_init()) return false;
    lcd_set_rotation(true);
    lcd_backlight_set(100);
    buttons_init();
    buzzer_init();
    return true;
}

void panel_backlight(uint8_t pct) { lcd_backlight_set(pct); }
void panel_fill(uint16_t color) { lcd_fill(color); }
void panel_rect(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t color) {
    if (!w || !h) return;
    lcd_fill_rect(x, y, w, h, color);
}
void panel_hline(unsigned x, unsigned y, unsigned w, uint16_t color) {
    panel_rect(x, y, w, 1, color);
}

unsigned panel_text(unsigned x, unsigned y, const char *s,
                    uint16_t fg, uint16_t bg, bool big, unsigned scale) {
    if (!s || !*s || !s_row) return 0;
    const unsigned gw = big ? FONT7X12_W : FONT5X7_W;
    const unsigned gh = big ? FONT7X12_H : FONT5X7_H;
    const unsigned cw = gw + 1;
    unsigned len = (unsigned)strlen(s);
    const unsigned screen_w = lcd_width();
    while (len && (x + len * cw * scale > screen_w)) len--;
    if (!len || x >= screen_w || y >= lcd_height()) return 0;

    unsigned width = len * cw * scale - scale;
    if (width > screen_w - x) width = screen_w - x;

    for (unsigned row = 0; row < gh; row++) {
        unsigned n = 0;
        for (unsigned i = 0; i < len; i++) {
            unsigned ch = (unsigned char)s[i];
            unsigned bits = 0;
            if (ch >= 32 && ch < 127)
                bits = big ? font7x12[ch - 32][row] : font5x7[ch - 32][row];
            for (unsigned col = 0; col < gw; col++) {
                uint16_t c = ((bits >> col) & 1) ? fg : bg;
                for (unsigned k = 0; k < scale; k++) s_row[n++] = c;
            }
            for (unsigned k = 0; k < scale && i + 1 < len; k++) s_row[n++] = bg;
        }
        if (n > width) n = width;
        /* The window is `scale` pixel rows high: repeat the expanded font row
         * vertically so every row of the window receives real pixels. */
        for (unsigned k = 1; k < scale; k++)
            memcpy(s_row + k * n, s_row, n * sizeof(uint16_t));
        lcd_set_window(x, y + row * scale, width, scale);
        lcd_pixels(s_row, n * scale);
    }
    return width;
}

unsigned panel_printf(unsigned x, unsigned y, uint16_t fg, uint16_t bg,
                      bool big, unsigned scale, const char *fmt, ...) {
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) return 0;
    return panel_text(x, y, line, fg, bg, big, scale);
}
