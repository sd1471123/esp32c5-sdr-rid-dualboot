#!/usr/bin/env python3
"""Package the four built images into the two files a release ships.

    python tools/package_release.py --version v0.01

Reads out/*.bin (produced by tools/build_all.sh), then writes:

    out/esp32c5-sdr-rid-dualboot-<ver>-full-16mb.bin   one-shot 16 MB image
    out/esp32c5-sdr-rid-dualboot-<ver>-parts.zip       individual images + FLASH.txt
    out/SHA256SUMS.txt

The full image is a straight 16 MB buffer filled with 0xFF, so burning it wipes
NVS as a side effect - which is exactly what a clean install wants. The parts
zip keeps the NVS-looking gap untouched and is for people who only want to
update one slot.
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "out"
FLASH_TEMPLATE = ROOT / "tools" / "release" / "FLASH.txt"

# (address, file) in burn order. C5 boots from 0x2000, not 0x0.
LAYOUT = [
    (0x2000, "bootloader.bin"),
    (0x8000, "partition-table.bin"),
    (0x10000, "ota_data_initial.bin"),
    (0x20000, "launcher.bin"),
    (0x120000, "sdr.bin"),
    (0x320000, "rid_a.bin"),
    (0x620000, "rid_b.bin"),
]
FLASH_SIZE = 16 * 1024 * 1024


def sha256(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True, help="release tag, e.g. v0.01")
    ap.add_argument("--size-mb", type=int, default=16)
    args = ap.parse_args()

    size = args.size_mb * 1024 * 1024
    if size != FLASH_SIZE:
        print(f"note: building a {args.size_mb} MB image, partition table assumes 16 MB")

    missing = [n for _, n in LAYOUT if not (OUT / n).exists()]
    if missing:
        raise SystemExit("missing build output: " + ", ".join(missing) + " - run tools/build_all.sh first")

    img = bytearray(b"\xff" * size)
    for addr, name in LAYOUT:
        data = (OUT / name).read_bytes()
        end = addr + len(data)
        if end > size:
            raise SystemExit(f"{name} does not fit: 0x{addr:x}+{len(data):#x} > 0x{size:x}")
        img[addr:end] = data
        print(f"  0x{addr:06x}  {name:24s} {len(data):>9,d} B")

    full = OUT / f"esp32c5-sdr-rid-dualboot-{args.version}-full-{args.size_mb}mb.bin"
    full.write_bytes(img)
    print(f"wrote {full.relative_to(ROOT)}  ({full.stat().st_size:,} B)")

    flash_txt = FLASH_TEMPLATE.read_text(encoding="utf-8").replace("{version}", args.version)
    zip_path = OUT / f"esp32c5-sdr-rid-dualboot-{args.version}-parts.zip"
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for _, name in LAYOUT:
            z.write(OUT / name, name)
        z.writestr("FLASH.txt", flash_txt)
    print(f"wrote {zip_path.relative_to(ROOT)}  ({zip_path.stat().st_size:,} B)")

    sums = OUT / "SHA256SUMS.txt"
    lines = [f"{sha256(p)} *{p.name}" for p in (full, zip_path)]
    sums.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {sums.relative_to(ROOT)}")
    for line in lines:
        print("  " + line)

    print("\nupload with:")
    print(f"  gh release create {args.version} {full} {zip_path} {sums} --title {args.version} --notes-file NOTES.md")


if __name__ == "__main__":
    main()
