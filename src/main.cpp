#include <Arduino.h>
#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <USB.h>
#include <USBMSC.h>
#include <tusb.h>
#include <WiFi.h>
#include <esp_system.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <cstring>
#include <vector>
#include <algorithm>

// M5Stack Cardputer Adv microSD slot, shared with the original Cardputer.
static constexpr int SD_CS = 12, SD_MOSI = 14, SD_MISO = 39, SD_SCK = 40;
static constexpr uint16_t BG = 0x0841, FG = 0xDFBE, ACCENT = 0x07E0;
static constexpr uint16_t MUTED = 0x8410, SEL = 0x020E, WARN = 0xFD20;

using Keys = Keyboard_Class::KeysState;
enum class Screen { Home, Files, Viewer, Disk, Usb, Input, Confirm, Message, Wifi, Bluetooth, Status, WifiPassword, WifiConnecting };
enum class Action { None, Mkdir, Rename, Delete };
struct Entry { String name; bool dir; uint64_t size; };

static Screen screen = Screen::Home, returnScreen = Screen::Files;
static Action action = Action::None;
static std::vector<Entry> entries;
static String cwd = "/", inputText, message, selectedPath;
static bool sdReady = false;
static size_t selected = 0, scrollPos = 0;
static int homeIndex = 0, diskIndex = 0;
static bool waitingKeyRelease = false;
struct ScanEntry { String label; int rssi; bool secured; };
static std::vector<ScanEntry> scanEntries;
static size_t scanSelection = 0, scanScroll = 0;
static String scanStatus;
static bool bleInitialized = false;
static String wifiTarget, wifiPassword;
static uint32_t wifiConnectStarted = 0;
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 18000;
static void draw();
static void scanWifi() {
  scanEntries.clear(); scanSelection=scanScroll=0;
  screen=Screen::Wifi;
  scanStatus="Scanning WiFi...";
  draw();
  WiFi.mode(WIFI_STA);
  // Scanning must not forcibly disconnect an established connection.
  int count=WiFi.scanNetworks(false,true);
  if (count<0) scanStatus="WiFi scan failed";
  else {
    for (int i=0;i<count && i<64;++i) {
      String name=WiFi.SSID(i);
      if (!name.length()) name="<hidden>";
      scanEntries.push_back({name, WiFi.RSSI(i), WiFi.encryptionType(i)!=WIFI_AUTH_OPEN});
    }
    scanStatus=String(count)+" network(s)";
  }
  WiFi.scanDelete();
  draw();
}
static void scanBluetooth() {
  scanEntries.clear(); scanSelection=scanScroll=0;
  screen=Screen::Bluetooth;
  scanStatus="Scanning BLE...";
  draw();
  if (!bleInitialized) { BLEDevice::init(""); bleInitialized=true; }
  BLEScan* scanner=BLEDevice::getScan();
  scanner->setActiveScan(true);
  BLEScanResults results=scanner->start(4,false);
  const int total=results.getCount();
  for(int i=0;i<total && i<64;++i) {
    BLEAdvertisedDevice d=results.getDevice(i);
    String name=d.haveName()?String(d.getName().c_str()):String(d.getAddress().toString().c_str());
    if (!name.length()) name="Unknown BLE";
    scanEntries.push_back({name,d.getRSSI(),false});
  }
  scanStatus=String(total)+" BLE device(s)";
  scanner->clearResults();
  draw();
}

static USBMSC msc;
static bool usbActive = false;
static bool usbConfigured = false;
static bool usbStarted = false;
static bool usbPhysicallyAttached = false;
static volatile bool usbSuspended = false;
static uint32_t usbSectorCount = 0;
static uint8_t probeSector[512];
// Callback counters are inspected only on the USB screen.
// Use critical sections because USB callbacks run in another task.
static portMUX_TYPE mscStatsMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t mscReads=0, mscWrites=0, mscErrors=0;
static uint32_t mscSlowReads=0, mscSlowWrites=0, mscMaxReadMs=0, mscMaxWriteMs=0;
static void recordMSC(bool write, uint32_t elapsed, bool success) {
  portENTER_CRITICAL(&mscStatsMux);
  if (write) {
    ++mscWrites;
    if (elapsed > mscMaxWriteMs) mscMaxWriteMs=elapsed;
    if (elapsed >= 100) ++mscSlowWrites;
  } else {
    ++mscReads;
    if (elapsed > mscMaxReadMs) mscMaxReadMs=elapsed;
    if (elapsed >= 100) ++mscSlowReads;
  }
  if (!success) ++mscErrors;
  portEXIT_CRITICAL(&mscStatsMux);
}

