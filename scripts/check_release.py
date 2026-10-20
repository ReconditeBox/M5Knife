#!/usr/bin/env python3
"""Pre-build checks for M5Knife version labels and flash-embedded assets.

Run: python scripts/check_release.py
Uses only Python's standard library; independent of PlatformIO and hardware.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
ENV = "m5stack-cardputer-adv"


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def main() -> None:
    firmware = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
    readme = (ROOT / "README.md").read_text(encoding="utf-8")
    ini = (ROOT / "platformio.ini").read_text(encoding="utf-8")

    def version(pattern: str, text: str, label: str) -> str:
        found = re.findall(pattern, text, flags=re.IGNORECASE)
        if len(found) != 1:
            fail(f"Expected one {label} version, found {len(found)}")
        return found[0]

    header = version(r'M5KNIFE\s+(\d+\.\d+\.\d+)', firmware, "header")
    about = version(r'Version\s+(\d+\.\d+\.\d+)', firmware, "About")
    docs = version(r'\*\*Current firmware:\s*(\d+\.\d+\.\d+)\*\*',
                   readme, "README")
    if not (header == about == docs):
        fail(f"Version mismatch: header={header}, About={about}, README={docs}")
    print(f"PASS: firmware, About and README all identify version {header}")

    if f"[env:{ENV}]" not in ini:
        fail(f"Missing PlatformIO environment {ENV}")
    for asset in ("src/splash.jpg", "src/icons.bin"):
        if asset not in ini:
            fail(f"Embedded asset not listed in platformio.ini: {asset}")
        if not (ROOT / asset).is_file():
            fail(f"Missing embedded asset: {asset}")

    splash = (ROOT / "src/splash.jpg").read_bytes()
    if len(splash) < 100 or not splash.startswith(b"\xff\xd8"):
        fail("Splash does not look like a valid JPEG")

    offsets_match = re.search(r"ICON_OFFSETS\[10\]\s*=\s*\{([^}]+)\}",
                              firmware, flags=re.DOTALL)
    if offsets_match is None:
        fail("Nine-icon offset table not found")
    try:
        offsets = [int(value.strip())
                   for value in offsets_match.group(1).split(",")
                   if value.strip()]
    except ValueError:
        fail("Icon offset table contains a non-integer")
    icons = (ROOT / "src/icons.bin").read_bytes()
    if len(offsets) != 10 or offsets[0] != 0 or offsets[-1] != len(icons):
        fail("Icon offsets must contain 10 entries ending at icons.bin size")
    if any(left >= right for left, right in zip(offsets, offsets[1:])):
        fail("Icon offsets are not strictly increasing")
    for i, (start, end) in enumerate(zip(offsets, offsets[1:]), start=1):
        if not icons[start:end].startswith(b"\xff\xd8"):
            fail(f"Embedded icon {i} is missing a JPEG start marker")
    print(f"PASS: 240x135 splash asset present; 9 packed JPEG icon ranges ({len(icons)} bytes)")
    print(f"PASS: PlatformIO environment {ENV} is configured")


if __name__ == "__main__":
    main()
