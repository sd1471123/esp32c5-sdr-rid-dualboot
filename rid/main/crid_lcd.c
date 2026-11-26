/* 合并工程新增：C-RID 本地 TFT 面板 + 本地配置。
 * 屏幕接线、5 个按键、蜂鸣器与 esp-sdr tft 分支完全一致（见 board_c5_lcd.h）。
 * 第 4 页是配置页：按键选择项目、改值、保存到 NVS 并立即生效。
 *
 * 刷新策略：只有在「切页 / 进入退出编辑」时才整屏重画；页内数字、光标变化
 * 一律局部重画对应区域。整屏清空 + 逐行 SPI 写入有可见的扫描过程，绝不能
 * 拿它去做 200ms 的周期性刷新。 */
#include "crid_lcd.h"

#include "board_c5_lcd.h"
#include "panel_draw.h"
#include "buttons.h"
#include "buzzer.h"
#include "dual_boot.h"

#include "crid_rx.h"
#include "crid_web.h"
#include "crid_wifi.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* crid-sim.c 里的收发应用函数，配置保存后复用它让发射/接收立即生效。 */
extern void crid_app_apply_mode(void);

static const char *TAG = "crid_lcd";
static cn_crid_config_t *s_cfg;
static cn_crid_config_t s_edit;          /* 编辑期的暂存副本，保存才写回 */
static uint8_t s_page;                   /* 0=status 1=targets 2=radar 3=about 4=config */
static uint8_t s_bright = 100;
static unsigned s_target;
static bool s_muted;
static unsigned lcd_edit_pos;            /* 字符串项编辑时的光标位置 */
static bool s_editing;                   /* EDIT 页内：false=编辑完态，true=改值态 */
static bool s_detail;                    /* 目标页：正在看某条航迹的详情 */
static crid_rx_track_t s_tracks[CRID_RX_MAX_TRACKS];

/* 蜂鸣器静音状态独立存在 crid 命名空间里，键名 "mute"。
 * 不塞进 cfg blob：单独一个 u8 键不会牵动配置的版本/结构，读写互不影响。 */
#define LCD_MUTE_NVS_NS  "crid"
#define LCD_MUTE_NVS_KEY "mute"

