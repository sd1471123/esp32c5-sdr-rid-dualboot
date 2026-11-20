#!/usr/bin/env python3
"""Render mock screenshots of the three firmwares for the README.

These are *simulated*: the pixels are produced from the same font tables and
the same layout constants the firmware uses (5x7 / 7x12 glyphs scaled exactly
like panel_text() does), not from a camera pointed at the panel. That keeps the
picture honest about spacing, wrapping and the amount of text that actually
fits on 320x240, while still being reproducible in CI.

    python tools/make_screenshots.py            # docs/screens/*.png at 1x
    python tools/make_screenshots.py --scale 2  # ... plus 2x for slide decks

Only "dark" is rendered on purpose: the UI is a fixed dark theme, so there is
no light variant to be inconsistent with.
"""

from __future__ import annotations

import argparse
import math
import pathlib
import random
import re

from PIL import Image, ImageDraw

ROOT = pathlib.Path(__file__).resolve().parent.parent
FONT_DIR = ROOT / "common" / "panel"
OUT_DIR = ROOT / "docs" / "screens"

W, H = 320, 240


# --------------------------------------------------------------------------- #
# Fonts: parse the generated C tables so the previews use the real glyphs.
# --------------------------------------------------------------------------- #
def load_font(path: pathlib.Path, height: int) -> list[list[int]]:
    text = path.read_text(encoding="utf-8")
    body = text[text.index("="):]
    glyphs = []
    for block in re.findall(r"\{([^{}]*)\}", body):
        vals = [int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]{2})", block)]
        if not vals:
            continue
        glyphs.append(vals[:height] + [0] * (height - len(vals)))
    if len(glyphs) < 95:
        raise SystemExit(f"{path}: parsed only {len(glyphs)} glyphs")
    return glyphs[:95]


FONT5X7 = load_font(FONT_DIR / "font5x7.c", 7)
FONT7X12 = load_font(FONT_DIR / "font7x12.c", 12)


# --------------------------------------------------------------------------- #
# Colours: the firmware stores RGB565; expand it the way the panel does.
# --------------------------------------------------------------------------- #
def c565(v: int) -> tuple[int, int, int]:
    r = (v >> 11) & 0x1F
    g = (v >> 5) & 0x3F
    b = v & 0x1F
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


COL_BG = c565(0x0000)
COL_PANEL = c565(0x0021)
COL_GRID = c565(0x0841)
COL_DIM = c565(0x5B2E)
COL_TEXT = c565(0xE75D)
COL_ACCENT = c565(0x063F)
COL_GREEN = c565(0x6F76)
COL_AMBER = c565(0xFD89)
COL_RED = c565(0xFB4D)
COL_TRACE = c565(0x069F)
COL_USB = c565(0x07FF)
COL_TINT_C = c565(0x0967)
COL_STRIP = c565(0x0021)


class Screen:
    """Just enough of panel_draw.c to reproduce a frame."""

    def __init__(self) -> None:
        self.img = Image.new("RGB", (W, H), COL_BG)
        self.draw = ImageDraw.Draw(self.img)

    # -- primitives ---------------------------------------------------------
    def fill(self, color=COL_BG) -> None:
        self.draw.rectangle([0, 0, W - 1, H - 1], fill=color)

    def rect(self, x, y, w, h, color) -> None:
        if w <= 0 or h <= 0:
            return
        self.draw.rectangle([x, y, x + w - 1, y + h - 1], fill=color)

    def hline(self, x, y, w, color) -> None:
        self.rect(x, y, w, 1, color)

    def text(self, x, y, s, fg, bg, big=False, scale=1) -> int:
        """Mirror of panel_text(): (glyph width + 1) * scale per character."""
        gw, gh = (7, 12) if big else (5, 7)
        font = FONT7X12 if big else FONT5X7
        cw = gw + 1
        n = len(s)
        while n and x + n * cw * scale > W:
            n -= 1
        s = s[:n]
        if not s:
            return 0
        width = min(n * cw * scale - scale, W - x)
        self.rect(x, y, width, gh * scale, bg)
        for row in range(gh):
            for i, ch in enumerate(s):
                o = ord(ch)
                bits = font[o - 32][row] if 32 <= o < 127 else 0
                for col in range(gw):
                    if not (bits >> col) & 1:
                        continue
                    px = x + (i * cw + col) * scale
                    for kx in range(scale):
                        xx = px + kx
                        if xx >= W:
                            break
                        for ky in range(scale):
                            yy = y + row * scale + ky
                            if yy < H:
                                self.img.putpixel((xx, yy), fg)
        return width

    def ptext(self, x, y, s, fg, bg, big=False, scale=1) -> int:
        return self.text(x, y, s, fg, bg, big, scale)

    def save(self, name: str, scale: int) -> pathlib.Path:
        suffix = "" if scale == 1 else f"@{scale}x"
        path = OUT_DIR / f"{name}{suffix}.png"
        img = self.img
        if scale != 1:
            img = img.resize((W * scale, H * scale), Image.NEAREST)
        img.save(path)
        return path


