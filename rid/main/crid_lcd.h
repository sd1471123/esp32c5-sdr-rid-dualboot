#pragma once
#include "crid_config.h"

/* 启动本地 TFT 显示任务。接线与 5 个按键完全沿用 esp-sdr tft 分支。
 * 传入 crid-sim.c 的全局配置：面板可编辑时任务会就地修改它并写入 NVS。 */
void crid_lcd_start(cn_crid_config_t *config);
