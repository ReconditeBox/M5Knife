# M5Knife — Cardputer Adv

First **development build**, independent of the Cardputer CP/M project.

## What is implemented

- Six-icon horizontal scrolling launcher: **Files**, **USB**, **WiFi**, **Bluetooth**, **Status**, and **About**. Three icons are visible at a time; About is an inactive placeholder. WiFi scans access points; Bluetooth scans nearby BLE advertisements; Status shows WiFi connection details and hardware addresses.
- Universal **G0** button returns to the launcher.
- Automatic SD card mount at boot using the Cardputer Adv's SPI SD connection.
- Browse SD directories, open a basic text preview, create folders, rename entries, delete files or **empty** directories with confirmation.
- SD capacity/used-space information under Files > Disk.
- Missing-SD handling and a retry key. The home screen checks card presence about every 2.5 seconds using a raw sector read when mounted, rather than unmounting and remounting each time. When missing, it periodically retries mounting. Files and R also retry.

**Not implemented yet:** FAT32 formatting; scrolling text viewer; recursive copy/move. USB Mass Storage has an initial experimental implementation using raw SD sectors and ESP32-S3 native USB OTG; it is NOT hardware validated. Back up the card before testing. Windows must safely eject it (or USB must disconnect) before using G0 to leave USB mode. Do not open Files while USB MSC is active.

## Keyboard

- Main launcher: `A` / `D` (or Fn + left/right arrows) moves horizontally across all six icons; Enter opens Files or USB. About does nothing yet; Status shows network details. WiFi and Bluetooth open scan results.
- Files: `W` / `S` (or Fn + up/down) scrolls; Enter opens; `B` goes up; `N` creates folder; `E` renames; `X` deletes (confirmation); `M` opens disk operations; `R` retries SD mount / refreshes.
- File preview: `B` backs out (preview currently shows beginning of file only).
- G0: returns to the main launcher from **any** screen; while USB MSC is active, first eject the drive in Windows or physically disconnect USB to reduce corruption risk.

## PlatformIO — Windows PowerShell

1. Extract this folder to your preferred PlatformIO projects directory.
2. Open the `M5Knife` folder in VS Code (File > Open Folder).
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

Delete the old `M5Knife` folder or extract this as a new project. If you reuse the same folder, remove `.pio/libdeps` and `.pio/build` before compiling so PlatformIO does not keep the previously downloaded M5GFX library.

## USB and hot-swap testing (experimental)

This version registers USB MSC when the firmware boots so Windows can enumerate an already-connected Cardputer. The disk is presented only while the USB icon is active. Use a disposable SD card, ideally inserted before power-on. Enter USB mode, then in Windows check **Device Manager** and **Disk Management**: a drive letter will only appear for media Windows can read. Safely eject the disk before G0.

**Known limitation:** the USB logical-unit capacity is established at firmware boot. A card inserted after boot may be detected by Files, but the USB logical unit cannot automatically resize for a different card. Reboot with the intended card inserted before testing USB. Windows may also cache device/media state; disconnecting/reconnecting USB after entering USB mode may be necessary. This implementation remains unverified on physical Cardputer Adv hardware. USB MSC must not be used with valuable data until its enumeration and write paths are validated.

## USB diagnostics

The USB screen now reports whether MSC registration succeeded, whether the USB device stack started, whether a USB host mounted the device, and the SD logical-sector count. These are *diagnostics*, not proof of a working removable drive. If Windows cannot see a drive, check Device Manager as well as Disk Management; do not initialise or format an unexpected drive. USB Mass Storage remains experimental.

## Display behaviour

The launcher redraw optimisation has been reverted pending USB regression testing. Navigation may flicker until a safer rendering change is implemented.

## WiFi and Bluetooth scanners (feature branch)

- **WiFi** runs an access-point scan and lists SSID, signal strength (dBm), and whether the AP is secured (`*`); hidden networks appear as `<hidden>`. It does not connect.
- **Bluetooth** scans **Bluetooth Low Energy (BLE)** advertisements for four seconds and lists names or addresses with signal strength. ESP32-S3 does not support Bluetooth Classic discovery.
- Press `W`/`S` to scroll, `R` to rescan, `B` or G0 to return home. Scans are currently synchronous; G0 cannot interrupt the few seconds while a scan is underway.
- Neither scanner pairs, connects or writes files. USB MSC implementation is unchanged on this branch.

## Status screen

Shows the WiFi SSID and IPv4 address only when connected (otherwise `Not connected`), plus the WiFi station MAC address and Bluetooth controller MAC address. `R` refreshes the values, `B` or G0 returns to the launcher. Viewing Status does not connect to WiFi or pair with Bluetooth.

## USB MSC performance diagnostics (test branch)

This isolated branch instruments the raw SD read/write callbacks without changing their transfer logic. The USB screen shows total reads/writes, failed requests, count of callbacks taking >=100ms, and maximum read/write callback time in milliseconds. Press `R` to refresh counters. Use a **spare** SD card and test Windows detection only; do not run format, repair, or write-speed tests until the results are inspected. G0 still requires host ejection or disconnection. These diagnostics are not validated by a local compile; do not merge until hardware testing confirms that build and USB access work.

## USB attachment timing experiment

The `fix/usb-msc-performance` branch now **electrically detaches the USB device after the USB stack starts**. Selecting USB checks that the SD card is ready, sets MSC media-present, and then calls TinyUSB `tud_connect()` to advertise a ready drive to Windows. After Windows safe-eject (or USB disconnection), G0 calls `tud_disconnect()` before withdrawing media. This is an experimental change; initial USB enumeration during device boot and Windows driver caching may still influence timing. The USB callback latency counters remain on-screen; press `R` to refresh them. Test on a disposable card and do not merge into `main` until Windows recognises storage quickly, Explorer remains responsive, file reads work, and safe ejection works. 

