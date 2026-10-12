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

The `[WIFI]` section of `/M5KNIFE.CFG` holds up to 16 network credentials as unencrypted plaintext, using numbered pairs and blank lines between networks:

```ini
SSID1=MyNetwork
PASS1=MyPassword

SSID2=Guest Network
PASS2=another-password
```

From the WiFi scan screen, highlight a network and press **L** to load that network's saved credentials and connect. Press **P** after successfully connecting with an entered password to add the network or update its existing password. Other stored networks are retained; the file is rewritten with consecutive numbers. **D** disconnects; **R** rescans. SSID/password values retain spaces, punctuation and equals signs. An SD card containing credentials must be kept secure: all passwords are readable by anyone who obtains the card or accesses it through USB MSC. Never use SD filesystem operations while USB MSC owns the card. This branch requires compilation and hardware testing.

## Infrared transmitter (experimental)

The **Infrared** launcher opens a NEC IR toolkit using Cardputer Adv's built-in IR emitter on **GPIO 44**. Select **Manual transmit** to enter hexadecimal address and command (A/C) and press Enter to send. **Remote control** provides POWER, VOLUP, VOLDOWN, CHUP, CHDOWN, MUTE commands driven by the `[REMOTE]` section of `/M5KNIFE.CFG` on the microSD card. **Load SD profile** reads the file before using the remote; only NEC is supported so far.

Example `[REMOTE]` section in `/M5KNIFE.CFG` (example values only, *not* guaranteed to work with any equipment):

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

`PROTOCOL=SAMSUNG` in the `[REMOTE]` section of `/M5KNIFE.CFG` transmits Samsung32, using conventional MSB-first 32-bit code notation. The 16-bit ADDRESS is the first four hex digits of the full code, and each button's 8-bit command is the following two digits; the final two digits are the inverse of the command. For example `E0E040BF` means address `0xE0E0`, command `0x40`.

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

The launcher now has **Network** immediately after WiFi. WiFi remains responsible for connecting/disconnecting and managing `M5KNIFE.CFG`; Network provides **Network information** (IPv4 address, gateway, netmask, DNS, WiFi MAC, SSID, signal strength), **DNS lookup** (hostname to IPv4) and **Ping host** (three ICMP echo requests, timing or timeout). Use W/S and Enter to choose, type the hostname/IP address in the input screen, Enter to run, Fn+backtick to cancel and G0 for Home. Network tools require WiFi connectivity. Ping uses a raw ICMP socket and may be blocked by a destination or network firewall. This branch is not yet compiled or hardware-tested; the tested `main` version remains unchanged.

### Network tools — stage two (experimental)

- **TCP port scan**: enter a host; check 12 common TCP ports and report only those accepting connections.
- **LAN scanner**: edit an inclusive range in the form `192.168.1.1-192.168.1.254`. The default is derived from WiFi's local address and subnet mask (bounded to at most 256 IP addresses). Lists only hosts answering TCP connection attempts and their open ports (21,22,23,53,80,139,443,445,3389,8080). W/S scrolls result lines. Other hosts, including devices that block TCP connections, are not detectable with this method. A full /24 range can take a while to scan.
- **TCP client**: removed for now.

Use only on networks and devices you are authorised to inspect. This revision is not yet compiled or hardware-tested. Tested `main` remains unchanged.

## Scrollable Status metrics (experimental)