static volatile bool usbEjected = false, usbConnected = false, usbWriting = false;
static uint32_t sdCheckAt = 0;
static constexpr uint32_t SD_CHECK_INTERVAL = 2500;
// MSC callbacks run in the USB task, never access the FAT filesystem while active.
static int32_t mscRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t size) {
  const uint32_t startMs=millis();
  const uint32_t count=usbSectorCount;
  bool ok=usbActive && count && offset<512 &&
    (uint64_t)lba*512+offset+size <= (uint64_t)count*512;
  if (ok) {
    uint8_t sector[512], *out=(uint8_t*)buffer;
    uint32_t pos=0;
    while (pos<size) {
      uint32_t n=lba+(offset+pos)/512, off=(offset+pos)%512;
      uint32_t bytes=std::min(512U-off,size-pos);
      if (!SD.readRAW(sector,n)) {ok=false;break;}
      memcpy(out+pos,sector+off,bytes);
      pos+=bytes;
    }
  }
  recordMSC(false,millis()-startMs,ok);
  return ok ? (int32_t)size : -1;
}
static int32_t mscWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t size) {
  const uint32_t startMs=millis();
  const uint32_t count=usbSectorCount;
  bool ok=usbActive && count && offset<512 &&
    (uint64_t)lba*512+offset+size <= (uint64_t)count*512;
  if (ok) {
    usbWriting=true;
    uint8_t sector[512];uint32_t pos=0;
    while(pos<size) {
      uint32_t n=lba+(offset+pos)/512,off=(offset+pos)%512;
      uint32_t bytes=std::min(512U-off,size-pos);
      if ((off || bytes!=512) && !SD.readRAW(sector,n)) {ok=false;break;}
      memcpy(sector+off,buffer+pos,bytes);
      if (!SD.writeRAW(sector,n)) {ok=false;break;}
      pos+=bytes;
    }
    usbWriting=false;
  }
  recordMSC(true,millis()-startMs,ok);
  return ok ? (int32_t)size : -1;
}
static bool mscStartStop(uint8_t, bool start, bool eject) {
  if (eject && !start) usbEjected = true;
  if (start) usbEjected = false;
  return true;
}
static void usbEvent(void*, esp_event_base_t base, int32_t event, void*) {
  if (base != ARDUINO_USB_EVENTS) return;
  if (event == ARDUINO_USB_STARTED_EVENT) usbConnected = true;
  if (event == ARDUINO_USB_STOPPED_EVENT) usbConnected = false;
  if (event == ARDUINO_USB_SUSPEND_EVENT) usbSuspended = true;
  if (event == ARDUINO_USB_RESUME_EVENT) usbSuspended = false;
}


