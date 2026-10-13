# M5Knife

**A standalone utilities toolkit for the M5Stack Cardputer Adv.**

**Current firmware: 0.5.20**

M5Knife combines an SD card file manager, USB card reader, Wi-Fi and network tools, Bluetooth scanning, infrared remotes and system information in a small keyboard-driven launcher. It also runs an HTTP file manager that can be reached over an existing Wi-Fi network or directly from a phone using the Cardputer's own hotspot.

This is the **M5Stack Cardputer Adv** project, not the separate Cardputer CP/M firmware.

## Contents

- [Getting started](#getting-started)
- [Keyboard controls](#keyboard-controls)
- [Launcher and tools](#launcher-and-tools)
- [HTTP file manager and hotspot](#http-file-manager-and-hotspot)
- [Infrared remote library](#infrared-remote-library)
- [Configuration and NVS](#configuration-and-nvs)
- [Troubleshooting](#troubleshooting)
- [Safety and current limitations](#safety-and-current-limitations)

## Getting started

### Requirements

- M5Stack **Cardputer Adv** with USB connection.
- A PC with **Visual Studio Code**, **PlatformIO**, and Git installed.
- A microSD card, preferably formatted FAT32, for file operations, HTTP file serving and USB Mass Storage. **Back up its contents first.** Basic launcher, settings, Wi-Fi, Bluetooth, Status and IR profile selection do not require the SD card.
- The firmware source from [ReconditeBox/M5Knife](https://github.com/ReconditeBox/M5Knife).

This project uses PlatformIO environment `m5stack-cardputer-adv`, Espressif32 `6.7.0`, Arduino, and the `esp32-s3-devkitc-1` board definition. Dependency versions are pinned in `platformio.ini`.

### Build and upload (Windows PowerShell)

If you don't have the repository locally yet, open **PowerShell**, go to the directory where you keep your PlatformIO projects, then clone it:

```powershell
git clone https://github.com/ReconditeBox/M5Knife.git
cd M5Knife
```

If you already have M5Knife locally, open PowerShell **in that project folder** and update the stable branch:

```powershell
git switch main
git pull origin main
```

Compile:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -e m5stack-cardputer-adv
```

Connect the Cardputer by USB, then upload (replace `COM3` if your device uses a different port):

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -e m5stack-cardputer-adv -t upload --upload-port COM3
```

To open a 115200 baud serial monitor:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" device monitor -b 115200 -p COM3
```

If you need to find the COM port, open **Windows Device Manager → Ports (COM & LPT)**. Close the serial monitor before uploading. USB MSC uses the ESP32-S3's native USB mode, which may change enumeration and available serial ports; use the device's download/BOOT mode if normal upload fails.

## Startup splash

When M5Knife powers on, it displays the supplied M5Knife Swiss Army knife and
mountain artwork full-screen at **240 × 135 pixels** for a minimum of two seconds.
The image is embedded directly in program flash as `src/splash.jpg` using
PlatformIO's `board_build.embed_files` option and is drawn with M5GFX's JPEG
decoder. An SD card, Wi-Fi connection or separate image file on the card is
**not** required. Startup initialisation runs while the splash is displayed;
the normal nine-icon launcher appears afterwards.

## Keyboard controls

M5Knife's launcher displays **three tiles at a time** and scrolls horizontally through all nine tools.

| Key | Action |
| --- | --- |
| `,` (←) / `/` (→) | Move left / right without Fn in the launcher and navigation screens |
| `;` (↑) / `.` (↓) | Move up / down without Fn in menus, lists and other navigation screens |
| `A` / `D` | Alternative left / right navigation |
| `W` / `S` | Alternative up / down navigation |
| `Fn + ;` / `,` / `.` / `/` | Move the cursor in the built-in text editor, leaving the unmodified punctuation keys available for typing |
| `Enter` | Select, open, or activate |
| `B` | Back in most menus |
| **Fn + backtick** | Escape/cancel in input and editor screens |
| **Fn + S** | Toggle random-pitch key beeps ON/OFF (except while editing a text file, where it still saves) |
| **G0** | Return to the main launcher; USB mode protects against unsafe exit |
| `R` | Refresh where offered |

**Keypresses are silent by default.** Press **Fn+S** from the launcher or another screen to toggle short, randomly pitched speaker beeps for keyboard presses and G0. A temporary footer message confirms the new mode; the choice is saved to NVS, so it survives a restart. In the built-in text editor, Fn+S **still saves the file** instead of changing the sound mode. To change sound while editing, exit the editor first.

**Fn is the Cardputer's `Aa` key.** The arrow symbols printed on the `; , . /` keys now navigate menus **without** Fn. In text entry fields these keys still type punctuation; in the built-in editor, hold Fn for cursor movement. The keys shown in the footer of each screen take precedence over these general shortcuts.

## Launcher and tools

### Icon refresh in 0.5.20

The launcher now uses **nine individually illustrated metallic red-and-chrome
icons** based on the approved M5Knife mountain/Swiss-army-knife concept:
Files, USB, WiFi, Network, Bluetooth, Infrared, Settings, Status and About.
Each is a compact **64 × 53 pixel JPEG**, shown inside the existing tile,
with a green selection outline and the tool's label below it.

The images are packed in `src/icons.bin` (about **14 KiB total**) and
embedded in firmware flash via `board_build.embed_files`, alongside the
startup splash. They **do not need a microSD card** and require no additional
runtime dependencies. The three-at-a-time horizontal launcher and all
controls are unchanged.

### Interface refinements in 0.5.18

The visual style and existing nine-tool layout are unchanged, but navigation is smoother:
moving between icons or list rows repaints the content without clearing and
redrawing the header and footer every time. The longer Files, Wi-Fi/Bluetooth,
Status, About, Settings, network-result and infrared-library lists now show a
thin position indicator at the right edge. The Settings list scrolls within
the available screen space instead of drawing across the footer.

Short text entry (file names, Wi-Fi passwords, network targets and search
fields), as well as the built-in text viewer/editor, also uses lighter redraws.
Information messages have a clearer border
and wrap long lines instead of letting text run off the display. These are
presentation changes only; tool functions and shortcuts remain the same.

| Tool | What it does |
| --- | --- |
| **Files** | Browse SD files and directories; view, search and edit small text files; create, rename and delete entries |
| **USB** | Expose the SD card as a USB Mass Storage device to a connected computer |
| **WiFi** | Scan and connect to wireless networks; save credentials for later use |
| **Network** | Network information, DNS lookup, ICMP ping, TCP port scan, LAN scanner, HTTP server and hotspot |
| **Bluetooth** | Scan nearby Bluetooth Low Energy advertisements |
| **Infrared** | NEC manual transmission and a saved NEC/Samsung remote library |
| **Settings** | Wi-Fi autoconnect, NTP, timezone, brightness, sleep, NVS save and SD configuration import/export |
| **Status** | Wi-Fi, hardware addresses, SD space, heap, battery and clock |
| **About** | Firmware and device information |

### Files and built-in text editor

The **Files** screen lists directories before files. Navigate with the up/down arrow-labelled keys (`;` / `.`) or W/S and open with Enter.

| Key | Files action |
| --- | --- |
| `B` | Go to parent directory |
| `N` | Create a directory |
| `T` | Create a blank text file |
| `E` | Rename selected file or directory |
| `X` | Delete selected file or **empty** directory, with confirmation |
| `M` | Open SD disk information / format menu |
| `R` | Refresh or retry mounting SD |

Opening a file loads the built-in text viewer; the maximum file size is **8 KiB**. In the viewer, W/S scroll, F finds text and E switches to the editor. In edit mode, use **Fn + arrow-labelled keys** to move the cursor, Del and Enter to change text; ordinary letters (including W/A/S/D) and punctuation remain typeable. **Fn+S** saves. Use **Fn + backtick** to exit the viewer/editor. Files larger than 8 KiB are not supported by this viewer. Carriage returns are removed when text is loaded, so saving can change CRLF line endings to LF: do not use the editor on files whose exact bytes must be preserved.

Saves use a temporary file and a backup/rename sequence. The disk menu reports SD capacity and usage. **SD formatting is displayed but deliberately disabled.** Recursive copying/moving files or directories is not implemented.

### USB Mass Storage

Open **USB** to present the microSD card to a USB-connected computer. While the computer has the card, M5Knife does not simultaneously use the FAT filesystem.

**In Windows, safely eject the drive before pressing G0.** The USB screen reports connection state and read/write/error timing diagnostics; G0 is intentionally restricted if the host still has the media mounted or is writing. If the drive does not appear promptly, check Windows Disk Management and try disconnecting/reconnecting the USB cable as appropriate. For the most reliable USB media capacity detection, insert the intended card before starting the firmware.

Never unplug the card, write to it from both sides or use your only copy of important data during USB tests.

### Wi-Fi

Open **WiFi** to scan networks. Select a network and press Enter to connect; enter a password if required.

- `P`: save the connected network's credentials to NVS.
- `L`: connect to the highlighted network using saved credentials, if available.
- `D`: disconnect.
- `R`: rescan.

Up to **16 network credential records** can be stored in the unified configuration. With **Auto WiFi** enabled in Settings, startup tries the first saved network. Credentials are retained in NVS even without an SD card.

### Network utilities

The **Network** menu provides:

- **Network info:** IP address, gateway, subnet mask, DNS, MAC, SSID and signal strength.
- **DNS lookup:** resolve a hostname to an IPv4 address.
- **Ping:** send three ICMP echo requests.
- **TCP port scan:** test a fixed set of common ports on a selected host.
- **LAN scanner:** test a range of up to 256 IPv4 addresses and list hosts that respond on the scanner's tested TCP ports.
- **HTTP File Server** and **HTTP Hotspot:** described below.

DNS, ping and scanning need a normal Wi-Fi connection. The LAN scanner is **TCP-based**, not a full device-discovery or ICMP scanner: a host with no open tested ports may not appear. Scan only networks and devices you are authorised to test.

### Bluetooth

**Bluetooth** scans nearby **BLE advertisements** and displays discovered devices, their advertised names (where available) and signal levels. This is a scanner, not a Bluetooth terminal or pairing manager.

### Settings, Status and About

**Settings** offers Auto WiFi, NTP sync, UTC offset, brightness, sleep timeout (including **Never**, the default), Save settings, Sync time now, and configuration Import/Export. Select a row using the up/down arrow-labelled keys or W/S; use left/right arrow-labelled keys, A/D or Enter to change/activate it. Choose **Save settings** to persist manual settings changes in NVS. The Fn+S key-beep switch saves its own setting immediately without saving unrelated unsaved settings.

**Status** is scrollable and reports connected Wi-Fi and IP, Wi-Fi/Bluetooth MAC addresses, SD total/used/free space, current/minimum/total heap, battery percentage/voltage and local clock. **About** identifies the firmware, version and hardware. From **About**, press **Enter** or **Q** on the highlighted **GitHub QR** option to display a scannable link to https://github.com/ReconditeBox/M5Knife. Press **B**, **Enter** or **Q** to return to About, or **G0** to return home. The QR code is generated locally and does not need Wi-Fi or an SD card.

## HTTP file manager and hotspot

From **Network**, choose one of these:

| Mode | Behaviour |
| --- | --- |
| **HTTP File Server** | Serves the SD card using the existing Wi-Fi connection. If not connected, starts its own hotspot instead. |
| **HTTP Hotspot** | Explicitly starts a standalone wireless network, even if the Cardputer had been connected to a router. The station connection is temporarily dropped. |

The browser interface has a **folder tree and file list** and supports uploads, downloads, creating directories, renaming entries, and deleting files or empty directories. It can display supported text and media types in the browser. Unrecognised or unsafe-to-render types are downloaded instead. **Browser-based text editing is not yet implemented.** Uploads do not silently overwrite files with the same name.

The server listens on **HTTP port 80** and runs only while its HTTP screen is open. Press **B** or **G0** to stop it; when M5Knife created a hotspot, the hotspot stops too.

### Connect using an existing Wi-Fi network

1. Connect the Cardputer to Wi-Fi.
2. Open **Network → HTTP File Server**.
3. On a phone or computer that can reach the Cardputer, visit the **`http://` URL shown on the screen**.
4. Sign in with the displayed HTTP credentials.
5. Press **Q** or **Enter** on the Cardputer to show the URL QR code; repeat to return to status.

### Connect directly using the M5Knife hotspot

Default values (customisable through `[HOTSPOT]` and `[HTTPD]`):

| Setting | Default |
| --- | --- |
| Hotspot SSID | `M5Knife` |
| Hotspot Wi-Fi password | `M5Knife1234` |
| Typical hotspot URL | `http://192.168.4.1/` |
| HTTP username | `m5knife` |
| HTTP password | `1234` |

1. Open **Network → HTTP Hotspot** (requires the SD card).
2. Join the **M5Knife** Wi-Fi network from the phone or computer.
3. Open the exact **`http://` address shown on the Cardputer** and sign in.
4. Use **Q** or **Enter** to cycle between **status → join-Wi-Fi QR → server-URL QR → status**.
5. Press **B** or **G0** on the Cardputer to stop the server and hotspot.

The hotspot offers **local access only, not Internet access**. Some phones will prefer mobile data when connected to Wi-Fi without Internet. If the page will not load, temporarily **turn off mobile data**, stay connected to the M5Knife Wi-Fi network and explicitly browse to **`http://192.168.4.1/`** (not HTTPS). This was necessary during phone testing of the hotspot.

**Security:** HTTP Basic credentials and file transfers use **unencrypted HTTP**, and default credentials are publicly documented. Change the defaults for your own setup, do not expose port 80 to the Internet, and use only trusted devices and networks.

## Infrared remote library

M5Knife transmits IR through the Cardputer Adv's built-in emitter on **GPIO 44**.

**Infrared** provides **Manual transmit** (NEC address and command in hex), **Remote control** (six configurable buttons) and **Remote library**. The supported saved-profile protocols are **NEC** and **Samsung**.

The library accepts **up to eight profiles**, including the original `[REMOTE]` format as profile slot 1. Other slots use `[REMOTE2]` through `[REMOTE8]`. A `[REMOTE1]` section can be used in place of `[REMOTE]`. Choose a remote using W/S and Enter in **Infrared → Remote library**, or press **L** from the remote-control screen. The active profile is marked with an asterisk and saved in **NVS**, so it is restored after restart without an SD card.

The remote-control buttons are `POWER`, `VOLUP`, `VOLDOWN`, `CHUP`, `CHDOWN` and `MUTE`; missing commands display as **(unset)**. NEC uses a 16-bit address and 8-bit command. Samsung32 codes use the conventional MSB-first form: `E0E040BF` corresponds to address `0xE0E0` and command `0x40`.

To **add or modify** profiles, edit the configuration text file and import it using **Settings → Import config from SD**. At present, the Cardputer keypad supports **selecting** saved profiles, not authoring new ones. IR receiving/learning would need separate receiver hardware and is not implemented.

## Configuration and NVS

**ESP32-S3 NVS flash is the primary configuration store.** It keeps settings (including the keyboard beep mode), saved Wi-Fi credentials, HTTP login, hotspot options and infrared profiles. The microSD root file **`/M5KNIFE.CFG`** is an **optional human-readable import/export copy**, not a requirement for ordinary startup.

On first startup without valid NVS configuration, M5Knife tries to import `/M5KNIFE.CFG` if present; otherwise it creates default configuration in NVS. In **Settings**:

- **Save settings** writes the current on-screen settings to NVS.
- **Export config to SD** writes the current NVS-backed configuration to `/M5KNIFE.CFG`, using a temporary file and backup during replacement.
- **Import config from SD** replaces the NVS configuration from the SD file and reloads settings and IR profiles. **Back up your working configuration before importing.**

The import file must contain at least `[SETTINGS]` and `[HTTPD]` and cannot exceed **16 KiB**. Other sections are optional. Network passwords and HTTP login details are stored **in plaintext**, both in the exported file and in this firmware's NVS contents. Avoid publishing a real configuration file.

### Example M5KNIFE.CFG

The codes and wireless details below are **examples**, not working credentials or guaranteed IR commands for your equipment.

```ini
[SETTINGS]
AUTO_WIFI=1
NTP=1
TZ_HOURS=0
BRIGHTNESS=128
SLEEP_MIN=0
KEY_BEEP=0

[WIFI]
SSID1=ExampleNetwork
PASS1=replace-with-real-password

[HTTPD]
USERNAME=m5knife
PASSWORD=replace-this-default

[HOTSPOT]
SSID=M5Knife
PASSWORD=M5Knife1234

[REMOTE]
NAME=Example Samsung TV
PROTOCOL=SAMSUNG
ADDRESS=0xE0E0
POWER=0x40
VOLUP=0xE0
VOLDOWN=0xD0
CHUP=0x48
CHDOWN=0x08
MUTE=0xF0

[REMOTE2]
NAME=Example NEC remote
PROTOCOL=NEC
ADDRESS=0x00FF
POWER=0x02
VOLUP=0x62
VOLDOWN=0xA8
CHUP=0x22
CHDOWN=0xC2
MUTE=0xE2

[IR_LIBRARY]
SLOT=1
```

`KEY_BEEP=0` means silent keyboard (default); `KEY_BEEP=1` enables the short random-pitch key clicks. The Fn+S toggle updates this value directly in NVS, and configuration export includes it.

Additional Wi-Fi records use `SSID2`/`PASS2`, etc., up to 16. Additional remote sections run through `[REMOTE8]`. `[IR_LIBRARY]` records the selected slot; normally the firmware updates it when you select a profile.

Hotspot SSIDs must be **1–32 bytes** and their WPA2 passwords **8–63 bytes**. If the `[HOTSPOT]` section is omitted, the default SSID and password are used.

## Troubleshooting

| Symptom | What to check |
| --- | --- |
| Build or upload fails | Open PowerShell inside the correct project; select the `m5stack-cardputer-adv` environment; verify the COM port and that no serial monitor is using it. |
| SD card is missing | Reseat a FAT32 card; from Files use `R` to retry mounting; check the top-bar SD icon. |
| Windows cannot see the USB disk | Enter **USB** mode, check Disk Management, then try USB reconnection. Make sure the correct SD card was present at boot. |
| Cannot leave USB mode | Safely eject the drive in Windows (or disconnect the USB host); then use G0. |
| Hotspot joins but webpage will not load | Stay connected despite the no-Internet warning, disable the phone's mobile data temporarily, and enter `http://192.168.4.1/` explicitly. |
| Browser requests an unexpected password | HTTP username/password are **separate** from the hotspot's Wi-Fi password; check `[HTTPD]` or the HTTP status screen. |
| IR remote missing after import | Check `[REMOTE]` / `[REMOTE2]` etc., including a valid `PROTOCOL` and hexadecimal `ADDRESS`, then import the complete SD configuration. |
| Settings do not survive reboot | Choose **Save settings**. A full flash erase or changed NVS partition can remove stored configuration; ordinary firmware uploads should not. |

## Safety and current limitations

- **Back up the SD card.** Rename, delete, editing, HTTP upload and USB MSC are real write operations.
- Do **not** use the SD card from the Cardputer and a USB host simultaneously. Eject before leaving USB mode.
- SD **formatting is not enabled**. Files copy/move, browser-based text editing, and mDNS (`m5knife.local`) are not yet implemented.
- The on-device text viewer/editor is limited to **8 KiB** and may normalise line endings.
- The IR library transmits NEC/Samsung commands; it does **not** learn IR signals.
- BLE scanning does not implement Bluetooth pairing or data connections.
- Network tools are intended for your own equipment and authorised networks.
- The hotspot has no Internet uplink, and its web server is **HTTP, not HTTPS**.
- M5Knife is developed and tested for the **Cardputer Adv**. Different devices, SD cards, phone browsers and USB hosts may behave differently.

## Project layout

- `src/main.cpp` — firmware, splash display, launcher and built-in tools.
- `src/splash.jpg` — 240 × 135 JPEG startup artwork, embedded in firmware flash at build time.
- `src/icons.bin` — nine compressed chrome-and-red launcher icon JPEGs, embedded in firmware flash.
- `platformio.ini` — board/platform configuration, USB build flags and pinned dependencies.
- `README.md` — current user guide.

Changes are normally developed on separate branches, tested on Cardputer hardware and merged into `main` after approval.
