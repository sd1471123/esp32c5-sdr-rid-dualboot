#!/usr/bin/env bash
# 一键构建三个 app：launcher(factory) + sdr(ota_0) + rid(ota_1/ota_2)
# 使用前先在 ESP-IDF 环境里（或设置好 IDF_PATH / IDF_PYTHON_ENV_PATH）。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY="${IDF_PYTHON_ENV_PATH:-}/Scripts/python.exe"
if [[ ! -x "$PY" ]]; then PY="$(command -v python || command -v python3)"; fi

idf() {
    "$PY" "$ROOT/tools/idfwrap.py" "$@"
}

echo "==> building launcher (factory)"
rm -f "$ROOT/launcher/sdkconfig"
idf -C "$ROOT/launcher" set-target esp32c5
idf -C "$ROOT/launcher" build

echo "==> building SDR (ota_0 / sdr)"
rm -f "$ROOT/sdr/sdkconfig"
SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c5;sdkconfig.defaults.esp32c5-lcd" \
    idf -C "$ROOT/sdr" set-target esp32c5
SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c5;sdkconfig.defaults.esp32c5-lcd" \
    idf -C "$ROOT/sdr" build

echo "==> building C-RID (ota_1 / rid_a)"
rm -f "$ROOT/rid/sdkconfig"
idf -C "$ROOT/rid" set-target esp32c5
idf -C "$ROOT/rid" build

mkdir -p "$ROOT/out"
cp "$ROOT/launcher/build/bootloader/bootloader.bin"                 "$ROOT/out/bootloader.bin"
cp "$ROOT/launcher/build/partition_table/partition-table.bin"       "$ROOT/out/partition-table.bin"
cp "$ROOT/launcher/build/ota_data_initial.bin"                      "$ROOT/out/ota_data_initial.bin"
cp "$ROOT/launcher/build/dual_boot_launcher.bin"                    "$ROOT/out/launcher.bin"
cp "$ROOT/sdr/build/esp_sdr.bin"                                    "$ROOT/out/sdr.bin"
cp "$ROOT/rid/build/remoteid_scanner.bin"                           "$ROOT/out/rid.bin"
cp "$ROOT/out/rid.bin"                                              "$ROOT/out/rid_a.bin"
cp "$ROOT/out/rid.bin"                                              "$ROOT/out/rid_b.bin"
cp "$ROOT/partitions/merged_16mb.csv"                               "$ROOT/out/partition-table.csv"

echo
echo "Build done. Flash with:"
echo "  bash tools/flash_all.sh -p COMx"
