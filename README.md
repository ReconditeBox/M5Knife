# M5 Toolkit — Cardputer Adv

First **development build**, independent of the Cardputer CP/M project.

## What is implemented

- Six-icon horizontal scrolling launcher: **Files**, **USB**, **WiFi**, **Bluetooth**, **Status**, and **About**. Three icons are visible at a time; WiFi, Bluetooth, Status, and About are inactive placeholders.
- Universal **G0** button returns to the launcher.
- Automatic SD card mount at boot using the Cardputer Adv's SPI SD connection.
- Browse SD directories, open a basic text preview, create folders, rename entries, delete files or **empty** directories with confirmation.
- SD capacity/used-space information under Files > Disk.
- Missing-SD handling and a retry key. The home screen rechecks card presence about every 2.5 seconds; Files and R also retry mounting.

**Not implemented yet:** FAT32 formatting; scrolling text viewer; recursive copy/move. USB Mass Storage has an initial experimental implementation using raw SD sectors and ESP32-S3 native USB OTG; it is NOT hardware validated. Back up the card before testing. Windows must safely eject it (or USB must disconnect) before using G0 to leave USB mode. Do not open Files while USB MSC is active.

## Keyboard

- Main launcher: `A` / `D` (or Fn + left/right arrows) moves horizontally across all six icons; Enter opens Files or USB. WiFi, Bluetooth, Status, and About do nothing yet.
- Files: `W` / `S` (or Fn + up/down) scrolls; Enter opens; `B` goes up; `N` creates folder; `E` renames; `X` deletes (confirmation); `M` opens disk operations; `R` retries SD mount / refreshes.
- File preview: `B` backs out (preview currently shows beginning of file only).
- G0: returns to the main launcher from **any** screen; while USB MSC is active, first eject the drive in Windows or physically disconnect USB to reduce corruption risk.

## PlatformIO — Windows PowerShell

1. Extract this folder to your preferred PlatformIO projects directory.
2. Open the `M5-Toolkit` folder in VS Code (File > Open Folder).
3. Insert an SD card (preferably FAT32, backed up) in the Cardputer Adv.
4. Connect the Cardputer Adv to your PC by USB.
5. In VS Code choose Terminal > New Terminal. Run:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -e m5stack-cardputer-adv
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -e m5stack-cardputer-adv -t upload
```

For serial logs, close any other serial monitor and run (adjust COM port):

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" device monitor -b 115200 -p COM3
```

Dependency versions are pinned to avoid unexpectedly pulling newer M5GFX releases. The first compile downloads dependencies. Hardware testing is required: this code is an initial scaffold and has not yet been compiled/flashed on the device in this environment.

## Safety

- Always back up a microSD card before testing filesystem operations.
- Deleting a file and renaming are real operations; formatting and MSC are *not* active.
- G0 exits current UI screens; it cannot cancel a synchronous SD write in progress.
- USB MSC is experimental; test with a disposable FAT32 card, never the only copy of valuable data.
- The build now uses native USB OTG mode (`ARDUINO_USB_MODE=0` and `ARDUINO_USB_CDC_ON_BOOT=0`). This can change serial monitor and upload behaviour. If normal upload fails, use the ESP32-S3 download/BOOT mode and the correct serial port.

## If changing from an older copy

Delete the old `M5-Toolkit` folder or extract this as a new project. If you reuse the same folder, remove `.pio/libdeps` and `.pio/build` before compiling so PlatformIO does not keep the previously downloaded M5GFX library.