The Status screen shows WiFi SSID and IPv4, WiFi/Bluetooth MAC addresses, SD total/used/free capacity in MiB, ESP32 heap size/free/minimum-free values in KiB, and the Cardputer battery percentage and voltage. W/S (or the keyboard's up/down keys) scroll through seven visible rows; R refreshes readings; B or G0 returns home. SD usage may take time to calculate depending on card contents. This branch has not yet been compiled or tested on the Cardputer Adv; `main` is unchanged.

## High-priority tools (experimental branch)

- **WiFi auto-connect:** At startup, if `AUTO_WIFI=1`, M5Knife attempts to connect to the first saved network in `/M5KNIFE.CFG` [WIFI] section without interrupting the launcher. Disable under Settings if unwanted. The saved WiFi profiles are still maintained in the existing WiFi menu.
- **NTP clock:** Once WiFi connects, synchronises using pool.ntp.org/time.google.com if enabled. The synchronised local date/time appears near the bottom of the scrollable Status page. Settings provides a whole-hour UTC offset; daylight-saving changes are not automatic.
- **Settings:** A ninth launcher tile opens settings for WiFi auto-connect, NTP sync, UTC offset, display brightness and display sleep timeout. W/S selects a row; A/D changes values. Choose **Save settings** to write `/M5KNIFE.CFG` on the SD card. Sleep turns display brightness off after inactivity; the keyboard or G0 wakes it. SD is required for persistence.
- **Text viewer:** Files now opens a RAM-backed 8 KiB maximum text viewer. W/S scroll, F searches text, E enters editor, Fn+Escape returns to Files.
- **Text editor:** The editor supports typing, deleting before the cursor, newline insertion, cursor navigation via Fn+cursor keys, and save using Fn+S. Edited files are saved using temporary/backup files. Unsaved changes block the normal editor exit; G0 can leave without saving. Edit only plain text files up to 8192 bytes. In Files, T creates a blank text file in the current directory, after which it can be opened and edited. This is a deliberately basic first version.

This branch is **not yet compiled or hardware-tested**. Stable `main` remains at M5Knife 0.4.5 until test results are confirmed.

## HTTP File Server (experimental)

Open **Network > HTTP File Server** after connecting to WiFi and inserting an SD card. The Cardputer shows a local `http://<device-ip>/` URL and the username `m5knife` plus a newly generated eight-digit password for that session. Open the address in a browser on the same local network; authenticate using those credentials. HTTP uses TCP port 80.

The web interface supports navigating SD folders, downloading files, uploading new files, creating folders, renaming items and deleting files or **empty** folders. Uploads do **not** overwrite existing files. File paths are checked to reject relative directory traversal and malformed names. The SD capacity and usage appear on the browser page.

The server is started **only** in HTTP File Server mode, stopped with B or G0, and stopped if WiFi disconnects. USB mass storage remains a separate screen/mode and is not entered while HTTP File Server owns the SD filesystem.

**Security:** This is HTTP, **not HTTPS**. The password is transmitted using HTTP Basic authentication and is not encrypted on the network. Only use this mode on trusted local networks, never expose port 80 to the internet or forward it through your router. Anyone with the login information and network access can read, change and delete SD files; keep backups. There is no guest/read-only role.

This branch is not yet compiled or hardware-tested. Stable `main` is unchanged at M5Knife 0.4.6.

### Two-pane web file manager (Stage 1)

The browser interface now has a dark M5Knife-style header, an SD usage bar, a **collapsible folder tree** at left and a **file list** for the current directory at right. Click a triangle to expand/collapse a branch, or click its name to open it. Branch contents load on demand using `GET /tree?dir=...`; the server returns only directories in JSON. The current folder is highlighted in the tree, with its path displayed above the list. The Refresh button reloads the page.

Existing upload, download, rename, delete and new-folder actions remain on the page. The file table shows name and size; modified timestamps are deliberately omitted pending reliable SD timestamp support. The browser page and file listing currently have a per-directory display cap (256 entries; folder branches 128), and large directories may use significant ESP32 memory when building the HTML page. Password credentials are unchanged (username `m5knife`, password `1234`). This experimental UI has not been compiled or hardware-tested yet.

### Browser file previews (development)

In the HTTP file manager, clicking a recognised file opens it in a browser tab with its appropriate MIME type: plain text/source files, CSV, JSON, PNG/JPEG/GIF/WebP/BMP/ICO images, PDF, MP3/WAV/OGG audio, and MP4/WebM video. The **Download** link still forces a download. Unrecognised or potentially active content (HTML, SVG, JavaScript) is downloaded instead of executed from the Cardputer server. The change retains HTTP Basic authentication (`m5knife` / `1234`) and has not yet been build- or hardware-tested.

### Unified SD configuration (v0.4.9 development)

The firmware uses one SD-card file, `/M5KNIFE.CFG`, split into `[SETTINGS]`, `[WIFI]`, `[REMOTE]`, and `[HTTPD]` sections. It no longer reads or migrates separate configuration files. Section updates preserve the other sections; an intermediate temporary file and backup are used when updating the configuration.

Example (replace with your own values; IR button names match the existing profile):
```ini
[SETTINGS]
AUTO_WIFI=1
NTP=1
BRIGHTNESS=128
SLEEP_MIN=0
TZ_HOURS=0

[WIFI]
SSID1=MyNetwork
PASS1=MyPassword

[REMOTE]
NAME=My remote
PROTOCOL=NEC
ADDRESS=00FF
POWER=45
VOLUP=46
VOLDOWN=47
CHUP=48
CHDOWN=49
MUTE=4A

[HTTPD]
USERNAME=m5knife
PASSWORD=1234
```

**SLEEP_MIN=0 means never sleep**, and is the default. Set `USERNAME` and `PASSWORD` in the `[HTTPD]` section to change HTTP Basic login; if omitted, defaults remain `m5knife` / `1234`. The password is stored in plaintext on the SD card and transmitted over unencrypted HTTP; use only on a trusted network. Stop HTTPD before editing its configuration, and restart the Cardputer to load changes. This revision needs compilation and on-device testing before merge.

### M5Knife 0.5.11 development: NVS settings with SD import/export

The ESP32-S3's internal non-volatile flash (NVS, via Preferences) is now the **primary** configuration store. Boot reads NVS without needing an SD card. If no saved NVS configuration exists yet, the firmware imports `/M5KNIFE.CFG` from the SD card once; otherwise it initializes built-in defaults (including `SLEEP_MIN=0`, meaning Never). Subsequent boots do not automatically overwrite NVS from SD. Routine Settings and Wi-Fi saves write to NVS and retain other configuration sections; infrared profiles and HTTPD credentials are also read from NVS.

Two Settings menu actions manage the same human-editable INI-style file: **Import config from SD** (replaces NVS configuration from `/M5KNIFE.CFG`) and **Export config to SD** (writes current NVS configuration to that file). Import checks file size and required sections before writing; take a backup before experimenting. Export uses a temporary file and backup during updates. The firmware does not erase the NVS partition on normal firmware upload; a full-flash erase or repartitioning can remove settings. NVS contains plaintext Wi-Fi and HTTPD credentials and is not encrypted by this firmware.

This is a development branch, version **0.5.11**, not yet compiled or hardware-tested. Stable `main` remains **0.4.9**.

### HTTP File Server QR code (development branch)

While the HTTP File Server is running under **Network → HTTP File Server**, press **Q** (or **Enter**) to show a QR code containing the current `http://<Cardputer-LAN-IP>` URL. Scan it with a phone on the same reachable network to open the existing browser-based file manager. Press **Q** or **Enter** again to return to the address/credentials/status display. Press **B** or **G0** to stop the HTTP server; showing the QR code does **not** stop serving web requests. **R** refreshes whichever HTTP display is showing.

The QR code uses the QR renderer already in M5GFX, with a white border for scanning on the Cardputer's small screen. HTTP Basic authentication still applies; the QR code holds only the server URL, not the username or password. Keep the server on a trusted network. No additional library dependency is needed.

The HTTP QR-code view has been accepted as working on the Cardputer for M5Knife 0.5.12.

## Wi-Fi hotspot and HTTP file server — 0.5.13 development

The **Network** menu now has two HTTP options:

- **HTTP File Server:** Keeps the existing router-connected mode if Wi-Fi is already connected. If not connected, starts a temporary hotspot automatically.
- **HTTP Hotspot:** Explicitly creates a hotspot even if the Cardputer was connected to a router. This temporarily disconnects the station connection; the normal auto-connect attempt resumes when the hotspot closes.

The Cardputer hotspot is a password-protected Wi-Fi access point. Defaults are **SSID: `M5Knife`**, **Wi-Fi password: `M5Knife1234`**. These are separate from the HTTP Basic authentication credentials (still `m5knife` / `1234` unless changed under `[HTTPD]`). The Cardputer shows its current IP/URL and both sets of credentials. Join its Wi-Fi network from a phone or computer, then open the displayed `http://...` address; the typical default hotspot address is `http://192.168.4.1`. Your device might warn that the hotspot has no Internet access; choose to remain connected.

Within HTTPD, press **Q** or **Enter** to cycle between the normal status screen, a **join Wi-Fi QR code** and a **file-server URL QR code** while hotspot mode is running. In normal router mode, Q switches between status and the URL QR code. The HTTP server continues handling requests while QR codes are shown. **B** or **G0** stops the web server and its own hotspot. This does not erase NVS or change Wi-Fi profiles.

Optional custom hotspot credentials can be specified in the existing NVS-backed `/M5KNIFE.CFG` import/export format:

```ini
[HOTSPOT]
SSID=M5Knife
PASSWORD=M5Knife1234
```

The SSID must be 1–32 bytes, and the WPA2 password 8–63 bytes. Change the SD file, then use **Settings → Import config from SD** (or reboot after a fresh NVS initialization) to load it; changes take effect the next time HTTPD starts. The `[HOTSPOT]` section is optional and built-in defaults are used otherwise. Settings exported from existing NVS without this section still work.

The hotspot provides a local connection to M5Knife, not Internet access. The HTTP file manager remains fully read/write, and the protocol is **plain HTTP**, so use only with devices you trust. For security, consider setting distinct private HTTP credentials through `[HTTPD]`.

**This feature is on `feature/wifi-hotspot` for compile and physical testing. Stable `main` remains M5Knife 0.5.12 until testing and approval.**