static void header(const char* text) {
  auto& d = M5Cardputer.Display;
  d.fillScreen(BG);
  d.fillRect(0, 0, 240, 19, 0x0205);
  d.setTextColor(ACCENT, 0x0205);
  d.setTextSize(1);
  d.drawString("M5KNIFE 0.2.1", 5, 5);
  d.setTextColor(FG, 0x0205);
  d.drawRightString(text, 235, 5);
}
static void footer(const String& text) {
  auto& d = M5Cardputer.Display;
  d.fillRect(0, 120, 240, 15, 0x0205);
  d.setTextColor(MUTED, 0x0205);
  d.drawString(text, 4, 123);
}
static void line(int y, const String& s, bool active=false, uint16_t color=FG) {
  auto& d = M5Cardputer.Display;
  if (active) d.fillRoundRect(3, y-2, 234, 13, 3, SEL);
  d.setTextColor(active ? ACCENT : color, active ? SEL : BG);
  d.drawString(s, 7, y);
}
static String basenameOf(const String& p) {
  int k = p.lastIndexOf('/'); return p.substring(k + 1);
}
static String childPath(const String& name) {
  return cwd == "/" ? "/" + name : cwd + "/" + name;
}
static bool mountSd() {
  // SD.begin() alone is not sufficient to declare the card usable.
  sdReady = false;
  SD.end();
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, SPI, 16000000)) return false;
  if (SD.cardType() == CARD_NONE || SD.cardSize() == 0) {
    SD.end();
    return false;
  }
  File root = SD.open("/");
  const bool usable = root && root.isDirectory();
  if (root) root.close();
  if (!usable) {
    SD.end();
    return false;
  }
  sdReady = true;
  return true;
}
static bool probeSd() {
  if (!sdReady) return mountSd();
  if (SD.cardType()==CARD_NONE || !SD.cardSize() || !SD.readRAW(probeSector, 0)) {
    sdReady=false;
    SD.end();
    return false;
  }
  return true;
}
// Plaintext credentials on SD root. Never touch SD while USB MSC owns it.
static bool wifiConfigSdReady() {
  if (usbActive) return false;
  return sdReady ? probeSd() : mountSd();
}
static String wifiReadLine(File& f) {
  String v=f.readStringUntil('\n');
  if(v.endsWith("\r")) v.remove(v.length()-1);
  return v;
}
static void saveWifiConfig() {
  if(WiFi.status()!=WL_CONNECTED || WiFi.SSID()!=wifiTarget) {
    scanStatus="Connect first, then P";draw();return;
  }
  if(!wifiConfigSdReady()) {scanStatus="SD card unavailable";draw();return;}
  if(wifiTarget.isEmpty() || wifiTarget.length()>32 ||
     wifiTarget.indexOf('\n')>=0 || wifiTarget.indexOf('\r')>=0 ||
     wifiPassword.indexOf('\n')>=0 || wifiPassword.indexOf('\r')>=0) {
    scanStatus="Invalid credentials";draw();return;
  }
  // Replace, never append.
  SD.remove("/wifi.conf");
  File file=SD.open("/wifi.conf",FILE_WRITE);
  if(!file) {scanStatus="Cannot create wifi.conf";draw();return;}
  String data=String("SSID=")+wifiTarget+"\nPASSWORD="+wifiPassword+"\n";
  size_t n=file.print(data);
  file.flush();file.close();
  scanStatus=n==data.length() ? "Saved /wifi.conf (plaintext)" : "WiFi config write failed";
  draw();
}
static void loadWifiConfig() {
  if(!wifiConfigSdReady()) {scanStatus="SD card unavailable";draw();return;}
  File file=SD.open("/wifi.conf",FILE_READ);
  if(!file) {scanStatus="No /wifi.conf on SD";draw();return;}
  String ssid=wifiReadLine(file), pass=wifiReadLine(file);
  file.close();
  if(!ssid.startsWith("SSID=") || !pass.startsWith("PASSWORD=")) {
    scanStatus="Invalid wifi.conf format";draw();return;
  }
  ssid.remove(0,5);pass.remove(0,9);
  if(ssid.isEmpty() || ssid.length()>32 || pass.length()>63) {
    scanStatus="Invalid SSID/password";draw();return;
  }
  wifiTarget=ssid;wifiPassword=pass;
  beginWifiConnection();
}

