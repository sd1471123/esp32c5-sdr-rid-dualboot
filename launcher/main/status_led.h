/* 板载 WS2812 状态灯：无屏幕核心板选择固件时用颜色做反馈。 */
#pragma once

#include <stdint.h>

void status_led_init(void);
void status_led_set(uint8_t r, uint8_t g, uint8_t b);