# --------------------------------------------------------------------------- #
# launcher/main/main.c -> draw_menu()
# --------------------------------------------------------------------------- #
def boot_menu() -> Screen:
    s = Screen()
    s.fill(COL_BG)

    s.rect(0, 0, W, 34, COL_PANEL)
    s.hline(0, 33, W, COL_ACCENT)
    s.text(10, 8, "ESP32-C5  DUAL BOOT", COL_TEXT, COL_PANEL, False, 2)
    s.text(264, 12, "16MB", COL_ACCENT, COL_PANEL, False, 2)

    s.rect(10, 42, 300, 78, COL_PANEL)
    s.rect(10, 42, 4, 78, COL_ACCENT)
    s.text(24, 52, "< SDR", COL_ACCENT, COL_PANEL, True, 2)
    s.text(24, 80, "SPECTRUM ANALYZER", COL_DIM, COL_PANEL, False, 2)
    s.text(24, 100, "2.4G / 5.8G   5 keys + ST7789", COL_DIM, COL_PANEL, False, 1)

    s.rect(10, 126, 300, 78, COL_PANEL)
    s.rect(10, 126, 4, 78, COL_AMBER)
    s.text(24, 136, "> C-RID", COL_AMBER, COL_PANEL, True, 2)
    s.text(24, 164, "REMOTE ID  TX/RX", COL_DIM, COL_PANEL, False, 2)
    s.text(24, 184, "GB42590 / GB46750", COL_DIM, COL_PANEL, False, 1)

    s.hline(0, 210, W, COL_GRID)
    s.text(10, 216, "LEFT/B: SDR   RIGHT/M: RID   ROT: light", COL_DIM, COL_BG, False, 1)
    return s


