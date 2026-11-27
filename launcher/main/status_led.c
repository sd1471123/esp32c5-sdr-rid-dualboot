/* ESP32-C5-DevKitC-1 板载 WS2812（GPIO27），由 RMT 产生单总线时序。
 * 只给 launcher 用：无屏幕时红/蓝闪两下表示将要启动的固件。 */
#include "status_led.h"

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "status_led";

#define STATUS_LED_GPIO 27
#define LED_RESOLUTION_HZ 20000000u /* 50 ns per tick */
/* WS2812B timing in 50 ns ticks: bit period 25 ticks = 1.25 us. */
#define LED_T0H 8
#define LED_T0L 17
#define LED_T1H 16
#define LED_T1L 9

static rmt_channel_handle_t s_tx;
static rmt_encoder_handle_t s_enc;

void status_led_init(void) {
    const rmt_tx_channel_config_t channel_config = {
        .gpio_num = STATUS_LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = LED_RESOLUTION_HZ,
        .mem_block_symbols = 48,
        .trans_queue_depth = 2,
        .flags.invert_out = false,
        .flags.with_dma = false,
    };
    if (rmt_new_tx_channel(&channel_config, &s_tx) != ESP_OK) {
        ESP_LOGW(TAG, "no RMT channel for the status LED");
        s_tx = NULL;
        return;
    }
    const rmt_copy_encoder_config_t encoder_config; /* the struct is empty by design */
    if (rmt_new_copy_encoder(&encoder_config, &s_enc) != ESP_OK ||
        rmt_enable(s_tx) != ESP_OK) {
        ESP_LOGW(TAG, "status LED encoder unavailable");
        s_tx = NULL;
        return;
    }
    status_led_set(0, 0, 0);
}

void status_led_set(uint8_t r, uint8_t g, uint8_t b) {
    if (s_tx == NULL || s_enc == NULL) return;
    /* WS2812 wants green first, then red, then blue, MSB first. */
    const uint8_t bytes[3] = {g, r, b};
    rmt_symbol_word_t symbols[24];
    unsigned n = 0;
    for (unsigned i = 0; i < 3; ++i) {
        for (int bit = 7; bit >= 0; --bit) {
            const bool one = (bytes[i] >> bit) & 1;
            symbols[n].level0 = 1;
            symbols[n].duration0 = one ? LED_T1H : LED_T0H;
            symbols[n].level1 = 0;
            symbols[n].duration1 = one ? LED_T1L : LED_T0L;
            ++n;
        }
    }
    const rmt_transmit_config_t transmit_config = {.loop_count = 0};
    if (rmt_transmit(s_tx, s_enc, symbols, sizeof(symbols), &transmit_config) != ESP_OK) return;
    if (rmt_tx_wait_all_done(s_tx, 100) != ESP_OK) return;
    /* The LED latches after the line has been low for >50 us; a tick is plenty. */
    vTaskDelay(1);
}
