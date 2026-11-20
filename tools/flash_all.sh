#!/usr/bin/env bash
# 整片烧录：bootloader + 分区表 + 三个 app（RID 同时写进两个槽）。
# 用法：bash tools/flash_all.sh [-p COMx] [--erase]
# 默认端口 COM6。整片烧录会清掉 NVS，两个固件的设置都会恢复默认。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="COM6"
DO_ERASE=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        -p) PORT="${2:?missing port}"; shift 2 ;;
        --erase) DO_ERASE=1; shift ;;
        *) PORT="$1"; shift ;;
    esac
done

PY="${IDF_PYTHON_ENV_PATH:-}/Scripts/python.exe"
if [[ ! -x "$PY" ]]; then PY="$(command -v python || command -v python3)"; fi

ESPTOOL=("$PY" -m esptool --chip esp32c5 -p "$PORT" -b 460800 \
         --before default-reset --after hard-reset)

if [[ $DO_ERASE -eq 1 ]]; then
    "${ESPTOOL[@]}" erase-flash
fi

"${ESPTOOL[@]}" write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \
    0x2000  "$ROOT/out/bootloader.bin" \
    0x8000  "$ROOT/out/partition-table.bin" \
    0x10000 "$ROOT/out/ota_data_initial.bin" \
    0x20000 "$ROOT/out/launcher.bin" \
    0x120000 "$ROOT/out/sdr.bin" \
    0x320000 "$ROOT/out/rid_a.bin" \
    0x620000 "$ROOT/out/rid_b.bin"

echo "Flashed. USB-Serial-JTAG console at 115200 baud shows the boot menu log."