# --------------------------------------------------------------------------- #
# SDR: status bar + spectrum + waterfall + channel strip
# --------------------------------------------------------------------------- #
def sdr_spectrum() -> Screen:
    s = Screen()
    s.fill(COL_BG)

    # --- status bar (30 px), laid out like render_topbar() ---------------
    s.rect(0, 0, W, 30, COL_PANEL)
    band = "2.4G"
    band_w = s.text(4, 9, band, COL_TEXT, COL_PANEL, True)
    bx, bw, bh, by = 4 + band_w + 5, 46, 20, 5
    s.rect(bx, by, bw, bh, COL_TINT_C if False else COL_PANEL)
    for x0, y0, x1, y1 in (
        (bx, by, bx + bw - 1, by),
        (bx, by + bh - 1, bx + bw - 1, by + bh - 1),
        (bx, by, bx, by + bh - 1),
        (bx + bw - 1, by, bx + bw - 1, by + bh - 1),
    ):
        s.draw.line([x0, y0, x1, y1], fill=COL_GREEN)
    live = "LIVE"
    lw = len(live) * 8 - 1
    s.text(bx + (bw - lw) // 2, by + 4, live, COL_GREEN, COL_PANEL, True)
    s.text(bx + bw + 6, 9, "PK12 A5 D62%", COL_TEXT, COL_PANEL, True)
    ch = "ch6 2437M"
    cw_ = len(ch) * 8 - 1
    s.text(W - 4 - cw_, 9, ch, COL_TEXT, COL_PANEL, True)

    # --- spectrum (y 30..124) --------------------------------------------
    spec_y, spec_h = 30, 94
    base = spec_y + spec_h - 1
    for gx in range(0, W, 32):
        s.draw.line([gx, spec_y, gx, base], fill=COL_GRID)
    for gy in range(spec_y + 12, base, 24):
        s.draw.line([0, gy, W - 1, gy], fill=COL_GRID)

    # Three fat 20 MHz Wi-Fi humps on a 2.4 GHz sweep, normalised to 0..1 so the
    # trace fills roughly 80 % of the plot instead of pinning to the top edge.
    power = lambda x, cx, amp, sig, fall: amp / (1.0 + ((x - cx) / sig) ** 2) ** fall
    peaks = [(58, 0.86, 26, 1.6), (152, 0.94, 30, 1.6), (246, 0.72, 24, 1.6)]

    def level_at(x: float, jitter: float = 0.0) -> float:
        v = 0.05 + jitter
        for cx, amp, sig, fall in peaks:
            v += power(x, cx, amp, sig, fall)
        return max(0.0, min(1.0, v))

    rnd = random.Random(20261120)
    points = []
    prev = 0.10
    for x in range(W):
        env = level_at(x, rnd.uniform(-0.02, 0.02))
        prev = prev * 0.35 + env * 0.65
        points.append((x, base - int(prev * (spec_h - 12))))
    s.draw.line(points, fill=COL_TRACE, width=1)
    # peak-hold: a dimmer line riding a few pixels above the live trace
    s.draw.line([(x, y - 4) for x, y in points], fill=c565(0x030F), width=1)
    # callout chip, as render_spectrum() draws for the loudest peak
    s.rect(6, spec_y + 4, 88, 15, COL_PANEL)
    s.draw.rectangle([6, spec_y + 4, 6 + 87, spec_y + 4 + 14], outline=COL_AMBER)
    s.text(9, spec_y + 8, "2442 -41dBm", COL_AMBER, COL_PANEL, False, 1)

    # --- waterfall (y 124..208) ------------------------------------------
    # Each row is one capture: same channel humps, drifting a pixel or two, with
    # per-pixel noise, then faded into black with age like the real waterfall.
    wf_y, wf_h = 124, 84
    px = Image.new("RGB", (W, wf_h))
    buf = px.load()
    ramp = [
        (0.00, (0, 0, 0)),
        (0.12, c565(0x0208)),
        (0.32, c565(0x0313)),
        (0.52, c565(0x04B7)),
        (0.70, c565(0x06DF)),
        (0.84, c565(0x4F93)),
        (0.95, COL_AMBER),
        (1.00, (255, 240, 190)),
    ]

    def ramp_color(v: float):
        for i in range(1, len(ramp)):
            t0, c0 = ramp[i - 1]
            t1, c1 = ramp[i]
            if v <= t1:
                k = 0.0 if t1 == t0 else (v - t0) / (t1 - t0)
                return tuple(int(c0[j] + (c1[j] - c0[j]) * k) for j in range(3))
        return ramp[-1][1]

    rnd = random.Random(4242)
    drift = 0.0
    history: list[list[float]] = []
    for y in range(wf_h):
        drift += rnd.uniform(-0.5, 0.5)
        drift = max(-3.0, min(3.0, drift))
        row = []
        for x in range(W):
            v = level_at(x + drift, rnd.uniform(-0.03, 0.03))
            if history:  # a little persistence, like the panel's real average
                v = v * 0.82 + history[-1][x] * 0.18
            row.append(v)
        history.append(row)
        fade = 1.0 - (wf_h - 1 - y) / wf_h * 0.55  # top = newest = brightest
        for x, v in enumerate(row):
            buf[x, y] = ramp_color(v * fade)
    s.img.paste(px, (0, wf_y))

    # --- channel strip (y 208..240) ---------------------------------------
    # 14 channel slots, bare channel numbers as labels (with the firmware's
    # "skip when the next label would touch" rule), an ACT and a DET bar per
    # slot and an accent underline under the tuned one.
    strip_y, strip_h = 208, 32
    s.rect(0, strip_y, W, strip_h, COL_STRIP)
    s.hline(0, strip_y, W, COL_BG)
    slots, active = 14, 5
    slot_w = W / slots
    act_y, det_y, bar_h = strip_y + 11, strip_y + 20, 7
    last_right, first = 0, True
    for i in range(slots):
        x0 = int(i * slot_w)
        x1 = int((i + 1) * slot_w) - 1
        if i == active:
            s.rect(x0, strip_y + 1, x1 - x0 + 1, strip_h - 1, COL_TINT_C)
        # ACT bar: green below 12 dB over noise, amber above, red at the top
        act = (34, 8, 61, 12, 5, 92, 44, 17, 9, 26, 7, 3, 12, 5)[i]
        s.rect(x0, act_y, x1 - x0 + 1, bar_h, COL_GRID)
        if act > 2:
            s.rect(x0, act_y, max(1, (x1 - x0 + 1) * act // 100), bar_h,
                   COL_GREEN if act < 50 else (COL_AMBER if act < 80 else COL_RED))
        det = (12, 4, 20, 6, 3, 38, 26, 9, 5, 14, 4, 2, 8, 3)[i]
        s.rect(x0, det_y, x1 - x0 + 1, bar_h, COL_GRID)
        if det > 2:
            s.rect(x0, det_y, max(1, (x1 - x0 + 1) * det // 100), bar_h, COL_USB)
        tag = str(i + 1)
        tw = len(tag) * 6 - 1
        lx = max(1, x0 + (x1 - x0 + 1 - tw) // 2)
        if not first and i + 1 != slots and lx < last_right + 4:
            continue
        s.text(lx, strip_y + 2, tag, COL_DIM, COL_TINT_C if i == active else COL_STRIP)
        last_right, first = lx + tw, False
    s.rect(int(active * slot_w), strip_y + strip_h - 3, int(slot_w) - 1, 2, COL_ACCENT)
    return s


# --------------------------------------------------------------------------- #
# C-RID pages (rid/main/crid_lcd.c)
# --------------------------------------------------------------------------- #
HDR_H = 22


def header(s: Screen, title: str, accent) -> None:
    s.rect(0, 0, W, HDR_H, COL_PANEL)
    s.hline(0, HDR_H - 1, W, accent)
    s.text(8, 4, title, accent, COL_PANEL, False, 2)


def rid_status() -> Screen:
    s = Screen()
    s.fill(COL_BG)
    header(s, "C-RID  TX", COL_GREEN)

    rows = [
        ("BAND ", "2.4G  CH 6", COL_ACCENT),
        ("STD  ", "GB42590-2023", COL_AMBER),
        ("UAS  ", "1595F39A2C4E81D0", COL_TEXT),
        ("NAME ", "DJI Mavic 3 Pro", COL_TEXT),
        ("OP   ", "OP-1101-8899", COL_DIM),
        ("POS  ", "30.27415,120.15507", COL_DIM),
        ("ALT  ", "126.4m  SPD 12.0", COL_DIM),
        ("HDG  ", "87   ST 2", COL_DIM),
        ("AP   ", "C-RID-2A4F", COL_ACCENT),
    ]
    y = 26
    for label, value, vcolor in rows:
        s.text(6, y, label, COL_DIM, COL_BG, False, 2)
        s.text(70, y, value, vcolor, COL_BG, False, 2)
        y += 20

    s.hline(0, 208, W, COL_GRID)
    s.text(8, 214, "LR: page   R+LR: light   L+R: mute", COL_DIM, COL_BG, False, 1)
    return s


def rid_targets() -> Screen:
    s = Screen()
    s.fill(COL_BG)
    header(s, "C-RID  RX TARGETS", COL_ACCENT)

    s.text(6, 26, "TRACK", COL_DIM, COL_BG, False, 2)
    s.text(78, 26, "3 ON / 5 ALL", COL_TEXT, COL_BG, False, 2)
    s.text(6, 48, "PKT  ", COL_DIM, COL_BG, False, 1)
    s.text(48, 48, "1842  HIT 37  OVF 0", COL_DIM, COL_BG, False, 1)
    s.hline(0, 62, W, COL_GRID)

    tracks = [
        (">3A4F", "-58 dBm", "1595F39A2C4E", "GB42590", "126m 12m/s"),
        (" 7C21", "-71 dBm", "1588C41D0092", "ASTM", "88m 6m/s"),
        (" B9E0", "-83 dBm", "1583AA7710F4", "GB46750", "42m 0m/s"),
    ]
    for slot, (mac, rssi, uid, proto, tail) in enumerate(tracks):
        y = 66 + slot * 52
        color = COL_GREEN if slot == 0 else COL_DIM
        s.text(6, y, mac, color, COL_BG, False, 2)
        s.text(126, y, rssi, color, COL_BG, False, 2)
        s.text(6, y + 17, "ID", COL_DIM, COL_BG, False, 2)
        s.text(42, y + 17, uid, COL_TEXT if slot == 0 else COL_DIM, COL_BG, False, 2)
        s.text(6, y + 34, proto, COL_DIM, COL_BG, False, 2)
        s.text(96, y + 34, tail, COL_DIM, COL_BG, False, 2)
        if slot:
            s.hline(0, y - 2, W, COL_GRID)

    s.text(8, 228, "M+LR: target   B/M tap: detail   LR: page", COL_DIM, COL_BG, False, 1)
    return s


def rid_detail() -> Screen:
    s = Screen()
    s.fill(COL_BG)
    header(s, "TARGET 1/5 ON", COL_GREEN)

    rows = [
        ("MAC  ", "3A4F91C7D2E8", COL_ACCENT),
        ("RSSI ", "-58 dBm  CH6", COL_GREEN),
        ("PROTO", "GB42590", COL_AMBER),
        ("MSG  ", "184  2s", COL_TEXT),
        ("UAS  ", "1595F39A2C4E81D0", COL_TEXT),
        ("NAME ", "DJI Mavic 3 Pro", COL_TEXT),
        ("OP   ", "OP-1101-8899", COL_DIM),
        ("POS  ", "30.27415,120.15507", COL_TEXT),
        ("ALT  ", "126.4m  AGL 120.00m", COL_TEXT),
        ("SPD  ", "H12.0 V0.4 HDG87", COL_TEXT),
        ("ST   ", "ST2 UA2 ACC", COL_DIM),
        ("OPPOS", "30.27400,120.15520", COL_DIM),
    ]
    y = 26
    for label, value, vcolor in rows:
        s.text(6, y, label, COL_DIM, COL_BG, False, 2)
        s.text(78, y, value, vcolor, COL_BG, False, 2)
        y += 16

    s.hline(0, 222, W, COL_GRID)
    s.text(8, 226, "LR: target   B/M: back", COL_DIM, COL_BG, False, 1)
    return s


def rid_about() -> Screen:
    s = Screen()
    s.fill(COL_BG)
    header(s, "C-RID  ABOUT", COL_AMBER)

    rows = [
        ("VER  ", "1.4.0-rid", COL_TEXT),
        ("PART ", "rid_a", COL_ACCENT),
        ("WEB  ", "192.168.4.1", COL_TEXT),
        ("BT   ", "Web Bluetooth", COL_DIM),
        ("OTA  ", "rid_a <-> rid_b", COL_DIM),
        ("LIGHT", "100%", COL_ACCENT),
    ]
    y = 26
    for label, value, vcolor in rows:
        s.text(6, y, label, COL_DIM, COL_BG, False, 2)
        s.text(66, y, value, vcolor, COL_BG, False, 2)
        y += 18

    s.hline(0, 146, W, COL_GRID)
    s.text(8, 154, "HOLD B+M -> menu", COL_AMBER, COL_BG, False, 2)
    s.text(8, 178, "R+LR:light  L+R:mute", COL_DIM, COL_BG, False, 1)
    s.text(8, 200, "CFG: Btap enters EDIT", COL_DIM, COL_BG, False, 1)
    return s


CFG_ROWS = [
    ("MODE", "TX  transmit"),
    ("STANDARD", "GB42590-2023"),
    ("BAND", "2.4G"),
    ("CHANNEL", "6"),
    ("UAS ID", "1595F39A2C4E81D0"),
    ("DRONE NAME", "DJI Mavic 3 Pro"),
    ("OPERATOR ID", "OP-1101-8899"),
    ("BASE LAT", "30.27415"),
    ("BASE LON", "120.15507"),
    ("BASE ALT", "126.0"),
    ("SPEED", "12.0"),
    ("BEACON MS", "200"),
]


def rid_config(cursor: int = 4) -> Screen:
    s = Screen()
    s.fill(COL_BG)
    header(s, "C-RID  CONFIG", COL_GREEN)
    for i, (label, value) in enumerate(CFG_ROWS):
        y = 26 + i * 16
        sel = i == cursor
        bg = COL_PANEL if sel else COL_BG
        fg = COL_AMBER if sel else COL_DIM
        if sel:
            s.rect(0, y, W, 15, bg)
        s.text(6, y, (" >" if sel else "  ") + f"{label:<12}", fg, bg, False, 1)
        s.text(96, y, value, fg, bg, False, 1)
    s.text(8, 228, "Btap:enter EDIT  LR:next  M:save+exit", COL_DIM, COL_BG, False, 1)
    return s


def rid_edit() -> Screen:
    s = Screen()
    s.fill(COL_BG)
    header(s, "EDIT  CONFIG", COL_AMBER)

    s.text(8, 40, "UAS ID", COL_ACCENT, COL_BG, False, 2)
    s.hline(0, 66, W, COL_GRID)
    s.text(8, 82, "1595F39A2C4E81D0", COL_TEXT, COL_BG, False, 2)
    s.rect(8 + 4 * 12, 102, 10, 2, COL_AMBER)  # caret under character 5

    s.text(8, 136, "LR:move char  B:next item", COL_DIM, COL_BG, False, 2)
    s.text(8, 168, "Bhold1s:edit  M:save+exit", COL_DIM, COL_BG, False, 2)
    s.text(8, 208, "HOLD B+M: abort + menu", COL_DIM, COL_BG, False, 1)
    return s


# --------------------------------------------------------------------------- #
# Overview sheet: 3 x 3 grid with captions, so the README shows one image.
# --------------------------------------------------------------------------- #
PAGES = [
    ("01-boot-menu", "BOOT MENU", boot_menu),
    ("02-sdr-spectrum", "SDR SPECTRUM", sdr_spectrum),
    ("03-rid-status", "C-RID STATUS", rid_status),
    ("04-rid-targets", "RX TARGETS", rid_targets),
    ("05-rid-detail", "TARGET DETAIL", rid_detail),
    ("06-rid-config", "CONFIG LIST", rid_config),
    ("07-rid-edit", "EDIT CONFIG", rid_edit),
    ("08-rid-about", "ABOUT", rid_about),
]


def overview(images: list[Image.Image]) -> Image.Image:
    gap, cap_h = 12, 20
    cols = 3
    rows = math.ceil(len(images) / cols)
    tw = cols * W + (cols + 1) * gap
    th = rows * (H + cap_h) + (rows + 1) * gap
    sheet = Image.new("RGB", (tw, th), c565(0x10A2))
    draw = ImageDraw.Draw(sheet)
    for idx, (name, cap, _) in enumerate(PAGES):
        if idx >= len(images):
            break
        r, c = divmod(idx, cols)
        x = gap + c * (W + gap)
        y = gap + r * (H + cap_h + gap)
        sheet.paste(images[idx], (x, y + cap_h))
        draw_caption(sheet, x + 2, y + 5, cap)
        draw.rectangle([x - 1, y + cap_h - 1, x + W, y + cap_h + H], outline=c565(0x2124))
    return sheet


def load_caption_font(size: int = 13):
    """A readable UI font for the sheet captions; these are documentation
    labels, not on-screen pixels, so the 5x7 panel font would only hurt."""
    from PIL import ImageFont

    for name in ("DejaVuSans-Bold.ttf", "arialbd.ttf", "seguisb.ttf", "arial.ttf"):
        for base in (None, "C:/Windows/Fonts/", "/usr/share/fonts/truetype/dejavu/"):
            try:
                return ImageFont.truetype(name if base is None else base + name, size)
            except OSError:
                continue
    return None


CAPTION_FONT = load_caption_font()


def draw_caption(img: Image.Image, x: int, y: int, s: str) -> None:
    if CAPTION_FONT is not None:
        ImageDraw.Draw(img).text((x, y), s, font=CAPTION_FONT, fill=c565(0x8C51))
        return
    for i, ch in enumerate(s):
        o = ord(ch)
        bits = FONT5X7[o - 32] if 32 <= o < 127 else [0] * 7
        for row in range(7):
            for col in range(5):
                if (bits[row] >> col) & 1:
                    img.putpixel((x + i * 6 + col, y + row), COL_DIM)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--scale", type=int, default=1, help="extra nearest-neighbour upscale")
    args = ap.parse_args()

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    shots = []
    for name, _, fn in PAGES:
        scr = fn()
        shots.append(scr.img)
        for sc in {1, args.scale}:
            p = scr.save(name, sc)
            print(f"wrote {p.relative_to(ROOT)} ({sc}x)")

    sheet = overview(shots)
    sheet.save(OUT_DIR / "overview.png")
    print(f"wrote {(OUT_DIR / 'overview.png').relative_to(ROOT)}")
    if args.scale != 1:
        big = sheet.resize((sheet.width * args.scale, sheet.height * args.scale), Image.NEAREST)
        big.save(OUT_DIR / "overview@2x.png")
        print(f"wrote {(OUT_DIR / 'overview@2x.png').relative_to(ROOT)}")


if __name__ == "__main__":
    main()