## WiFi connection (feature branch)

In WiFi, scan networks and highlight an SSID using W/S. Enter selects it; an encrypted network prompts for a password (input displayed as entered, without masking), while an open network connects immediately. Enter submits the password, Fn+backtick cancels, R rescans, and D disconnects. The connection attempt is asynchronous with an 18-second timeout and can be cancelled with G0. Status reports the SSID and IPv4 address once connected. WiFi passwords are not saved by the toolkit; the ESP32 WiFi system may persist configuration according to its framework settings. Hidden SSIDs and enterprise WiFi are not supported in this version. This feature has not yet been compile-tested or hardware-tested. USB MSC and SD code are unchanged.

## WiFi credentials on SD (experimental)

The SD card root `/WIFI.CFG` holds up to 16 network credentials as unencrypted plaintext, using numbered pairs and blank lines between networks:

```ini
SSID1=MyNetwork
PASS1=MyPassword

SSID2=Guest Network
PASS2=another-password
```

From the WiFi scan screen, highlight a network and press **L** to load that network's saved credentials and connect. Press **P** after successfully connecting with an entered password to add the network or update its existing password. Other stored networks are retained; the file is rewritten with consecutive numbers. **D** disconnects; **R** rescans. SSID/password values retain spaces, punctuation and equals signs. An SD card containing credentials must be kept secure: all passwords are readable by anyone who obtains the card or accesses it through USB MSC. Never use SD filesystem operations while USB MSC owns the card. This branch requires compilation and hardware testing.

## Infrared transmitter (experimental)

The **Infrared** launcher opens a NEC IR toolkit using Cardputer Adv's built-in IR emitter on **GPIO 44**. Select **Manual transmit** to enter hexadecimal address and command (A/C) and press Enter to send. **Remote control** provides POWER, VOLUP, VOLDOWN, CHUP, CHDOWN, MUTE commands driven by the file `/IR.CFG` on the microSD card. **Load SD profile** reads the file before using the remote; only NEC is supported so far.

Example `/IR.CFG` (example values only, *not* guaranteed to work with any equipment):

```ini
[REMOTE]
NAME=My TV
PROTOCOL=NEC
ADDRESS=0x00FF
POWER=0x02
VOLUP=0x62
VOLDOWN=0xA8
CHUP=0x22
CHDOWN=0xC2
MUTE=0xE2
```

Codes here use **NEC 16-bit address and 8-bit command**, not a raw 32-bit waveform code; convert codes accordingly. Unsupported or absent button entries are marked '(unset)'. The IR receiver/learning feature is intentionally not implemented (external receive hardware needed). Transmit only to equipment you own or have permission to control. No SD access occurs while USB MSC is active. This branch has not yet been compiled or hardware-tested.

### Samsung TV infrared test

`PROTOCOL=SAMSUNG` in `/IR.CFG` transmits Samsung32, using conventional MSB-first 32-bit code notation. The 16-bit ADDRESS is the first four hex digits of the full code, and each button's 8-bit command is the following two digits; the final two digits are the inverse of the command. For example `E0E040BF` means address `0xE0E0`, command `0x40`.

```ini
[REMOTE]
NAME=Samsung TV
PROTOCOL=SAMSUNG
ADDRESS=0xE0E0
POWER=0x40
VOLUP=0xE0
VOLDOWN=0xD0
CHUP=0x48
CHDOWN=0x08
MUTE=0xF0
```

The transmitter uses IRremote's MSB-first Samsung routine for this input convention. Load the profile from the Infrared menu, enter Remote control, and try MUTE or VOLUP first. Codes are a common Samsung set and may not work for every television model. The manual transmitter remains NEC-only in this initial release; Samsung support applies to SD remote profiles. Hardware verification is pending.

## Network tools (experimental)

The launcher now has **Network** immediately after WiFi. WiFi remains responsible for connecting/disconnecting and managing `WIFI.CFG`; Network provides **Network information** (IPv4 address, gateway, netmask, DNS, WiFi MAC, SSID, signal strength), **DNS lookup** (hostname to IPv4) and **Ping host** (three ICMP echo requests, timing or timeout). Use W/S and Enter to choose, type the hostname/IP address in the input screen, Enter to run, Fn+backtick to cancel and G0 for Home. Network tools require WiFi connectivity. Ping uses a raw ICMP socket and may be blocked by a destination or network firewall. This branch is not yet compiled or hardware-tested; the tested `main` version remains unchanged.

### Network tools — stage two (experimental)

- **TCP port scan**: enter a hostname or IPv4 address. Checks only common TCP ports (21,22,23,25,53,80,110,139,443,445,3389,8080) with 180 ms per-port connection timeouts. Reports ports that accept connections; this does not test UDP.
- **LAN scanner**: on a /24 subnet only, checks the first 32 host addresses for reachable TCP port 80 or 443 (75 ms timeout per port). This is **not** a full subnet or comprehensive device discovery scan; devices without those open ports are omitted.
- **TCP client**: enter `host:port`, press Enter, enter one optional text line and press Enter again. It sends the line with CRLF, reads up to 350 response bytes for about 1.6 seconds, then closes. It does not speak TLS/HTTPS and is not a full interactive terminal.
  
Use only on networks and devices you are authorised to inspect. Results are shown on the small built-in screen and may be truncated. Network connections can take a few seconds, during which the UI may temporarily be unresponsive. Existing Ping/DNS/Network Info remain unchanged. This code is not yet compiled or hardware-tested and must not be merged into `main` until verified.