static void lcd_mute_save(bool muted) {
    nvs_handle_t h;
    if (nvs_open(LCD_MUTE_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_u8(h, LCD_MUTE_NVS_KEY, muted ? 1 : 0) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

static bool lcd_mute_load(void) {
    nvs_handle_t h;
    uint8_t v = 0;
    bool muted = false;
    if (nvs_open(LCD_MUTE_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, LCD_MUTE_NVS_KEY, &v) == ESP_OK) muted = (v != 0);
        nvs_close(h);
    }
    return muted;
}

/* ---- 配置项定义 ---- */
typedef enum {
    CFG_MODE = 0, CFG_STD, CFG_BAND, CFG_CHAN,
    CFG_UAS, CFG_NAME, CFG_OP, CFG_LAT, CFG_LON, CFG_ALT, CFG_SPEED, CFG_BEACON,
    CFG_COUNT
} cfg_id_t;

static const char *cfg_label(unsigned i) {
    static const char *const labels[CFG_COUNT] = {
        "MODE", "STANDARD", "BAND", "CHANNEL", "UAS ID", "DRONE NAME", "OPERATOR ID",
        "BASE LAT", "BASE LON", "BASE ALT", "SPEED", "BEACON MS"
    };
    return i < CFG_COUNT ? labels[i] : "?";
}

static bool cfg_is_string(unsigned i) {
    return i == CFG_UAS || i == CFG_NAME || i == CFG_OP;
}

static char *cfg_str(unsigned i) {
    switch (i) {
        case CFG_UAS:  return s_edit.uas_id;
        case CFG_NAME: return s_edit.drone_name;
        case CFG_OP:   return s_edit.operator_id;
        default:       return NULL;
    }
}

static size_t cfg_str_cap(unsigned i) {
    switch (i) {
        case CFG_UAS:  return sizeof(s_edit.uas_id);
        case CFG_NAME: return sizeof(s_edit.drone_name);
        case CFG_OP:   return sizeof(s_edit.operator_id);
        default:       return 0;
    }
}

static void cfg_value(unsigned i, char *buf, size_t cap) {
    switch (i) {
        case CFG_MODE:
            snprintf(buf, cap, "%s", crid_band_tx_enabled(&s_edit) ? "TX  transmit" : "RX  receive");
            break;
        case CFG_STD:
            snprintf(buf, cap, "%s", s_edit.rid_standard == RID_STD_GB46750 ? "GB46750-2025" : "GB42590-2023");
            break;
        case CFG_BAND:
            snprintf(buf, cap, "%s", s_edit.band == CRID_BAND_5G ? "5.8 GHz" : "2.4 GHz");
            break;
        case CFG_CHAN:
            snprintf(buf, cap, "%u", crid_band_channel(&s_edit));
            break;
        case CFG_UAS:   snprintf(buf, cap, "%s", s_edit.uas_id); break;
        case CFG_NAME:  snprintf(buf, cap, "%s", s_edit.drone_name); break;
        case CFG_OP:    snprintf(buf, cap, "%s", s_edit.operator_id); break;
        case CFG_LAT:   snprintf(buf, cap, "%.5f", s_edit.base_latitude); break;
        case CFG_LON:   snprintf(buf, cap, "%.5f", s_edit.base_longitude); break;
        case CFG_ALT:   snprintf(buf, cap, "%.0f m", s_edit.base_altitude_msl); break;
        case CFG_SPEED: snprintf(buf, cap, "%.1f m/s", s_edit.speed_horizontal); break;
        case CFG_BEACON:snprintf(buf, cap, "%u ms", (unsigned)s_edit.beacon_interval_ms); break;
        default: buf[0] = 0; break;
    }
}

static const char CHARS[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.@";

static unsigned char_index(char c) {
    for (unsigned i = 0; i < sizeof(CHARS) - 1; i++) if (CHARS[i] == c) return i;
    return 0;
}

static void cfg_change(unsigned i, int dir) {
    switch (i) {
        case CFG_MODE: {
            bool tx = !crid_band_tx_enabled(&s_edit);
            if (s_edit.band == CRID_BAND_5G) { s_edit.tx_5g = tx; s_edit.rx_5g = !tx; }
            else                             { s_edit.tx_2g = tx; s_edit.rx_2g = !tx; }
            break;
        }
        case CFG_STD:
            s_edit.rid_standard = (uint8_t)((s_edit.rid_standard + 1) % 2);
            break;
        case CFG_BAND:
            s_edit.band = (uint8_t)((s_edit.band + 1) % 2);
            break;
        case CFG_CHAN:
            if (s_edit.band == CRID_BAND_5G) {
                uint8_t ch = s_edit.channel_5g;
                if (ch < CRID_5G_CH_MIN || ch > CRID_5G_CH_MAX) ch = CRID_5G_CH_DEFAULT;
                ch = (uint8_t)(ch + (dir > 0 ? CRID_5G_CH_STEP : -CRID_5G_CH_STEP));
                if (ch > CRID_5G_CH_MAX) ch = CRID_5G_CH_MIN;
                if (ch < CRID_5G_CH_MIN) ch = CRID_5G_CH_MAX;
                s_edit.channel_5g = ch;
            } else {
                int ch = (int)s_edit.channel + (dir > 0 ? 1 : -1);
                if (ch < 1) ch = 13;
                if (ch > 13) ch = 1;
                s_edit.channel = (uint8_t)ch;
            }
            break;
        case CFG_UAS: case CFG_NAME: case CFG_OP: {
            char *s = cfg_str(i);
            size_t cap = cfg_str_cap(i);
            if (s && lcd_edit_pos < cap - 1) {
                unsigned n = sizeof(CHARS) - 1;
                unsigned idx = (char_index(s[lcd_edit_pos]) + (dir > 0 ? 1 : n - 1)) % n;
                s[lcd_edit_pos] = CHARS[idx];
            }
            break;
        }
        case CFG_LAT:
            s_edit.base_latitude += dir * 0.00001f;
            if (s_edit.base_latitude > 90) s_edit.base_latitude = -90;
            if (s_edit.base_latitude < -90) s_edit.base_latitude = 90;
            break;
        case CFG_LON:
            s_edit.base_longitude += dir * 0.00001f;
            if (s_edit.base_longitude > 180) s_edit.base_longitude = -180;
            if (s_edit.base_longitude < -180) s_edit.base_longitude = 180;
            break;
        case CFG_ALT: {
            float v = s_edit.base_altitude_msl + dir * 1.0f;
            if (v < -500) v = -500;
            if (v > 9000) v = 9000;
            s_edit.base_altitude_msl = v;
            break;
        }
        case CFG_SPEED: {
            float v = s_edit.speed_horizontal + dir * 0.5f;
            if (v < 0) v = 0;
            if (v > 50) v = 50;
            s_edit.speed_horizontal = v;
            break;
        }
        case CFG_BEACON: {
            int v = (int)s_edit.beacon_interval_ms + dir * 100;
            if (v < 200) v = 200;
            if (v > 60000) v = 60000;
            s_edit.beacon_interval_ms = (uint16_t)v;
            break;
        }
        default: break;
    }
}

static void cfg_save(void) {
    if (!s_cfg) return;
    uint8_t old_band = s_cfg->band;
    uint8_t old_ch = crid_band_channel(s_cfg);
    uint16_t old_beacon = s_cfg->beacon_interval_ms;

    crid_config_radio_mutex(&s_edit);
    s_edit.latitude = s_edit.base_latitude;
    s_edit.longitude = s_edit.base_longitude;
    s_edit.altitude_msl = s_edit.base_altitude_msl;
    s_edit.altitude_agl = s_edit.altitude_msl - s_edit.ground_altitude;

    uint8_t saved_counter = s_cfg->message_counter;
    *s_cfg = s_edit;
    s_cfg->message_counter = saved_counter;
    crid_config_save_nvs(s_cfg);
    ESP_LOGI(TAG, "config saved from TFT: mode=%s band=%u ch=%u std=%u",
             crid_band_tx_enabled(s_cfg) ? "TX" : "RX", s_cfg->band,
             crid_band_channel(s_cfg), s_cfg->rid_standard);

    bool band_changed = s_cfg->band != old_band;
    bool ap_changed = band_changed || crid_band_channel(s_cfg) != old_ch ||
                      s_cfg->beacon_interval_ms != old_beacon;
    if (ap_changed) {
        if (crid_band_rx_enabled(s_cfg)) crid_rx_stop();
        esp_err_t e;
        if (band_changed)
            e = crid_wifi_switch_band(s_cfg->band, s_cfg->ssid, s_cfg->ap_password,
                                      crid_band_channel(s_cfg), s_cfg->beacon_interval_ms);
        else
            e = crid_wifi_update_ap(s_cfg->ssid, s_cfg->ap_password,
                                    crid_band_channel(s_cfg), s_cfg->beacon_interval_ms);
        if (e != ESP_OK) ESP_LOGE(TAG, "apply AP after TFT config failed: %s", esp_err_to_name(e));
    }
    crid_app_apply_mode();
    buzzer_play(UI_BUZZ_TOP);
}

static const char *proto_name(uint8_t p) {
    switch (p) {
        case RX_PROTO_ASTM:    return "ASTM";
        case RX_PROTO_GB42590: return "GB42590";
        case RX_PROTO_GB46750: return "GB46750";
        default:               return "UNKNOWN";
    }
}

/* ---- 渲染：整页 ---- */

/* 标题栏高度。原来 34 px 占掉 240 px 屏的 1/7，改成 22 px：
 * 标题用 7x12 小字，一行就够，把纵向空间还给正文。 */
#define HDR_H 22

static void render_header(const char *title, uint16_t accent) {
    panel_rect(0, 0, PANEL_W, HDR_H, COL_PANEL);
    panel_hline(0, HDR_H - 1, PANEL_W, accent);
    panel_text(8, 4, title, accent, COL_PANEL, false, 2);
    /* 右上角不再常驻 B%：最小号灰字在 320 宽标题上会和长标题重叠、看不清。
     * 背光档位改到「关于页」用大字显示；切换时明暗变化 + 蜂鸣即是反馈。 */
}

/* ---- 通用「标签固定 + 只刷数值」行 ----
 * 每行记住标签、数值和上次画到屏上的内容：
 *   标签没变 → 只清右侧数值区并重画数值；
 *   标签变了（列表换了目标、配置光标移过来）→ 整行清掉重画。
 * 周期刷新走 rows_refresh()，屏幕主体静止，只有真正变的数值在动。 */
#define ROW_MAX       20
#define ROW_TEXT_MAX  26

typedef struct {
    int16_t  y;
    uint8_t  rh;        /* 重画该行时清除的高度 */
    uint8_t  scale;     /* 1 = 5x7 原尺寸, 2 = 放大 2 倍 */
    int16_t  xval;      /* 数值列 x */
    uint16_t lcolor;
    uint16_t vcolor;
    uint16_t bg;
    bool     drawn;     /* 是否已经画过 */
    char     label[ROW_TEXT_MAX];
    char     val[ROW_TEXT_MAX];
    char     plabel[ROW_TEXT_MAX];
    char     pval[ROW_TEXT_MAX];
} lcd_row_t;

static lcd_row_t s_rows[ROW_MAX];
static unsigned  s_row_count;
static unsigned  s_row_cursor;

static void rows_begin(unsigned n) {
    s_row_count = (n > ROW_MAX) ? ROW_MAX : n;
    s_row_cursor = 0;
}

static lcd_row_t *row_add(int y, int rh, int scale, int xval,
                          uint16_t lcolor, uint16_t vcolor, uint16_t bg) {
    if (s_row_cursor >= s_row_count) return NULL;
    lcd_row_t *r = &s_rows[s_row_cursor++];
    r->y = (int16_t)y;
    r->rh = (uint8_t)rh;
    r->scale = (uint8_t)scale;
    r->xval = (int16_t)xval;
    r->lcolor = lcolor;
    r->vcolor = vcolor;
    r->bg = bg;
    return r;
}

static void row_label(lcd_row_t *r, const char *text) {
    if (r) snprintf(r->label, sizeof(r->label), "%s", text);
}

static void row_value(lcd_row_t *r, const char *fmt, ...) {
    if (!r) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->val, sizeof(r->val), fmt, ap);
    va_end(ap);
}

static void row_paint(lcd_row_t *r, bool force) {
    if (!r) return;
    bool label_changed = force || !r->drawn || strcmp(r->label, r->plabel) != 0;
    if (!label_changed && strcmp(r->val, r->pval) == 0) return;

    if (label_changed) {
        panel_rect(0, r->y, PANEL_W, r->rh, r->bg);
        panel_text(6, r->y, r->label, r->lcolor, r->bg, false, r->scale);
    } else {
        panel_rect(r->xval, r->y, PANEL_W - r->xval, r->rh, r->bg);
    }
    panel_text(r->xval, r->y, r->val, r->vcolor, r->bg, false, r->scale);

    memcpy(r->plabel, r->label, sizeof(r->plabel));
    memcpy(r->pval, r->val, sizeof(r->pval));
    r->drawn = true;
}

/* 全部重画（切页 / 布局变化时用）。 */
static void rows_paint(void) {
    for (unsigned i = 0; i < s_row_count; i++) row_paint(&s_rows[i], true);
}

/* 只重画内容变了的行（周期刷新用）。 */
static void rows_refresh(void) {
    for (unsigned i = 0; i < s_row_count; i++) row_paint(&s_rows[i], false);
}

/* 状态页：9 行，全部"标签固定 + 数值刷新"。
 * 标签列宽 5 字符（60px），数值列 x = 70。行高 20px，y 26..186。 */
#define STATUS_ROW_H 20
#define STATUS_XVAL  70

static char s_status_title[24];

static void status_build_rows(void) {
    const cn_crid_config_t *c = s_cfg;
    rows_begin(9);
    if (!c) return;
    const char *band = (c->band == CRID_BAND_5G) ? "5.8G" : "2.4G";
    lcd_row_t *r;
    int y = 26;

    r = row_add(y, STATUS_ROW_H, 2, STATUS_XVAL, COL_DIM, COL_ACCENT, COL_BG);
    row_label(r, "BAND ");
    row_value(r, "%s  CH %u", band, crid_band_channel(c));
    y += STATUS_ROW_H;

    r = row_add(y, STATUS_ROW_H, 2, STATUS_XVAL, COL_DIM, COL_AMBER, COL_BG);
    row_label(r, "STD  ");
    row_value(r, "%s", c->rid_standard == RID_STD_GB46750 ? "GB46750-2025" : "GB42590-2023");
    y += STATUS_ROW_H;

    r = row_add(y, STATUS_ROW_H, 2, STATUS_XVAL, COL_DIM, COL_TEXT, COL_BG);
    row_label(r, "UAS  ");
    row_value(r, "%.20s", c->uas_id);
    y += STATUS_ROW_H;

    r = row_add(y, STATUS_ROW_H, 2, STATUS_XVAL, COL_DIM, COL_TEXT, COL_BG);
    row_label(r, "NAME ");
    row_value(r, "%.20s", c->drone_name);
    y += STATUS_ROW_H;

    r = row_add(y, STATUS_ROW_H, 2, STATUS_XVAL, COL_DIM, COL_DIM, COL_BG);
    row_label(r, "OP   ");
    row_value(r, "%.20s", c->operator_id);
    y += STATUS_ROW_H;

    r = row_add(y, STATUS_ROW_H, 2, STATUS_XVAL, COL_DIM, COL_DIM, COL_BG);
    row_label(r, "POS  ");
    row_value(r, "%.5f,%.5f", c->latitude, c->longitude);
    y += STATUS_ROW_H;

    r = row_add(y, STATUS_ROW_H, 2, STATUS_XVAL, COL_DIM, COL_DIM, COL_BG);
    row_label(r, "ALT  ");
    row_value(r, "%.1fm  SPD %.1f", c->altitude_msl, c->speed_horizontal);
    y += STATUS_ROW_H;

    r = row_add(y, STATUS_ROW_H, 2, STATUS_XVAL, COL_DIM, COL_DIM, COL_BG);
    row_label(r, "HDG  ");
    row_value(r, "%.0f   ST %u", c->heading, (unsigned)c->status);
    y += STATUS_ROW_H;

    r = row_add(y, STATUS_ROW_H, 2, STATUS_XVAL, COL_DIM, COL_ACCENT, COL_BG);
    row_label(r, "AP   ");
    row_value(r, "%.18s", c->ssid);
}

static void render_status(void) {
    const cn_crid_config_t *c = s_cfg;
    if (!c) return;
    bool tx = crid_band_tx_enabled(c);
    bool rx = crid_band_rx_enabled(c);
    const char *title = tx ? "C-RID  TX" : (rx ? "C-RID  RX" : "C-RID  IDLE");

    /* 标题已写明 TX/RX/IDLE，正文不再重复一行 MODE，省下的高度给行距。 */
    render_header(title, tx ? COL_GREEN : COL_ACCENT);
    snprintf(s_status_title, sizeof(s_status_title), "%s", title);

    status_build_rows();
    rows_paint();
    panel_hline(0, 208, PANEL_W, COL_GRID);
    panel_text(8, 214, "LR: page   R+LR: light   L+R: mute", COL_DIM, COL_BG, false, 1);
}

/* 每秒比对一次：只重画数值变了的行；收发模式变了才动标题栏。 */
static void refresh_status_live(void) {
    const cn_crid_config_t *c = s_cfg;
    if (!c) return;
    bool tx = crid_band_tx_enabled(c);
    bool rx = crid_band_rx_enabled(c);
    const char *title = tx ? "C-RID  TX" : (rx ? "C-RID  RX" : "C-RID  IDLE");
    if (strcmp(title, s_status_title) != 0) {
        snprintf(s_status_title, sizeof(s_status_title), "%s", title);
        render_header(title, tx ? COL_GREEN : COL_ACCENT);
    }
    status_build_rows();
    rows_refresh();
}

/* ---- 目标列表页 ---- */
#define TRACK_ROW_H 52      /* 一条航迹占 3 行（17px 行距） */
#define TRACK_LIST_Y 66
#define TRACK_SLOTS  3

static int s_slots_drawn;   /* 上次画的槽位数：数量变了要整块重画 */

static void targets_build_rows(void) {
    int n = crid_rx_get_tracks(s_tracks);
    int online = 0;
    for (int i = 0; i < n; i++) if (s_tracks[i].active) online++;
    uint32_t total = 0, detected = 0, parsed = 0, overflow = 0;
    crid_rx_get_stats(&total, &detected, &parsed, &overflow);

    unsigned shown = 0;
    if (n > 0) {
        shown = (unsigned)n < TRACK_SLOTS ? (unsigned)n : TRACK_SLOTS;
        if (s_target >= (unsigned)n) s_target = 0;
    }
    rows_begin(2 + shown * 3);

    lcd_row_t *r = row_add(26, 20, 2, 78, COL_DIM, COL_TEXT, COL_BG);
    row_label(r, "TRACK");
    row_value(r, "%d ON / %d ALL", online, n);

    r = row_add(48, 12, 1, 48, COL_DIM, COL_DIM, COL_BG);
    row_label(r, "PKT  ");
    row_value(r, "%u  HIT %u  OVF %u", (unsigned)total, (unsigned)detected, (unsigned)overflow);

    for (unsigned slot = 0; slot < shown; slot++) {
        unsigned idx = (s_target + slot) % (unsigned)n;
        const crid_rx_track_t *t = &s_tracks[idx];
        int y = TRACK_LIST_Y + (int)slot * TRACK_ROW_H;
        char mac[16];
        snprintf(mac, sizeof(mac), "%c%02X%02X%02X%02X",
                 t->active ? '>' : ' ', (unsigned)t->mac[0], (unsigned)t->mac[1],
                 (unsigned)t->mac[2], (unsigned)t->mac[3]);

        /* 第一行：MAC 当固定标签，RSSI 当数值。换目标时标签变 -> 整行重画。 */
        r = row_add(y, 16, 2, 126, t->active ? COL_GREEN : COL_DIM,
                    t->active ? COL_GREEN : COL_DIM, COL_BG);
        row_label(r, mac);
        row_value(r, "%d dBm", t->rssi);

        r = row_add(y + 17, 16, 2, 42, COL_DIM, COL_TEXT, COL_BG);
        row_label(r, "ID");
        row_value(r, "%.14s", t->uas_id[0] ? t->uas_id : t->drone_name);

        r = row_add(y + 34, 14, 2, 96, COL_DIM, COL_DIM, COL_BG);
        row_label(r, proto_name(t->protocol));
        row_value(r, "%.0fm %.0fm/s", t->altitude_geo, t->speed_horizontal);
    }
}

/* 整页（或整块内容区）重画：切页 / 翻航迹 / 槽位数量变化时用。 */
static void render_targets_page(void) {
    int n = crid_rx_get_tracks(s_tracks);
    render_header("C-RID  RX TARGETS", COL_ACCENT);
    panel_rect(0, HDR_H, PANEL_W, 240 - HDR_H, COL_BG);
    targets_build_rows();
    rows_paint();
    panel_hline(0, 62, PANEL_W, COL_GRID);
    if (n <= 0) {
        s_slots_drawn = 0;
        panel_text(8, 100, "No RID frames received yet", COL_DIM, COL_BG, false, 2);
        panel_text(8, 130, "Check band / channel in web UI", COL_DIM, COL_BG, false, 2);
    } else {
        int shown = (n < TRACK_SLOTS) ? n : TRACK_SLOTS;
        s_slots_drawn = shown;
        /* 槽位之间加一条暗色细线：三行一组本来就密，没有分隔会看成一片。 */
        for (int slot = 1; slot < shown; slot++)
            panel_hline(0, TRACK_LIST_Y + slot * TRACK_ROW_H - 2, PANEL_W, COL_GRID);
    }
    panel_printf(8, 228, COL_DIM, COL_BG, false, 1,
                 "M+LR: target   B/M tap: detail   LR: page");
}

/* 每秒：槽位数量变了才整块重画，否则只刷变了的数值。 */
static void refresh_targets_live(void) {
    int n = crid_rx_get_tracks(s_tracks);
    int want = (n <= 0) ? 0 : ((n < TRACK_SLOTS) ? n : TRACK_SLOTS);
    if (want != s_slots_drawn) {
        render_targets_page();
        return;
    }
    targets_build_rows();
    rows_refresh();
}

/* ---- 目标详情页：把一条航迹的全部可读字段铺满整屏 ---- */

/* ---- 详情页：标签固定、只刷数值 ----
 * 字号用 5x7 放大 2 倍（10x14），行高 16px，12 行铺满内容区。
 * 标签（左侧 6 字符）只在进页/切目标时画一次；之后每秒只重画"值变了"的那几行右侧区域。
 * 这样屏幕主体是静止的，只有坐标/高度/速度这些数字在跳。 */
#define DETAIL_ROWS 12
#define DETAIL_Y0   26
#define DETAIL_RH   16
#define DETAIL_LX   6
#define DETAIL_VX   78                  /* 6 字标签 × 12px + 左边距 */
#define DETAIL_VMAX 21                  /* 值最多 20 字符（78+20*12=318） */

static const char *const k_detail_labels[DETAIL_ROWS] = {
    "MAC  ", "RSSI ", "PROTO", "MSG  ", "UAS  ", "NAME ",
    "OP   ", "POS  ", "ALT  ", "SPD  ", "ST   ", "OPPOS",
};

static const uint16_t k_detail_colors[DETAIL_ROWS] = {
    COL_ACCENT, COL_GREEN, COL_AMBER, COL_TEXT, COL_TEXT, COL_TEXT,
    COL_DIM, COL_TEXT, COL_TEXT, COL_TEXT, COL_DIM, COL_DIM,
};

static char s_dv[DETAIL_ROWS][DETAIL_VMAX];       /* 当前值 */
static char s_dv_prev[DETAIL_ROWS][DETAIL_VMAX];  /* 上次画到屏上的值 */
static char s_detail_title[40];                   /* 上次画到标题栏的字符串 */

static void detail_build_values(const crid_rx_track_t *t) {
    uint32_t age_s = 0;
    if (t->last_seen_ms) {
        uint32_t now_ms = esp_log_timestamp();
        if (now_ms > t->last_seen_ms) age_s = (now_ms - t->last_seen_ms) / 1000u;
    }
    snprintf(s_dv[0], DETAIL_VMAX, "%02X%02X%02X%02X%02X%02X",
             (unsigned)t->mac[0], (unsigned)t->mac[1], (unsigned)t->mac[2],
             (unsigned)t->mac[3], (unsigned)t->mac[4], (unsigned)t->mac[5]);
    snprintf(s_dv[1], DETAIL_VMAX, "%d dBm  CH%u", t->rssi, (unsigned)t->channel);
    snprintf(s_dv[2], DETAIL_VMAX, "%s", proto_name(t->protocol));
    snprintf(s_dv[3], DETAIL_VMAX, "%u  %us", (unsigned)t->msg_count, (unsigned)age_s);
    snprintf(s_dv[4], DETAIL_VMAX, "%.20s", t->uas_id[0] ? t->uas_id : "--");
    snprintf(s_dv[5], DETAIL_VMAX, "%.20s", t->drone_name[0] ? t->drone_name : "--");
    snprintf(s_dv[6], DETAIL_VMAX, "%.20s", t->operator_id[0] ? t->operator_id : "--");
    snprintf(s_dv[7], DETAIL_VMAX, "%.5f,%.5f", t->latitude, t->longitude);
    if (t->has_agl)
        snprintf(s_dv[8], DETAIL_VMAX, "%.1fm  AGL %.1fm", t->altitude_geo, t->altitude_agl);
    else
        snprintf(s_dv[8], DETAIL_VMAX, "%.1fm  AGL --", t->altitude_geo);
    snprintf(s_dv[9], DETAIL_VMAX, "H%.1f V%.1f HDG%.0f",
             t->speed_horizontal, t->speed_vertical, t->direction);
    snprintf(s_dv[10], DETAIL_VMAX, "ST%u UA%u %s",
             (unsigned)t->status, (unsigned)t->ua_type, t->has_acc ? "ACC" : "--");
    if (t->has_op_loc)
        snprintf(s_dv[11], DETAIL_VMAX, "%.5f,%.5f", t->op_latitude, t->op_longitude);
    else
        snprintf(s_dv[11], DETAIL_VMAX, "--");
}

/* 左侧标签：进页 / 切目标时画一次，之后不再动。 */
static void render_detail_labels(void) {
    for (unsigned i = 0; i < DETAIL_ROWS; i++)
        panel_text(DETAIL_LX, DETAIL_Y0 + i * DETAIL_RH, k_detail_labels[i],
                   COL_DIM, COL_BG, false, 2);
}

/* 右侧数值：只有和上次不同（或 force）才清那一行并重画。 */
static void render_detail_values(bool force) {
    for (unsigned i = 0; i < DETAIL_ROWS; i++) {
        if (!force && strcmp(s_dv[i], s_dv_prev[i]) == 0) continue;
        memcpy(s_dv_prev[i], s_dv[i], DETAIL_VMAX);
        unsigned y = DETAIL_Y0 + i * DETAIL_RH;
        panel_rect(DETAIL_VX, y, PANEL_W - DETAIL_VX, DETAIL_RH, COL_BG);
        panel_text(DETAIL_VX, y, s_dv[i], k_detail_colors[i], COL_BG, false, 2);
    }
}

/* 每秒调一次：只重画变了的数值行；标题（目标序号/在线状态）变了才动标题栏。 */
static void refresh_detail_live(void) {
    int n = crid_rx_get_tracks(s_tracks);
    if (n <= 0) return;
    if (s_target >= (unsigned)n) s_target = 0;
    const crid_rx_track_t *t = &s_tracks[s_target];

    char title[40];
    snprintf(title, sizeof(title), "TARGET %u/%d %s",
             s_target + 1, n, t->active ? "ON" : "OFF");
    if (strcmp(title, s_detail_title) != 0) {
        memcpy(s_detail_title, title, sizeof(s_detail_title));
        render_header(title, t->active ? COL_GREEN : COL_DIM);
    }

    detail_build_values(t);
    render_detail_values(false);
}

/* 整页详情：标题 + 标签 + 全部数值。切页/切目标时用。 */
static void render_target_detail(void) {
    int n = crid_rx_get_tracks(s_tracks);
    if (n <= 0) {
        render_header("TARGET  --", COL_DIM);
        panel_text(8, 100, "No target selected", COL_DIM, COL_BG, false, 2);
        return;
    }
    if (s_target >= (unsigned)n) s_target = 0;
    const crid_rx_track_t *t = &s_tracks[s_target];

    char title[40];
    snprintf(title, sizeof(title), "TARGET %u/%d %s",
             s_target + 1, n, t->active ? "ON" : "OFF");
    memcpy(s_detail_title, title, sizeof(s_detail_title));
    render_header(title, t->active ? COL_GREEN : COL_DIM);

    render_detail_labels();
    detail_build_values(t);
    memset(s_dv_prev, 0, sizeof(s_dv_prev));
    render_detail_values(true);

    panel_hline(0, 222, PANEL_W, COL_GRID);
    panel_text(8, 226, "LR: target   B/M: back", COL_DIM, COL_BG, false, 1);
}

/* 关于页：6 行标签固定 + 数值刷新（LIGHT 会随背光切换变）。
 * 下面的按键提示是静态文字，只在整页渲染时画一次。 */
#define ABOUT_ROW_H 18
#define ABOUT_XVAL  66

static void about_build_rows(void) {
    const esp_app_desc_t *desc = esp_app_get_description();
    rows_begin(6);
    lcd_row_t *r;
    int y = 26;

    r = row_add(y, ABOUT_ROW_H, 2, ABOUT_XVAL, COL_DIM, COL_TEXT, COL_BG);
    row_label(r, "VER  ");
    row_value(r, "%s", (desc && desc->version[0]) ? desc->version : "?");
    y += ABOUT_ROW_H;

    r = row_add(y, ABOUT_ROW_H, 2, ABOUT_XVAL, COL_DIM, COL_ACCENT, COL_BG);
    row_label(r, "PART ");
    row_value(r, "%s", dualboot_running_label());
    y += ABOUT_ROW_H;

    r = row_add(y, ABOUT_ROW_H, 2, ABOUT_XVAL, COL_DIM, COL_TEXT, COL_BG);
    row_label(r, "WEB  ");
    row_value(r, "192.168.4.1");
    y += ABOUT_ROW_H;

    r = row_add(y, ABOUT_ROW_H, 2, ABOUT_XVAL, COL_DIM, COL_DIM, COL_BG);
    row_label(r, "BT   ");
    row_value(r, "Web Bluetooth");
    y += ABOUT_ROW_H;

    r = row_add(y, ABOUT_ROW_H, 2, ABOUT_XVAL, COL_DIM, COL_DIM, COL_BG);
    row_label(r, "OTA  ");
    row_value(r, "rid_a <-> rid_b");
    y += ABOUT_ROW_H;

    r = row_add(y, ABOUT_ROW_H, 2, ABOUT_XVAL, COL_DIM, COL_ACCENT, COL_BG);
    row_label(r, "LIGHT");
    row_value(r, "%u%%", (unsigned)s_bright);
}

static void render_about(void) {
    render_header("C-RID  ABOUT", COL_AMBER);
    about_build_rows();
    rows_paint();
    panel_hline(0, 146, PANEL_W, COL_GRID);
    panel_text(8, 154, "HOLD B+M -> menu", COL_AMBER, COL_BG, false, 2);
    panel_text(8, 178, "R+LR:light  L+R:mute", COL_DIM, COL_BG, false, 1);
    panel_text(8, 200, "CFG: Btap enters EDIT", COL_DIM, COL_BG, false, 1);
}

/* 关于页只有 LIGHT 会变，其余是常量。 */
static void refresh_about_live(void) {
    about_build_rows();
    rows_refresh();
}

static unsigned s_cfg_cursor;
static int s_edit_item = -1;    /* -1 = 列表态，>=0 = 正在编辑该项 */

/* 配置列表：12 行，标签里带光标前缀，光标移到哪行就只有那两行变（旧行/新行）。
 * 行高 16px、字号 1 倍，标签列固定 14 字符宽，数值列 x = 96。 */
#define CFG_ROW_Y0 26
#define CFG_ROW_H  16
#define CFG_XVAL   96

static void config_list_build_rows(void) {
    rows_begin(CFG_COUNT);
    char val[40];
    char lab[28];
    for (unsigned i = 0; i < CFG_COUNT; i++) {
        bool sel = (i == s_cfg_cursor);
        cfg_value(i, val, sizeof(val));
        snprintf(lab, sizeof(lab), "%c %-12s", sel ? '>' : ' ', cfg_label(i));
        lcd_row_t *r = row_add(CFG_ROW_Y0 + (int)i * CFG_ROW_H, 15, 1, CFG_XVAL,
                               sel ? COL_AMBER : COL_DIM,
                               sel ? COL_AMBER : COL_DIM,
                               sel ? COL_PANEL : COL_BG);
        row_label(r, lab);
        row_value(r, "%s", val);
    }
}

static void render_config_list(void) {
    config_list_build_rows();
    rows_paint();
}

/* EDIT 页里"标签 + 当前值"这一块，切项和改值时只重画这里，不动整屏。 */
static void render_edit_fields(void) {
    char val[40];
    panel_rect(0, 34, PANEL_W, 80, COL_BG);
    panel_printf(8, 40, COL_ACCENT, COL_BG, false, 2, "%s", cfg_label((unsigned)s_edit_item));
    panel_hline(0, 66, PANEL_W, COL_GRID);
    cfg_value((unsigned)s_edit_item, val, sizeof(val));
    panel_printf(8, 82, COL_TEXT, COL_BG, false, 2, "%s", val);
    /* 字符串项在编辑完态也要显示光标：←/→ 就是移这个光标 */
    if (cfg_is_string((unsigned)s_edit_item))
        panel_rect(8 + lcd_edit_pos * 12, 102, 10, 2, COL_AMBER);
}

static void render_config(void) {
    if (s_edit_item < 0) {
        render_header("C-RID  CONFIG", COL_GREEN);
        render_config_list();
        panel_printf(8, 228, COL_DIM, COL_BG, false, 1,
                     "Btap:enter EDIT  LR:next  M:save+exit");
    } else if (!s_editing) {
        /* 编辑完态：B 点按切下一项，←/→ 移光标(字符串)/改数值，长按 B 1s 进改值 */
        render_header("EDIT  CONFIG", COL_AMBER);
        render_edit_fields();
        if (cfg_is_string((unsigned)s_edit_item))
            panel_text(8, 136, "LR:move char  B:next item", COL_DIM, COL_BG, false, 2);
        else
            panel_text(8, 136, "LR:change value B:next", COL_DIM, COL_BG, false, 2);
        panel_text(8, 168, "Bhold1s:edit  M:save+exit", COL_DIM, COL_BG, false, 2);
        panel_text(8, 208, "HOLD B+M: abort + menu", COL_DIM, COL_BG, false, 1);
    } else {
        /* 改值态：←/→ 改当前字符/数值，M 退回编辑完态 */
        render_header("EDIT  VALUE", COL_GREEN);
        render_edit_fields();
        if (cfg_is_string((unsigned)s_edit_item))
            panel_text(8, 136, "LR:change char  B:next char", COL_DIM, COL_BG, false, 2);
        else
            panel_text(8, 136, "LR:change value", COL_DIM, COL_BG, false, 2);
        panel_text(8, 168, "M:done -> browse", COL_GREEN, COL_BG, false, 2);
        panel_text(8, 208, "HOLD B+M: abort + menu", COL_DIM, COL_BG, false, 1);
    }
}

/* ---- 雷达页 ----
 * 设备没有陀螺仪/罗盘，所以方位固定「上北下南、右东左西」，不随设备转动。
 * 本机坐标从手机上报的 /api/selfpos 取（App 连蓝牙自动报，或网页点「使用
 * 手机定位」）；坐标只在 RAM 中，超过 30 秒未更新按过期处理。
 *
 * 屏上分三块：
 *   - 静态表盘：4 圈同心圆 + 十字线 + 方位字，只在进页时画一次；
 *   - 目标光点：每次刷新只擦/画位置或颜色变化的小方块；
 *   - 右侧信息列：量程、GPS 状态、在线数量、选中目标，逐行比对后才重画。 */
#define RADAR_CX       104
#define RADAR_CY       128
#define RADAR_R        84
#define RADAR_X0       202      /* 右侧信息列起点 */
#define RADAR_STALE_MS 30000    /* 手机定位超过 30 秒没更新就标为过期 */

static const uint16_t k_radar_ranges[] = { 200, 500, 1000, 2000, 5000 };
static uint8_t s_radar_rng;     /* 0=自动，1..5 对应 k_radar_ranges */
static uint16_t s_radar_auto = 500;  /* 自动量程当前档：带滞回，避免目标在边界来回跳 */

/* 量程记忆：和静音一样，在 crid 命名空间里单独放一个字节键，不牵动配置 blob。
 * 只在用户按 B/M 切档时写一次，NVS 磨损可以忽略。 */
#define LCD_RADAR_NVS_KEY "radar"

static void radar_rng_save(uint8_t v) {
    nvs_handle_t h;
    if (nvs_open(LCD_MUTE_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_u8(h, LCD_RADAR_NVS_KEY, v) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

static uint8_t radar_rng_load(void) {
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(LCD_MUTE_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, LCD_RADAR_NVS_KEY, &v) != ESP_OK || v > 5) v = 0;
        nvs_close(h);
    }
    return v;
}

/* 切下一档量程并立即落盘（B/M 两个键共用） */
static void radar_rng_next(void) {
    s_radar_rng = (uint8_t)((s_radar_rng + 1) % 6);
    radar_rng_save(s_radar_rng);
}

typedef struct {
    int16_t  x, y;
    uint8_t  sz;
    uint16_t col;
    bool     drawn;
} radar_blip_t;

static radar_blip_t s_radar_blips[CRID_RX_MAX_TRACKS];

static struct {
    char     text[20];
    uint16_t col;
} s_radar_info[7];

#define RADAR_IRND(v) ((int)((v) >= 0 ? (v) + 0.5f : (v) - 0.5f))

static void radar_blip_erase(radar_blip_t *b) {
    int x = b->x - 1, y = b->y - 1;
    int w = b->sz + 2, h = b->sz + 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > PANEL_W) w = PANEL_W - x;
    if (y + h > PANEL_H) h = PANEL_H - y;
    if (w > 0 && h > 0) panel_rect((unsigned)x, (unsigned)y, (unsigned)w, (unsigned)h, COL_BG);
    b->drawn = false;
}

/* 右侧信息列：同一行文字/颜色都没变就完全不动屏 */
static void radar_info_line(unsigned slot, unsigned y, uint16_t color, const char *text) {
    if (slot >= sizeof(s_radar_info) / sizeof(s_radar_info[0])) return;
    if (s_radar_info[slot].col == color && strcmp(s_radar_info[slot].text, text) == 0) return;
    snprintf(s_radar_info[slot].text, sizeof(s_radar_info[slot].text), "%s", text);
    s_radar_info[slot].col = color;
    panel_rect(RADAR_X0, y, PANEL_W - RADAR_X0, 10, COL_BG);
    if (text[0]) panel_text(RADAR_X0, y, text, color, COL_BG, false, 1);
}

/* 只刷新动态部分：GPS 状态、量程、计数、选中信息、目标光点 */
static void radar_refresh(void) {
    crid_selfpos_t sp;
    bool has_sp = crid_web_selfpos_get(&sp);
    bool fresh = has_sp && sp.age_ms <= RADAR_STALE_MS;

    int n = crid_rx_get_tracks(s_tracks);
    if (n < 0) n = 0;
    if (n > CRID_RX_MAX_TRACKS) n = CRID_RX_MAX_TRACKS;
    if (n > 0 && s_target >= (unsigned)n) s_target = 0;

    int online = 0, nopos = 0;
    float de[CRID_RX_MAX_TRACKS], dn[CRID_RX_MAX_TRACKS];
    bool  pv[CRID_RX_MAX_TRACKS] = { false };
    float maxd = 0;

    double kx = 0;
    if (fresh) kx = 111320.0 * cos(sp.latitude * 0.0174532925199433);
    for (int i = 0; i < n; i++) {
        const crid_rx_track_t *t = &s_tracks[i];
        if (t->active) online++;
        bool has_ll = !(t->latitude == 0.0 && t->longitude == 0.0);
        if (!has_ll) { nopos++; continue; }
        if (!fresh) continue;
        de[i] = (float)((t->longitude - sp.longitude) * kx);
        dn[i] = (float)((t->latitude - sp.latitude) * 110540.0);
        pv[i] = true;
        float d = sqrtf(de[i] * de[i] + dn[i] * dn[i]);
        if (d > maxd) maxd = d;
    }

    /* 量程：自动取「刚好装下最远目标」的那一档，留 15% 边距；B/M 可手动覆盖。
     * 自动档带滞回：只有装不下、或目标缩到量程 30% 以内才换档，否则目标在
     * 档位边界小幅移动时整个画面会反复缩放。 */
    uint16_t range;
    if (s_radar_rng == 0) {
        float need = maxd * 1.15f;
        range = s_radar_auto;
        bool fit = (need <= 0.1f) || (need <= (float)range && need >= (float)range * 0.3f);
        if (!fit) {
            range = k_radar_ranges[sizeof(k_radar_ranges) / sizeof(k_radar_ranges[0]) - 1];
            for (unsigned k = 0; k < sizeof(k_radar_ranges) / sizeof(k_radar_ranges[0]); k++) {
                if ((float)k_radar_ranges[k] >= need) { range = k_radar_ranges[k]; break; }
            }
            s_radar_auto = range;
        }
    } else {
        range = k_radar_ranges[(s_radar_rng - 1) % 5];
    }

    char line[48];
    if (range >= 1000)
        snprintf(line, sizeof(line), "%s %.1fkm", s_radar_rng ? "RNG" : "AUTO",
                 (double)range / 1000.0);
    else
        snprintf(line, sizeof(line), "%s %um", s_radar_rng ? "RNG" : "AUTO", (unsigned)range);
    radar_info_line(0, 28, COL_ACCENT, line);

    if (!has_sp) {
        radar_info_line(1, 48, COL_RED, "NO SELF POS");
        radar_info_line(2, 62, COL_DIM, "Phone GPS not set");
    } else if (!fresh) {
        snprintf(line, sizeof(line), "GPS STALE %us", (unsigned)(sp.age_ms / 1000));
        radar_info_line(1, 48, COL_AMBER, line);
        radar_info_line(2, 62, COL_DIM, "Phone GPS stale");
    } else {
        snprintf(line, sizeof(line), "GPS OK ~%.0fm", (double)sp.accuracy_m);
        radar_info_line(1, 48, COL_GREEN, line);
        snprintf(line, sizeof(line), "%.5f,%.5f", sp.latitude, sp.longitude);
        radar_info_line(2, 62, COL_DIM, line);
    }

    snprintf(line, sizeof(line), "ON %d/%d  NOPOS %d", online, n, nopos);
    radar_info_line(3, 82, COL_TEXT, line);

    if (n > 0) {
        const crid_rx_track_t *t = &s_tracks[s_target];
        snprintf(line, sizeof(line), "%.18s",
                 t->drone_name[0] ? t->drone_name : (t->uas_id[0] ? t->uas_id : "--"));
        radar_info_line(4, 106, COL_AMBER, line);
        if (pv[s_target]) {
            float d = sqrtf(de[s_target] * de[s_target] + dn[s_target] * dn[s_target]);
            float brg = atan2f(de[s_target], dn[s_target]) * 57.29578f;
            if (brg < 0) brg += 360.0f;
            if (d >= 1000)
                snprintf(line, sizeof(line), "%.1fkm  BRG %.0f", (double)(d / 1000.0f), (double)brg);
            else
                snprintf(line, sizeof(line), "%um  BRG %.0f", (unsigned)d, (double)brg);
            radar_info_line(5, 120, COL_TEXT, line);
            snprintf(line, sizeof(line), "ALT %.0fm", (double)t->altitude_geo);
            radar_info_line(6, 134, COL_DIM, line);
        } else {
            radar_info_line(5, 120, COL_DIM, "no position");
            radar_info_line(6, 134, COL_DIM, t->active ? "online" : "offline");
        }
    } else {
        radar_info_line(4, 106, COL_DIM, "no targets");
        radar_info_line(5, 120, COL_BG, "");
        radar_info_line(6, 134, COL_BG, "");
    }

    /* 两遍处理：先擦掉所有位置/颜色变化的旧光点，再统一画新的。
     * 否则两个目标挨得很近时，后一个的擦除会把前一个刚画好的点抹掉。 */
    struct {
        int x, y, sz;
        uint16_t col;
        bool ok;
    } pend[CRID_RX_MAX_TRACKS];
    memset(pend, 0, sizeof(pend));

    for (int i = 0; i < CRID_RX_MAX_TRACKS; i++) {
        radar_blip_t *b = &s_radar_blips[i];
        bool want = fresh && i < n && pv[i];
        if (!want) {
            if (b->drawn) radar_blip_erase(b);
            continue;
        }
        float d = sqrtf(de[i] * de[i] + dn[i] * dn[i]);
        float sx = de[i] * (float)RADAR_R / (float)range;
        float sy = dn[i] * (float)RADAR_R / (float)range;
        if (d > (float)range && d > 0) {          /* 手动量程装不下时贴到圆环上 */
            sx = de[i] / d * (float)RADAR_R;
            sy = dn[i] / d * (float)RADAR_R;
        }
        int x = RADAR_CX + RADAR_IRND(sx);
        int y = RADAR_CY - RADAR_IRND(sy);
        int sz = (i == (int)s_target) ? 7 : 5;
        uint16_t col = !s_tracks[i].active ? COL_DIM
                       : (i == (int)s_target ? COL_AMBER : COL_GREEN);
        if (x < 4) x = 4;
        if (x > PANEL_W - 12) x = PANEL_W - 12;
        if (y < HDR_H + 3) y = HDR_H + 3;
        if (y > PANEL_H - 12) y = PANEL_H - 12;
        if (b->drawn && (b->x != x || b->y != y || b->sz != sz || b->col != col))
            radar_blip_erase(b);
        pend[i].x = x; pend[i].y = y; pend[i].sz = sz; pend[i].col = col; pend[i].ok = true;
    }
    for (int i = 0; i < CRID_RX_MAX_TRACKS; i++) {
        if (!pend[i].ok) continue;
        radar_blip_t *b = &s_radar_blips[i];
        if (b->drawn && b->x == pend[i].x && b->y == pend[i].y &&
            b->sz == pend[i].sz && b->col == pend[i].col) continue;
        panel_rect((unsigned)pend[i].x, (unsigned)pend[i].y,
                   (unsigned)pend[i].sz, (unsigned)pend[i].sz, pend[i].col);
        b->x = (int16_t)pend[i].x;
        b->y = (int16_t)pend[i].y;
        b->sz = (uint8_t)pend[i].sz;
        b->col = pend[i].col;
        b->drawn = true;
    }
}

static void radar_dial(void) {
    /* 同心圆按 6° 打点：一整圈 60 个 1px 点，比逐行算圆弧省一个量级的 SPI 写 */
    for (unsigned ring = 1; ring <= 4; ring++) {
        int r = RADAR_R * (int)ring / 4;
        for (int a = 0; a < 360; a += 6) {
            float rad = (float)a * 0.0174532925f;
            int x = RADAR_CX + RADAR_IRND(cosf(rad) * (float)r);
            int y = RADAR_CY - RADAR_IRND(sinf(rad) * (float)r);
            panel_rect((unsigned)x, (unsigned)y, 1, 1, COL_DIM);
        }
    }
    panel_hline((unsigned)(RADAR_CX - RADAR_R), RADAR_CY, RADAR_R * 2 + 1, COL_GRID);
    panel_rect(RADAR_CX, RADAR_CY - RADAR_R, 1, RADAR_R * 2 + 1, COL_GRID);
    panel_text(RADAR_CX - 2, 30, "N", COL_DIM, COL_BG, false, 1);
    panel_text(RADAR_CX - 2, 214, "S", COL_DIM, COL_BG, false, 1);
    panel_text(193, RADAR_CY - 3, "E", COL_DIM, COL_BG, false, 1);
    panel_text(12, RADAR_CY - 3, "W", COL_DIM, COL_BG, false, 1);
    /* 右下角图例与固定方位提示（静态，只画一次） */
    panel_text(RADAR_X0, 160, "GREEN online", COL_GREEN, COL_BG, false, 1);
    panel_text(RADAR_X0, 174, "AMBER selected", COL_AMBER, COL_BG, false, 1);
    panel_text(RADAR_X0, 188, "GRAY offline", COL_DIM, COL_BG, false, 1);
    panel_text(RADAR_X0, 204, "N-UP FIXED", COL_DIM, COL_BG, false, 1);
}

static void render_radar(void) {
    render_header("C-RID  RADAR", COL_GREEN);
    panel_rect(0, HDR_H, PANEL_W, PANEL_H - HDR_H, COL_BG);
    radar_dial();
    memset(s_radar_blips, 0, sizeof(s_radar_blips));
    for (unsigned i = 0; i < sizeof(s_radar_info) / sizeof(s_radar_info[0]); i++)
        s_radar_info[i].text[0] = '\0';
    radar_refresh();
    panel_text(8, 230, "M+LR: target   B/M: range   LR: page", COL_DIM, COL_BG, false, 1);
}

static void render(void) {
    panel_fill(COL_BG);   /* 只在切页 / 进入退出编辑时调用 */
    switch (s_page) {
        case 1:
            if (s_detail) {
                render_target_detail();
            } else {
                render_targets_page();
            }
            break;
        case 2: render_radar(); break;
        case 3: render_about(); break;
        case 4: render_config(); break;
        default: render_status(); break;
    }
}

/* ---- 布局签名：切页 / 进出编辑靠它兜底整页重画 ----
 * 页内数值刷新不再用签名，改由每页的 rows_refresh() 逐行比对。 */
static char s_layout_sig[24];

static void layout_sig_sync(void) {
    snprintf(s_layout_sig, sizeof(s_layout_sig), "%u|%d|%d|%d",
             (unsigned)s_page, s_edit_item >= 0, (int)s_editing, (int)s_detail);
}

static void lcd_task(void *arg) {
    (void)arg;
    int64_t last_check = 0, last_paint = 0, menu_chord_us = 0;
    int64_t b_hold_us = 0, b_release_us = 0;
    bool b_long_fired = false;
    for (;;) {
        int64_t now = esp_timer_get_time();
        bool layout_changed = false;   /* 触发整页重画 */
        bool config_local = false;     /* 配置页局部重画 */
        bool radar_local = false;      /* 雷达页局部重画 */

        /* B + M 同时长按 1.5 秒返回开机菜单。已按用户要求取消 R 单独长按回菜单。 */
        bool bm = buttons_down(UI_BTN_BAND) && buttons_down(UI_BTN_MODE);
        if (bm) {
            if (!menu_chord_us) menu_chord_us = now;
            if (now - menu_chord_us >= 1500000) {
                buzzer_play(UI_BUZZ_RESET);
                dualboot_reboot_to_launcher();
            }
        } else {
            menu_chord_us = 0;
        }

        /* EDIT 页长按 B 1 秒：编辑完态 -> 改值态，再长按 -> 退回编辑完态。
         * 进 EDIT 页现在是点按 B（见下方 short_press），列表态长按 B 无额外动作。
         * 光标位置保留，方便"先用 ←/→ 移到目标字符再长按进来改"。
         * B+M 组合正在进行时不触发，避免和回菜单冲突。 */
        bool bdown = buttons_down(UI_BTN_BAND);
        if (bdown && !bm) {
            if (!b_hold_us) b_hold_us = now;
            if (!b_long_fired && now - b_hold_us >= 1000000 && s_page == 4 && s_edit_item >= 0) {
                s_editing = !s_editing;                   /* 编辑完态 <-> 改值态 */
                b_long_fired = true;
                layout_changed = true;
                buzzer_play(UI_BUZZ_CHANNEL);
            }
        } else if (!bdown) {
            if (b_hold_us || b_long_fired) b_release_us = now;
            b_hold_us = 0;
        }
        /* 长按后的释放会产生一次 short_press，由下面吞掉；若没产生（B 被当修饰键），
         * 超时后清掉标志，避免误吞下一次点按。 */
        if (b_release_us && now - b_release_us > 400000) {
            b_release_us = 0;
            b_long_fired = false;
        }

        ui_button_event_t ev;
        buttons_poll(&ev);
        uint16_t press = (uint16_t)(ev.short_press | ev.repeat);
        bool dir_l = !!(press & (1u << UI_BTN_LEFT));
        bool dir_r = !!(press & (1u << UI_BTN_RIGHT));
        bool dir = dir_l || dir_r;
        int step = dir_r ? 1 : -1;

        if (ev.chord_mute) {
            s_muted = !s_muted;
            buzzer_enable(!s_muted);
            lcd_mute_save(s_muted);   /* 记住静音档，下次开机延续 */
            buzzer_play(UI_BUZZ_BAND);
        }

        /* R + 方向键 = 切背光（100% <-> 10%） */
        if (dir && ev.dir_mod == 3 && ev.short_press) {
            /* Toggle between the two presets. Testing the value (not truthiness)
             * matters: 10 is non-zero, so `s_bright ? 10 : 100` can never leave
             * the dim state once entered. */
            s_bright = (s_bright == 100) ? 10 : 100;
            panel_backlight(s_bright);
            buzzer_play(UI_BUZZ_MODE);
            /* 关于页会显示 LIGHT 档位：正在看那页就立刻刷这一行，其它页不用动屏 */
            if (s_page == 3 && s_edit_item < 0) refresh_about_live();
            layout_sig_sync();
        }
        /* 修饰键 + 方向键 = 翻目标（目标页 / 雷达页共用同一个选中项）。dir_mod 1 是
         * BAND 位、2 是 MODE 位，两页上都没有别的用途，所以都接受，避免按键命名
         * 和实物不符时失效。 */
        else if (dir && (ev.dir_mod == 1 || ev.dir_mod == 2) && (s_page == 1 || s_page == 2)) {
            int n = crid_rx_get_tracks(s_tracks);
            if (n > 0) {
                s_target = (unsigned)(((int)s_target + step + n) % n);
                if (s_page == 2)      radar_local = true;   /* 雷达：换高亮目标 */
                else if (s_detail)    layout_changed = true; /* 详情页：整页换一条 */
                else                  render_targets_page(); /* 列表：整块换一批 */
                s_slots_drawn = -1;                          /* 强制下次周期重新核对槽位数 */
            }
            layout_sig_sync();
        }
        /* 普通方向键 */
        else if (dir && ev.dir_mod == 0) {
            if (s_page == 1 && s_detail) {
                /* 详情页：←/→ 直接切上一条/下一条，B 或 M 返回列表 */
                int n = crid_rx_get_tracks(s_tracks);
                if (n > 0) {
                    s_target = (unsigned)(((int)s_target + step + n) % n);
                    layout_changed = true;
                }
            } else if (s_edit_item >= 0 && s_editing) {
                /* 改值态：←/→ 改当前字符/数值 */
                cfg_change((unsigned)s_edit_item, step);
                config_local = true;
            } else if (s_edit_item >= 0 && cfg_is_string((unsigned)s_edit_item)) {
                /* 编辑完态·字符串项：←/→ 只移动光标，不改字符 */
                unsigned cap = (unsigned)cfg_str_cap((unsigned)s_edit_item);
                unsigned len = (unsigned)strlen(cfg_str((unsigned)s_edit_item));
                if (len > cap - 1) len = cap - 1;
                if (len) lcd_edit_pos = (unsigned)((lcd_edit_pos + step + len) % len);
                config_local = true;
            } else if (s_edit_item >= 0) {
                /* 编辑完态·数值项：←/→ 直接改数值 */
                cfg_change((unsigned)s_edit_item, step);
                config_local = true;
            } else if (s_page == 4) {
                s_cfg_cursor = (unsigned)((s_cfg_cursor + (step > 0 ? 1 : CFG_COUNT - 1)) % CFG_COUNT);
                config_local = true;
            } else {
                /* 顺序：状态 → 目标 → 雷达 → 关于 → 配置 */
                s_page = (uint8_t)((s_page + (step > 0 ? 1 : 4)) % 5);
                layout_changed = true;
            }
        }

        if (ev.short_press & (1u << UI_BTN_BAND)) {
            if (b_long_fired) {
                /* 长按进入编辑后松开 B 产生的那一次 short_press，吞掉 */
                b_long_fired = false;
                b_release_us = 0;
            } else if (s_page == 1) {
                /* 目标页：B 和 M 都能进/出详情，避免按键实物命名与代码不符时失效 */
                if (s_detail) {
                    s_detail = false;
                    layout_changed = true;
                } else if (crid_rx_get_tracks(s_tracks) > 0) {
                    s_detail = true;
                    layout_changed = true;
                }
                buzzer_play(UI_BUZZ_CHANNEL);
            } else if (s_page == 2) {
                /* 雷达页：B/M 循环切换量程（自动 → 200m → … → 5km），并记住档位 */
                radar_rng_next();
                radar_local = true;
                buzzer_play(UI_BUZZ_CHANNEL);
            } else if (s_page == 4) {
                if (s_edit_item >= 0 && s_editing) {
                    /* 改值态点按 B：字符串项把光标移到下一字符 */
                    if (cfg_is_string((unsigned)s_edit_item)) {
                        unsigned cap = (unsigned)cfg_str_cap((unsigned)s_edit_item);
                        unsigned len = (unsigned)strlen(cfg_str((unsigned)s_edit_item));
                        if (len > cap - 1) len = cap - 1;
                        if (len) lcd_edit_pos = (lcd_edit_pos + 1) % len;
                    }
                } else if (s_edit_item >= 0) {
                    /* EDIT 编辑完态点按 B = 下一项 */
                    s_edit_item = (s_edit_item + 1) % CFG_COUNT;
                    s_cfg_cursor = (unsigned)s_edit_item;
                    lcd_edit_pos = 0;
                } else {
                    /* 列表态点按 B = 进入 EDIT 页（编辑完态） */
                    s_edit_item = (int)s_cfg_cursor;
                    s_editing = false;
                    lcd_edit_pos = 0;
                    layout_changed = true;   /* 进 EDIT 是布局切换，整页重画 */
                }
                if (!layout_changed) config_local = true;
                buzzer_play(UI_BUZZ_CHANNEL);
            }
        }
        if (ev.short_press & (1u << UI_BTN_MODE)) {
            if (s_page == 1) {
                if (s_detail) {
                    s_detail = false;             /* 详情页按 M 返回列表 */
                    layout_changed = true;
                } else if (crid_rx_get_tracks(s_tracks) > 0) {
                    s_detail = true;              /* 列表页按 M 看详情 */
                    layout_changed = true;
                }
                buzzer_play(UI_BUZZ_CHANNEL);
            } else if (s_page == 2) {
                radar_rng_next();
                radar_local = true;
                buzzer_play(UI_BUZZ_CHANNEL);
            } else if (s_page == 4) {
                if (s_edit_item >= 0 && s_editing) {
                    /* 改值态按 M = 完成本项，退回编辑完态（不保存）。
                     * 编辑完态里又是：点按 B 下一项、长按 B 1s 进改值。 */
                    s_editing = false;
                    layout_changed = true;
                    buzzer_play(UI_BUZZ_CHANNEL);
                } else {
                    /* 列表态 / EDIT 编辑完态按 M = 保存全部并回状态页 */
                    cfg_save();
                    s_edit_item = -1;
                    s_editing = false;
                    s_page = 0;
                    layout_changed = true;
                }
            }
        }
        if (ev.short_press & (1u << UI_BTN_ROT)) {
            buzzer_play(UI_BUZZ_BOOT);
        }

        if (layout_changed) {
            render();                 /* 整页：只有切页 / 进出编辑 */
            layout_sig_sync();
        } else if (config_local) {
            if (s_edit_item >= 0) {
                render_edit_fields();                      /* 只刷标签+值那一块 */
            } else {
                config_list_build_rows();                  /* 光标移动只影响两行 */
                rows_refresh();
            }
            layout_sig_sync();
        } else if (radar_local) {
            radar_refresh();                          /* 量程/高亮变化：只刷雷达动态部分 */
        }

        /* 周期检查：只做局部刷新。
         * 每页都是"标签固定 + 逐行比对数值"，rows_refresh() 只重画真正变了的行，
         * 所以这里可以放心按固定节奏跑。目标页/详情页 1 秒一次，状态页 500ms。 */
        if (now - last_check >= 200000) {
            char layout[sizeof(s_layout_sig)];
            snprintf(layout, sizeof(layout), "%u|%d|%d|%d",
                     (unsigned)s_page, s_edit_item >= 0, (int)s_editing, (int)s_detail);
            if (strcmp(layout, s_layout_sig) != 0) {
                render();                 /* 布局真的变了（兜底） */
                layout_sig_sync();
            } else if (s_page == 1 && now - last_paint >= 1000000) {
                last_paint = now;
                if (s_detail) refresh_detail_live();
                else          refresh_targets_live();
            } else if (s_page == 2 && now - last_paint >= 1000000) {
                last_paint = now;
                radar_refresh();          /* 雷达：每秒只动变化的光点与信息行 */
            } else if (s_page == 0 && now - last_paint >= 500000) {
                last_paint = now;
                refresh_status_live();
            } else if (s_page == 3 && now - last_paint >= 1000000) {
                last_paint = now;
                refresh_about_live();
            }
            last_check = now;
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

void crid_lcd_start(cn_crid_config_t *config) {
    s_cfg = config;
    s_editing = false;
    if (!panel_init()) {
        ESP_LOGE(TAG, "panel init failed - check the ST7789 wiring");
        return;
    }
    /* lcd_init() leaves the PWM backlight at 0 %. Restore the saved/default
     * level now that the timer is really running (XTAL-clocked), otherwise the
     * panel would stay dark. */
    panel_backlight(s_bright);

    /* 恢复上次的蜂鸣器静音状态，让开机延续关机前的选择。 */
    s_muted = lcd_mute_load();
    buzzer_enable(!s_muted);

    /* 恢复上次的雷达量程档位（0=自动）；从未设置过就是自动。 */
    s_radar_rng = radar_rng_load();

    memcpy(&s_edit, s_cfg, sizeof(s_edit));
    render();
    layout_sig_sync();
    buzzer_play(UI_BUZZ_BOOT);
    BaseType_t ok = xTaskCreate(lcd_task, "crid_lcd", 6144, NULL, 4, NULL);
    if (ok != pdPASS) ESP_LOGE(TAG, "failed to create LCD task");
}