static void scanDir() {
  entries.clear(); selected=scrollPos=0;
  if (!sdReady) return;
  File folder = SD.open(cwd.c_str());
  if (!folder || !folder.isDirectory()) { if (folder) folder.close(); return; }
  File f = folder.openNextFile();
  while (f && entries.size() < 512) {
    String name = basenameOf(String(f.name()));
    if (name.length()) entries.push_back({name, f.isDirectory(), (uint64_t)f.size()});
    f.close(); f = folder.openNextFile();
  }
  folder.close();
  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    if (a.dir != b.dir) return a.dir > b.dir;
    String x = a.name, y = b.name; x.toLowerCase(); y.toLowerCase(); return x < y;
  });
}
static void drawHome() {
  header(sdReady ? "SD READY" : "NO SD");
  auto& d = M5Cardputer.Display;
  const char* labels[6] = {"FILES", "USB", "WIFI", "BLUETOOTH", "STATUS", "ABOUT"};
  // Three visible tiles. Keep the selection centred where possible.
  const int first = std::min(3, std::max(0, homeIndex - 1));
  for (int slot = 0; slot < 3; ++slot) {
    const int i = first + slot;
    const int x = 8 + slot * 78;
    const int y = 29;
    const bool active = homeIndex == i;
    const uint16_t fill = active ? SEL : 0x1082;
    const uint16_t ink = active ? ACCENT : FG;
    d.fillRoundRect(x, y, 68, 70, 5, fill);
    d.drawRoundRect(x, y, 68, 70, 5, active ? ACCENT : MUTED);
    const int cx = x + 34;
    const int cy = y + 27;
    if (i == 0) { // Folder
      d.fillRoundRect(cx-14,cy-5,28,19,2,0xFFE0);
      d.fillRect(cx-14,cy-9,13,6,0xFFE0);
    } else if (i == 1) { // USB
      d.drawLine(cx,cy-15,cx,cy+14,ink);
      d.drawLine(cx,cy-15,cx-5,cy-10,ink);
      d.drawLine(cx,cy-15,cx+5,cy-10,ink);
      d.drawLine(cx,cy+2,cx-11,cy-5,ink);
      d.fillCircle(cx-11,cy-5,2,ink);
      d.drawLine(cx,cy+9,cx+10,cy+2,ink);
      d.fillRect(cx+8,cy,5,5,ink);
    } else if (i == 2) { // WiFi
      d.drawArc(cx,cy+9,20,17,220,320,ink);
      d.drawArc(cx,cy+9,13,10,220,320,ink);
      d.fillCircle(cx,cy+8,3,ink);
    } else if (i == 3) { // Bluetooth
      d.drawLine(cx,cy-15,cx,cy+15,ink);
      d.drawLine(cx,cy-15,cx+10,cy-7,ink);
      d.drawLine(cx+10,cy-7,cx-8,cy+8,ink);
      d.drawLine(cx-8,cy-8,cx+10,cy+7,ink);
      d.drawLine(cx+10,cy+7,cx,cy+15,ink);
    } else if (i == 4) { // Status: gauge
      d.drawCircle(cx,cy,13,ink);
      d.drawLine(cx,cy,cx+7,cy-7,ink);
      d.fillCircle(cx,cy,2,ink);
    } else { // About: information symbol
      d.drawCircle(cx,cy,13,ink);
      d.fillCircle(cx,cy-7,2,ink);
      d.drawLine(cx,cy-2,cx,cy+9,ink);
    }
    d.setTextColor(ink, fill);
    d.drawCenterString(labels[i], cx, y+53);
  }
  d.setTextColor(MUTED, BG);
  d.drawCenterString(String(homeIndex+1) + " / 6", 120, 104);
  footer("A/D or arrows: scroll  Enter  G0");
}
static void drawFiles() {
  header("FILES");
  line(23, cwd.length()>38 ? "..."+cwd.substring(cwd.length()-35) : cwd, false, ACCENT);
  if (!sdReady) { line(54, "SD not mounted", false, WARN); footer("G0 menu  R retry"); return; }
  if (entries.empty()) line(51, "(empty directory)", false, MUTED);
  for (int r=0; r<6; ++r) {
    size_t i = scrollPos + r; if (i>=entries.size()) break;
    const auto& e=entries[i];
    String label=String(e.dir ? "> " : "  ")+e.name;
    if (label.length()>32) label=label.substring(0,29)+"...";
    line(39+r*13, label, selected==i);
  }
  footer("Fn+;,./ arrows  B back  M disk");
}
static void drawWifiPassword() {
  header("WIFI PASSWORD");
  line(23, "Network:", false, ACCENT);
  line(38, wifiTarget.substring(0,34));
  line(57, "Password:", false, ACCENT);
  // Deliberately show entered password characters, as requested.
  line(72, wifiPassword.substring(wifiPassword.length()>34?wifiPassword.length()-34:0));
  footer("Enter connect  Fn+` cancel");
}
static void drawWifiConnecting() {
  header("WIFI CONNECT");
  line(32, wifiTarget.substring(0,34), false, ACCENT);
  line(56, "Connecting...");
  footer("G0 cancel and return home");
}
static void beginWifiConnection() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiTarget.c_str(),wifiPassword.c_str());
  // Hold credentials in RAM until explicitly saved or replaced.
  wifiConnectStarted=millis();
  screen=Screen::WifiConnecting;
  drawWifiConnecting();
}
static void showWifiConnectionResult(bool connected) {
  screen=Screen::Wifi;
  if (!connected) wifiPassword="";
  scanStatus=connected ? String("Connected: ")+WiFi.localIP().toString() : "Connection failed / timed out";
  draw();
}

