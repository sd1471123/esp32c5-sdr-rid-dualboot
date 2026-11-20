#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Shared ST7789 front-panel helpers for the launcher and the RID firmware.
 * The pin map, wiring and 5-key behaviour are identical to the esp-sdr tft
 * branch (see board_c5_lcd.h).  Coordinates are the 320x240 landscape space. */

#define PANEL_W 320
#define PANEL_H 240

/* RGB565 colours used by both UIs. */
#define COL_BG      0x0000  /* black */
#define COL_PANEL   0x0021  /* #000408 */
#define COL_GRID    0x0841  /* #080808 */
#define COL_DIM     0x5B2E  /* #5c6675 */
#define COL_TEXT    0xE75D  /* #e8edf4 */
#define COL_ACCENT  0x063F  /* #00c8ff */
#define COL_GREEN   0x6F76  /* #6ef0b0 */
#define COL_AMBER   0xFD89  /* #ffb347 */
#define COL_RED     0xFB4D  /* #ff6b6b */

bool panel_init(void);              /* LCD + keys + buzzer, landscape 320x240 */
void panel_backlight(uint8_t pct);
void panel_fill(uint16_t color);
void panel_rect(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t color);
void panel_hline(unsigned x, unsigned y, unsigned w, uint16_t color);

/* Text with a solid background. `big` selects the 7x12 font, otherwise 5x7.
 * `scale` (1..3) multiplies each glyph pixel.  Returns the drawn width. */
unsigned panel_text(unsigned x, unsigned y, const char *s,
                    uint16_t fg, uint16_t bg, bool big, unsigned scale);

/* printf-style convenience: formats into a 128-byte stack buffer. */
unsigned panel_printf(unsigned x, unsigned y, uint16_t fg, uint16_t bg,
                      bool big, unsigned scale, const char *fmt, ...);
