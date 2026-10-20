# M5Knife hardware regression tests

This is the manual acceptance checklist for the **M5Stack Cardputer Adv**.
It complements the GitHub Actions build: a successful build does **not**
prove SD, USB, wireless, IR, keypad or display behaviour.

## Before starting

Record the firmware version (About screen), Git commit, tester/date,
Cardputer model, microSD make/capacity/filesystem, Windows version and COM port.
Use a **separate, backed-up FAT32 test card** and devices/networks you own.
Never remove an SD card during a write or intentionally interrupt a host write.
Use the same firmware build for the complete regression.

Mark every applicable case **PASS / FAIL / NOT RUN / N/A** and record details
for failures. A release candidate does not pass while any required case is
FAIL or NOT RUN. If a capability needs extra equipment, mark N/A explicitly;
do not silently count it as passed.

| ID | Test | Expected result | Result |
| --- | --- | --- | --- |
| START-01 | Power on without SD, then with a FAT32 test card | Flash-embedded 240x135 splash appears and launcher opens; SD icon reflects card | NOT RUN |
| UI-01 | Move left/right through all nine launcher tiles with arrows and A/D | All labels/icons appear; red selection and 1/9–9/9 indicators update cleanly | NOT RUN |
| UI-02 | Enter and leave each of Files, USB, WiFi, Network, Bluetooth, Infrared, Settings, Status and About | No hang/reboot; G0 returns home except documented USB/editor safeguards | NOT RUN |
| UI-03 | Toggle Fn+S, reboot | Key beep setting persists; Fn+S in active editor saves instead | NOT RUN |
| SD-01 | Insert and remove SD while on Home and Files (not during I/O) | Mount indicator updates; missing card is reported without false empty folder | NOT RUN |
| SD-02 | Browse nested folders; create, rename and delete a test file/folder | Directory listing and data persist; delete requires confirmation | NOT RUN |
| SD-03 | Attempt unsupported SD format option | Firmware clearly states formatting is disabled; no data is changed | NOT RUN |
| EDIT-01 | Open a small text file, modify, exit with Fn+backtick or G0 | Save / Discard / Back prompt appears; G0 does not bypass it | NOT RUN |
| EDIT-02 | Save edited test file and reopen | Contents match saved text (note CRLF may become LF) | NOT RUN |
| EDIT-03 | Modify text, discard; reopen | Original file remains unchanged | NOT RUN |
| EDIT-04 | Open a file over 8 KiB | Clean error without crash or modifying the file | NOT RUN |
| USB-01 | Enter USB with test card; transfer files from Windows | Host sees card; files are readable and transferred content matches | NOT RUN |
| USB-02 | Try G0 while host has not ejected; then safely eject and press G0 | Unsafe exit blocked; safe exit returns launcher; Cardputer remounts card | NOT RUN |
| USB-03 | Re-enter USB after ejection; repeat transfer | Host sees card again; no new MSC errors; no damaged test files | NOT RUN |
| WIFI-01 | Scan WiFi; connect with correct and incorrect credentials | Network list and connection/failure messages are correct | NOT RUN |
| WIFI-02 | Save known WiFi in NVS, reboot with Auto WiFi enabled | Saved network reconnects without SD | NOT RUN |
| NET-01 | DNS resolution, three pings and one allowed TCP port scan | Results correspond to target; failure is reported, not frozen | NOT RUN |
| NET-02 | Start up to 256-address LAN scan on own network; press B then G0 on a second run | B stops and displays partial results; G0 returns home promptly | NOT RUN |
| BLE-01 | Scan BLE advertisements twice | Lists nearby devices with no hang; scanner can be revisited | NOT RUN |
| IR-01 | With a compatible receiver/TV, transmit a stored NEC or Samsung profile | Expected remote action occurs; saved profile survives reboot | NOT RUN |
| HTTP-01 | Start HTTP server on trusted WiFi and hotspot, check login | Unique NVS passwords accepted; old public defaults rejected; server stops on G0 | NOT RUN |
| HTTP-02 | Upload/download, create/rename/delete on test card | Correct files; existing upload names do not overwrite silently | NOT RUN |
| HTTP-03 | Remove test card only after HTTP operations have completed | Server stops and reports SD unavailable; safe subsequent remount | NOT RUN |
| CFG-01 | Export configuration to SD, reboot, import a valid copy | Settings, saved WiFi and IR library remain usable | NOT RUN |
| CFG-02 | Attempt import with missing section, bad range or oversized value | Rejection with explanation; previous NVS settings remain intact | NOT RUN |
| CFG-03 | Reboot again after HTTP/hotspot migration | New passwords persist; custom valid passwords remain unchanged | NOT RUN |
| STATUS-01 | Inspect Status and About, scan GitHub QR | Network/SD/heap/battery indicators sensible; repo QR opens expected URL | NOT RUN |
| STRESS-01 | Repeat opening/closing all nine tools (at least 10 rounds) | No crashes or progressive failure; record lowest observed heap | NOT RUN |
| STRESS-02 | Browse a large SD folder (up to 512 entries) and HTTP folder (up to 256 entries) | No corruption or crash; record UI performance and free heap | NOT RUN |
| STRESS-03 | Run with WiFi connected for at least 30 minutes, navigate and rescan | No watchdog reset, unexpected server exposure or persistent heap decline | NOT RUN |

## Failure report template

- **Firmware / commit:** 
- **Test ID and observed result:** 
- **Expected result:** 
- **Reproduction steps:** 
- **Hardware, SD card, host / network:** 
- **Serial log or photo (redact credentials):** 
- **Workaround or severity:** 
- **Retest date / outcome:** 

## Sign-off

- Automated PlatformIO build: **NOT VERIFIED**
- Hardware test date / tester: **NOT RUN**
- Remaining blocking failures: **UNKNOWN**
- Approved for 0.9.0 release candidate: **NO — pending tests**

Do not mark this document as passed merely because the build succeeds.