static void drawScan(bool wifi) {
  header(wifi ? "WIFI SCAN" : "BLUETOOTH BLE");
  if (wifi && WiFi.status()==WL_CONNECTED)
    line(21, String("Connected: ")+WiFi.SSID().substring(0,18),false,ACCENT);
  else line(21,scanStatus,false,ACCENT);
  if(scanEntries.empty()) line(55,"No devices found",false,MUTED);
  for (int row=0;row<6;++row) {
    size_t n=scanScroll+row;
    if(n>=scanEntries.size()) break;
    const auto& e=scanEntries[n];
    String label=String(e.secured?"* ":"  ")+e.label;
    if (label.length()>23) label=label.substring(0,20)+"...";
    label+=" "+String(e.rssi)+"dBm";
    line(36+row*13,label,n==scanSelection);
  }
  footer(wifi ? "Enter join P save L load D off" : "W/S scroll  R rescan  B back");
}
static String hardwareAddress(esp_mac_type_t type) {
  uint8_t mac[6] = {0};
  if (esp_read_mac(mac, type) != ESP_OK) return "Unavailable";
  char result[18];
  snprintf(result, sizeof(result), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  return String(result);
}
static void drawStatus() {
  header("STATUS");
  const bool connected = WiFi.status() == WL_CONNECTED;
  line(23, "WiFi network", false, ACCENT);
  String ssid = connected ? WiFi.SSID() : "Not connected";
  if (ssid.length()>34) ssid=ssid.substring(0,31)+"...";
  line(36, ssid);
  line(51, "IP address", false, ACCENT);
  line(64, connected ? WiFi.localIP().toString() : "Not connected");
  line(79, "WiFi MAC", false, ACCENT);
  line(91, hardwareAddress(ESP_MAC_WIFI_STA));
  line(104, String("BT MAC: ")+hardwareAddress(ESP_MAC_BT));
  footer("R refresh  B back  G0 menu");
}
static void drawDisk() {
  header("FILES / DISK");
  line(29, "SD card operations", false, ACCENT);
  line(51, "Card details", diskIndex==0);
  line(66, "Format SD card", diskIndex==1);
  footer("Up/Down  Enter  G0 home");
}
static void drawUsb() {
  header("USB TEST");
  if (!usbActive) {
    line(28,"SD media not active",false,WARN);
    footer("G0 main menu");
    return;
  }
  uint32_t reads,writes,errors,slowR,slowW,maxR,maxW;
  portENTER_CRITICAL(&mscStatsMux);
  reads=mscReads; writes=mscWrites; errors=mscErrors;
  slowR=mscSlowReads;slowW=mscSlowWrites;
  maxR=mscMaxReadMs;maxW=mscMaxWriteMs;
  portEXIT_CRITICAL(&mscStatsMux);
  line(22,String("USB: ")+(usbPhysicallyAttached?"ATTACHED":"DETACHED"),false,ACCENT);
  line(36,String("Reads: ")+String(reads)+"  Writes: "+String(writes));
  line(50,String("Errors: ")+String(errors),false,errors?WARN:FG);
  line(64,String("Slow R/W: ")+String(slowR)+"/"+String(slowW));
  line(78,String("Max R: ")+String(maxR)+"ms");
  line(92,String("Max W: ")+String(maxW)+"ms");
  line(106,usbEjected?"Ejected":"Eject before G0",false,WARN);
  footer("R refresh   G0 after eject");
}
static void draw();
static void stopUsb() {
  // USB stays enumerated. The host must eject the drive before G0 can
  // withdraw its media; on re-entry Windows sees fresh media.
  // Remove the whole USB device before withdrawing the media.
  tud_disconnect();
  usbPhysicallyAttached = false;
  delay(80); // Allow USB task to service the detach before unmount.
  msc.mediaPresent(false);
  usbActive = false;
  usbEjected = false;
  SD.end();
  sdReady = false;
  sdCheckAt = millis() + SD_CHECK_INTERVAL;
}
static void startUsb() {
  if (!mountSd()) {screen=Screen::Usb;draw();return;}
  if (SD.sectorSize()!=512 || SD.numSectors()==0) {screen=Screen::Usb;draw();return;}
  if (!usbConfigured || !usbStarted) {screen=Screen::Usb;draw();return;}
  usbSectorCount=SD.numSectors();
  portENTER_CRITICAL(&mscStatsMux);
  mscReads=mscWrites=mscErrors=0;
  mscSlowReads=mscSlowWrites=mscMaxReadMs=mscMaxWriteMs=0;
  portEXIT_CRITICAL(&mscStatsMux);
  usbEjected=false;
  // Advertise ready media BEFORE the host can enumerate the drive.
  msc.mediaPresent(true);
  usbActive=true;
  tud_connect();
  usbPhysicallyAttached=true;
  screen=Screen::Usb;draw();
}
static void drawInput() {
  header(action==Action::Mkdir?"NEW FOLDER":"RENAME");
  line(32, action==Action::Mkdir?"Folder name:":"New name:", false, ACCENT);
  line(54, inputText.length()>33 ? inputText.substring(inputText.length()-33) : inputText);
  footer("Type  Enter OK  Fn+` cancel");
}
static void drawConfirm() {
  header("CONFIRM DELETE");
  line(35, "Delete selected file/empty dir?", false, WARN);
  line(57, basenameOf(selectedPath).substring(0,32));
  footer("Y delete   N cancel   G0 home");
}
static void drawMessage() {
  header("INFORMATION");
  int y=30, from=0;
  while (from < (int)message.length() && y<=108) {
    int end=message.indexOf('\n',from);
    if (end<0) end=message.length();
    line(y,message.substring(from,end)); y+=15; from=end+1;
  }
  footer("Enter / G0 home");
}
static void drawViewer() {
  header("VIEW TEXT");
  File file=SD.open(selectedPath.c_str(),FILE_READ);
  if (!file) { line(40,"Cannot open file",false,WARN); footer("B back  G0 home"); return; }
  String row; int y=25; int chars=0;
  while (file.available() && y<=105 && chars<2000) {
    int c=file.read(); ++chars;
    if (c=='\r') continue;
    if (c=='\n' || row.length()>=37) {
      line(y,row); y+=12; row="";
    }
    if (c>=32 && c<=126) row+=(char)c;
  }
  if (row.length() && y<=105) line(y,row);
  file.close(); footer("Preview only  B back  G0 home");
}
static void draw() {
  switch(screen) {
    case Screen::Home: drawHome(); break;
    case Screen::Files: drawFiles(); break;
    case Screen::Viewer: drawViewer(); break;
    case Screen::Disk: drawDisk(); break;
    case Screen::Usb: drawUsb(); break;
    case Screen::Input: drawInput(); break;
    case Screen::Confirm: drawConfirm(); break;
    case Screen::Message: drawMessage(); break;
    case Screen::Wifi: drawScan(true); break;
    case Screen::Bluetooth: drawScan(false); break;
    case Screen::Status: drawStatus(); break;
    case Screen::WifiPassword: drawWifiPassword(); break;
    case Screen::WifiConnecting: drawWifiConnecting(); break;
  }
}
static void goHome() { if(screen==Screen::WifiConnecting) WiFi.disconnect(false,false); wifiPassword=""; screen=Screen::Home; action=Action::None; draw(); }
static void openFiles() { if (!sdReady) mountSd(); cwd="/"; scanDir(); screen=Screen::Files; draw(); }
static void showMessage(const String& s, Screen back=Screen::Files) {
  message=s; returnScreen=back; screen=Screen::Message; draw();
}
static bool hasInvalidName(const String& s) {
  if (!s.length() || s=="." || s==".." || s.length()>100) return true;
  for (auto c:s) if (c=='/' || c=='\\' || c==':' || c=='*' || c=='?' || c=='"' || c=='<' || c=='>' || c=='|') return true;
  return false;
}
static void completeInput() {
  if (hasInvalidName(inputText)) { showMessage("Invalid name."); return; }
  bool ok=false;
  String to=childPath(inputText);
  if (SD.exists(to)) { showMessage("Name already exists."); return; }
  if (action==Action::Mkdir) ok=SD.mkdir(to.c_str());
  if (action==Action::Rename) ok=SD.rename(selectedPath.c_str(),to.c_str());
  action=Action::None; scanDir(); screen=Screen::Files; draw();
  if (!ok) showMessage("SD operation failed.");
}
static void activateFile() {
  if (selected>=entries.size()) return;
  const auto& e=entries[selected];
  selectedPath=childPath(e.name);
  if (e.dir) { cwd=selectedPath; scanDir(); draw(); }
  else { screen=Screen::Viewer; draw(); }
}
static void backFile() {
  if (cwd=="/") {goHome();return;}
  int slash=cwd.lastIndexOf('/'); cwd = slash<=0 ? "/" : cwd.substring(0,slash);
  scanDir(); draw();
}
static bool typed(const Keys& k, char c) {
  for (char v:k.word) if (v==c || v==char(c-32)) return true;
  return false;
}
static void handleKeys(const Keys& k) {
  // v1.1.1 KeysState has no arrow/esc/backspace members.
  // Fn+; , . / supplies the cursor directions; Fn+` is Escape.
  const bool up=(k.fn && typed(k,';')) || (!k.fn && typed(k,'w'));
  const bool down=(k.fn && typed(k,'.')) || (!k.fn && typed(k,'s'));
  const bool left=(k.fn && typed(k,',')) || (!k.fn && typed(k,'a'));
  const bool right=(k.fn && typed(k,'/')) || (!k.fn && typed(k,'d'));
  const bool escape=k.fn && typed(k,'`');
  const bool backspace=k.del && !k.fn;
  if(screen==Screen::Home) {
    if(left && homeIndex > 0) --homeIndex;
    if(right && homeIndex < 5) ++homeIndex;
    if(k.enter) {
      // Explicit dispatch for all six launcher tiles.
      switch (homeIndex) {
        case 0: openFiles(); break;
        case 1: startUsb(); break;
        case 2: scanWifi(); break;
        case 3: scanBluetooth(); break;
        case 4:
          screen = Screen::Status;
          drawStatus();
          break;
        case 5: break; // About: placeholder
      }
      return;
    }
  } else if(screen==Screen::WifiPassword) {
    if (escape) {wifiPassword="";screen=Screen::Wifi;draw();return;}
    if (k.enter) {beginWifiConnection();return;}
    if (k.del) {if(wifiPassword.length()) wifiPassword.remove(wifiPassword.length()-1);}
    else for(char c:k.word) if(c>=32 && c<=126 && wifiPassword.length()<63) wifiPassword+=c;
    draw(); return;
  } else if(screen==Screen::WifiConnecting) {
    // The connection attempt is asynchronous; G0 is handled centrally.
    return;
  } else if(screen==Screen::Wifi || screen==Screen::Bluetooth) {
    if (escape||backspace||typed(k,'b')) {goHome();return;}
    if (screen==Screen::Wifi && typed(k,'p')) {saveWifiConfig();return;}
    if (screen==Screen::Wifi && typed(k,'l')) {loadWifiConfig();return;}
    if (screen==Screen::Wifi && typed(k,'d')) {
      WiFi.disconnect(false,false);
      wifiPassword="";
      scanStatus="Disconnected";
      draw();return;
    }
    if (typed(k,'r')) {if(screen==Screen::Wifi) scanWifi(); else scanBluetooth();return;}
    if (screen==Screen::Wifi && k.enter && scanSelection<scanEntries.size()) {
      const auto& network=scanEntries[scanSelection];
      if (network.label=="<hidden>") {scanStatus="Hidden SSID unsupported";draw();return;}
      wifiTarget=network.label;
      wifiPassword="";
      if (network.secured) {screen=Screen::WifiPassword;draw();return;}
      beginWifiConnection();return;
    }
    if (up && scanSelection>0) --scanSelection;
    if (down && scanSelection+1<scanEntries.size()) ++scanSelection;
    if (scanSelection<scanScroll) scanScroll=scanSelection;
    if (scanSelection>=scanScroll+6) scanScroll=scanSelection-5;
  } else if(screen==Screen::Usb) {
    if (typed(k,'r')) {drawUsb();return;}
    return;
  } else if(screen==Screen::Status) {
    if(escape||backspace||typed(k,'b')) {goHome();return;}
    if(typed(k,'r')) {drawStatus();return;}
    return;
  } else if(screen==Screen::Files) {
    if (up && selected>0) --selected;
    if (down && selected+1<entries.size()) ++selected;
    if(selected<scrollPos) scrollPos=selected;
    if(selected>=scrollPos+6) scrollPos=selected-5;
    if(k.enter) {activateFile();return;}
    if(escape||backspace||typed(k,'b')) {backFile();return;}
    if(typed(k,'r')) { if (!sdReady) mountSd(); else probeSd(); scanDir(); }
    if(typed(k,'m')) {diskIndex=0;screen=Screen::Disk;draw();return;}
    if(typed(k,'n') && sdReady) {action=Action::Mkdir;inputText="";screen=Screen::Input;draw();return;}
    if(typed(k,'e') && selected<entries.size()) {action=Action::Rename;selectedPath=childPath(entries[selected].name);inputText=entries[selected].name;screen=Screen::Input;draw();return;}
    if(typed(k,'x') && selected<entries.size()) {action=Action::Delete;selectedPath=childPath(entries[selected].name);screen=Screen::Confirm;draw();return;}
  } else if(screen==Screen::Viewer) {
    if(escape||backspace||typed(k,'b')||k.enter) {screen=Screen::Files;draw();return;}
  } else if(screen==Screen::Disk) {
    if(up||down) diskIndex=1-diskIndex;
    if(k.enter) {
      if(diskIndex==0) {
        if(!sdReady) showMessage("No mounted SD card.",Screen::Disk);
        else showMessage("Capacity: "+String((uint32_t)(SD.cardSize()/1048576))+" MiB\nUsed: "+String((uint32_t)(SD.usedBytes()/1048576))+" MiB",Screen::Disk);
      } else showMessage("Formatting NOT enabled.\nThis needs a tested FAT32\nformatter with safeguards.",Screen::Disk);
      return;
    }
    if(escape||backspace) {screen=Screen::Files;draw();return;}
  } else if(screen==Screen::Input) {
    if(escape) {action=Action::None;screen=Screen::Files;draw();return;}
    if(k.enter) {completeInput();return;}
    if(k.del) {if(inputText.length()) inputText.remove(inputText.length()-1);}
    else if(!k.fn) for(char c:k.word) if(c>=32 && c<=126 && inputText.length()<100) inputText+=c;
  } else if(screen==Screen::Confirm) {
    if(typed(k,'y')) {
      bool ok=false;
      File f=SD.open(selectedPath.c_str()); bool isDir=f && f.isDirectory(); if(f)f.close();
      ok=isDir ? SD.rmdir(selectedPath.c_str()) : SD.remove(selectedPath.c_str());
      scanDir();screen=Screen::Files;draw();
      if(!ok) showMessage("Delete failed.\nDirectory must be empty.");
      return;
    }
    if(typed(k,'n')||escape||backspace) {screen=Screen::Files;draw();return;}
  } else if(screen==Screen::Message) {
    if(k.enter||escape||backspace) {screen=returnScreen;draw();return;}
  }
  draw();
}
void setup() {
  auto cfg=M5.config();
  M5Cardputer.begin(cfg,true);
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setFont(&fonts::Font0);
  M5Cardputer.Display.setTextSize(1);
  Serial.begin(115200);
  sdReady=mountSd();
  sdCheckAt=millis()+SD_CHECK_INTERVAL;
  USB.onEvent(usbEvent);
  // Register MSC and begin native USB at boot so an already-connected
  // Windows PC can enumerate the device. Present media only in USB mode.
  msc.vendorID("M5STACK");
  msc.productID("CARDPUTER SD");
  msc.productRevision("1.0");
  msc.onRead(mscRead);
  msc.onWrite(mscWrite);
  msc.onStartStop(mscStartStop);
  msc.mediaPresent(false);
  const uint32_t sectors = sdReady ? SD.numSectors() : 0;
  // Report a large enough logical unit; actual limits enforced in callbacks.
  usbSectorCount = sectors;
  usbConfigured = msc.begin(sectors ? sectors : 1048576, 512);
  if (usbConfigured) {
    usbStarted = USB.begin();
    if (usbStarted) {
      // Keep the device electrically detached until the SD card is
      // ready and the user explicitly enters USB mode.
      tud_disconnect();
      usbPhysicallyAttached=false;
    }
  }
  draw();
}
void loop() {
  M5Cardputer.update();
  // G0 is globally handled BEFORE all tool-specific keyboard processing.
  if(M5Cardputer.BtnA.wasPressed()) {
    if (usbActive) {
      if ((!usbConnected || usbEjected) && !usbWriting) {stopUsb();goHome();}
      else drawUsb();
    } else goHome();
    waitingKeyRelease=true;
  }
  if(M5Cardputer.BtnA.wasReleased()) waitingKeyRelease=false;
  if(waitingKeyRelease) {delay(5);return;}
  if(M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed())
    handleKeys(M5Cardputer.Keyboard.keysState());
  if(screen==Screen::WifiConnecting) {
    wl_status_t state=WiFi.status();
    if(state==WL_CONNECTED) showWifiConnectionResult(true);
    else if((uint32_t)(millis()-wifiConnectStarted)>=WIFI_CONNECT_TIMEOUT_MS) {
      WiFi.disconnect(false,false);
      showWifiConnectionResult(false);
    }
  }
  // Poll only on the launcher; never touch filesystem while host owns the SD.
  if (!usbActive && screen==Screen::Home && (int32_t)(millis()-sdCheckAt)>=0) {
    bool was=sdReady;
    probeSd();
    sdCheckAt=millis()+SD_CHECK_INTERVAL;
    if (was!=sdReady) draw();
  }
  delay(10);
}
