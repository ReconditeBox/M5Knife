#include <Arduino.h>
#define DISABLE_CODE_FOR_RECEIVER
#define SEND_PWM_BY_TIMER
#define IR_TX_PIN 44
#include <IRremote.hpp>
#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <USB.h>
#include <USBMSC.h>
#include <tusb.h>
#include <WiFi.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <lwip/netdb.h>
#include <unistd.h>
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
enum class Screen { Home, Files, Viewer, Disk, Usb, Input, Confirm, Message, Wifi, Network, NetInfo, NetInput, NetResult, Bluetooth, Infrared, IrManual, IrEdit, IrRemote, Status, WifiPassword, WifiConnecting, About };
enum class Action { None, Mkdir, Rename, Delete };
struct Entry { String name; bool dir; uint64_t size; };

static Screen screen = Screen::Home, returnScreen = Screen::Files;
static Action action = Action::None;
static std::vector<Entry> entries;
static String cwd = "/", inputText, message, selectedPath;
static bool sdReady = false;
static size_t selected = 0, scrollPos = 0;
static int homeIndex = 0, diskIndex = 0;
static int statusScroll=0;
static int aboutScroll=0;
static bool waitingKeyRelease = false;
struct ScanEntry { String label; int rssi; bool secured; };
static std::vector<ScanEntry> scanEntries;
static size_t scanSelection = 0, scanScroll = 0;
static String scanStatus;
static bool bleInitialized = false;
static int netMenuIndex=0;
static bool netPingMode=false;
static int netTool=0;
static size_t netResultScroll=0;
static String netTarget, netOutput;
static String netDefaultTarget="8.8.8.8";
static void drawNetworkMenu();
static void drawNetworkInfo();
static void drawNetworkInput();
static void drawNetworkResult();
static void runNetworkQuery();

static int irMenuIndex=0, irEditField=0;
static uint16_t irAddress=0;
static uint8_t irCommand=0;
static String irEditText, irNotice;
static String irName="No IR profile";
static uint16_t irProfileAddress=0;
static bool irProfileLoaded=false;
static bool irProfileSamsung=false;
static int irRemoteSelection=0;
static const char* const irButtonNames[]={"POWER","VOLUP","VOLDOWN","CHUP","CHDOWN","MUTE"};
static uint8_t irButtonCommands[6]={};
static bool irButtonPresent[6]={};
static bool parseHexValue(String value,uint32_t& out,uint32_t limit) {
  value.trim();
  if(value.startsWith("0x") || value.startsWith("0X")) value.remove(0,2);
  if(!value.length() || value.length()>8) return false;
  uint32_t v=0;
  for(char c:value) {
    int n=(c>='0'&&c<='9')?c-'0':(c>='a'&&c<='f')?c-'a'+10:(c>='A'&&c<='F')?c-'A'+10:-1;
    if(n<0) return false;
    v=(v<<4)|n;
  }
  if(v>limit) return false;
  out=v; return true;
}
static void irSend(uint16_t address,uint8_t command,bool samsung=false) {
  if(samsung) {
    // Profile uses conventional MSB-first Samsung32 hex codes such as
    // POWER E0E040BF => ADDRESS=E0E0, POWER=40.
    uint32_t raw=((uint32_t)address<<16)|((uint32_t)command<<8)|(uint8_t)~command;
    IrSender.sendSamsungMSB(raw,32);
  } else {
    IrSender.sendNEC(address,command,0);
  }
  irNotice=String("Sent ")+(samsung?"Samsung":"NEC")+" 0x"+String(address,HEX)+"/0x"+String(command,HEX);
}

static String wifiTarget, wifiPassword;
static uint32_t wifiConnectStarted = 0;
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 18000;
static void draw();
static void beginWifiConnection(); // Used by SD WiFi config loader.

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
  d.drawString("M5KNIFE 0.4.5", 5, 5);
  d.setTextColor(FG, 0x0205);
  // Reserve the top-right of every screen for compact status icons.
  d.drawRightString(text, 185, 5);
  if (sdReady) {
    // MicroSD card silhouette, 12x12.
    const int x=196, y=3;
    d.drawLine(x+3,y,x+11,y,MUTED);
    d.drawLine(x+11,y,x+11,y+12,MUTED);
    d.drawLine(x+11,y+12,x,y+12,MUTED);
    d.drawLine(x,y+12,x,y+3,MUTED);
    d.drawLine(x,y+3,x+3,y,MUTED);
    d.fillRect(x+3,y+5,6,4,ACCENT);
  }
  if (WiFi.status()==WL_CONNECTED) {
    // WiFi arcs and dot: green when connected, hidden otherwise.
    const int x=224, y=12;
    d.drawArc(x,y,9,8,220,320,ACCENT);
    d.drawArc(x,y,6,5,220,320,ACCENT);
    d.fillCircle(x,y,2,ACCENT);
  }
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
// /WIFI.CFG uses SSID1/PASS1, SSID2/PASS2, etc.
// Preserve spaces and punctuation in values; no trimming or comment parsing.
struct WifiRecord { String ssid, pass; };
static constexpr size_t MAX_WIFI_RECORDS=16;
static bool wifiConfigSdReady() {
  if (usbActive) return false;
  return sdReady ? probeSd() : mountSd();
}
static bool readWifiRecords(std::vector<WifiRecord>& records) {
  records.clear();
  File f=SD.open("/WIFI.CFG",FILE_READ);
  if(!f) return false;
  String names[MAX_WIFI_RECORDS], passwords[MAX_WIFI_RECORDS];
  bool hasName[MAX_WIFI_RECORDS]={}, hasPassword[MAX_WIFI_RECORDS]={};
  while(f.available()) {
    String v=f.readStringUntil('\n');
    if(v.endsWith("\r")) v.remove(v.length()-1);
    int eq=v.indexOf('=');
    if(eq<0) continue;
    String key=v.substring(0,eq), value=v.substring(eq+1);
    bool isSsid=key.startsWith("SSID"), isPass=key.startsWith("PASS");
    if(!isSsid && !isPass) continue;
    String number=key.substring(isSsid?4:4);
    if(!number.length()) continue;
    bool digits=true;
    for(size_t j=0;j<number.length();++j)
      if(number[j]<'0'||number[j]>'9') {digits=false;break;}
    if(!digits) continue;
    int slot=number.toInt();
    if(slot<1 || slot>(int)MAX_WIFI_RECORDS) continue;
    if(isSsid) {names[slot-1]=value;hasName[slot-1]=true;}
    else {passwords[slot-1]=value;hasPassword[slot-1]=true;}
  }
  f.close();
  for(size_t i=0;i<MAX_WIFI_RECORDS;++i)
    if(hasName[i]&&hasPassword[i]&&names[i].length()>0&&names[i].length()<=32&&passwords[i].length()<=63)
      records.push_back({names[i],passwords[i]});
  return true;
}
static bool writeWifiRecords(const std::vector<WifiRecord>& records) {
  // Build the new file before removing the original.
  SD.remove("/wifi.tmp");
  File f=SD.open("/wifi.tmp",FILE_WRITE);
  if(!f) return false;
  bool ok=true;
  for(size_t i=0;i<records.size();++i) {
    String pair=String("SSID")+String(i+1)+"="+records[i].ssid+"\n"+
      "PASS"+String(i+1)+"="+records[i].pass+"\n\n";
    if(f.print(pair)!=pair.length()) {ok=false;break;}
  }
  f.flush();f.close();
  if(!ok) {SD.remove("/wifi.tmp");return false;}
  // FAT rename will not overwrite an existing destination.
  if(SD.exists("/WIFI.CFG") && !SD.remove("/WIFI.CFG")) return false;
  return SD.rename("/wifi.tmp","/WIFI.CFG");
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
  std::vector<WifiRecord> records;
  if(SD.exists("/WIFI.CFG") && !readWifiRecords(records)) {
    scanStatus="Cannot read WIFI.CFG";draw();return;
  }
  bool found=false;
  for(auto& item:records)
    if(item.ssid==wifiTarget) {item.pass=wifiPassword;found=true;break;}
  if(!found) {
    if(records.size()>=MAX_WIFI_RECORDS) {scanStatus="WiFi config full";draw();return;}
    records.push_back({wifiTarget,wifiPassword});
  }
  scanStatus=writeWifiRecords(records) ? "Saved /WIFI.CFG" : "Failed saving WIFI.CFG";
  draw();
}
static void loadWifiConfig() {
  if(!wifiConfigSdReady()) {scanStatus="SD card unavailable";draw();return;}
  std::vector<WifiRecord> records;
  if(!readWifiRecords(records)) {scanStatus="No /WIFI.CFG on SD";draw();return;}
  if(records.empty()) {scanStatus="No valid saved networks";draw();return;}
  // L connects to the highlighted network if saved; otherwise report it.
  if(scanSelection>=scanEntries.size()) {scanStatus="Select a network first";draw();return;}
  const String target=scanEntries[scanSelection].label;
  for(const auto& item:records) {
    if(item.ssid==target) {
      wifiTarget=item.ssid;
      wifiPassword=item.pass;
      beginWifiConnection();
      return;
    }
  }
  scanStatus="Selected WiFi not in WIFI.CFG";draw();
}

static bool irLoadProfile() {
  if(usbActive || (!sdReady && !mountSd())) {irNotice="SD not available";return false;}
  File f=SD.open("/IR.CFG",FILE_READ);
  if(!f) {irNotice="No /IR.CFG on SD";return false;}
  String name="";uint32_t address=0;bool addressOk=false;
  uint8_t commands[6]={};bool found[6]={};
  bool inRemote=false,nec=false,samsung=false;
  while(f.available()) {
    String entry=f.readStringUntil('\n'); entry.trim();
    if(entry=="[REMOTE]") {inRemote=true;continue;}
    if(!inRemote || entry.isEmpty() || entry.startsWith("#") || entry.startsWith(";")) continue;
    int equals=entry.indexOf('='); if(equals<0)continue;
    String key=entry.substring(0,equals),value=entry.substring(equals+1);
    key.trim();value.trim();
    if(key=="NAME")name=value;
    else if(key=="PROTOCOL") {nec=value.equalsIgnoreCase("NEC");samsung=value.equalsIgnoreCase("SAMSUNG");}
    else if(key=="ADDRESS")addressOk=parseHexValue(value,address,65535);
    else for(int i=0;i<6;i++)if(key==irButtonNames[i]){
      uint32_t v=0;
      if(parseHexValue(value,v,255)){commands[i]=(uint8_t)v;found[i]=true;}
    }
  }
  f.close();
  if((!nec && !samsung) || !addressOk){irNotice="IR.CFG: protocol/address?";return false;}
  irName=name.length()?name:"IR remote";
  irProfileAddress=(uint16_t)address;
  irProfileSamsung=samsung;
  for(int i=0;i<6;i++){irButtonCommands[i]=commands[i];irButtonPresent[i]=found[i];}
  irProfileLoaded=true; irNotice="Loaded /IR.CFG";
  return true;
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
  header("HOME");
  auto& d = M5Cardputer.Display;
  const char* labels[8] = {"FILES", "USB", "WIFI", "NETWORK", "BLUETOOTH", "INFRARED", "STATUS", "ABOUT"};
  // Three visible tiles. Keep the selection centred where possible.
  const int first = std::min(5, std::max(0, homeIndex - 1));
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
    } else if (i == 3) { // Network: interconnected nodes
      d.drawLine(cx-11,cy+9,cx,cy-11,ink);
      d.drawLine(cx,cy-11,cx+12,cy+9,ink);
      d.drawLine(cx-11,cy+9,cx+12,cy+9,ink);
      d.fillCircle(cx-11,cy+9,4,ink);
      d.fillCircle(cx+12,cy+9,4,ink);
      d.fillCircle(cx,cy-11,4,ink);
    } else if (i == 4) { // Bluetooth
      d.drawLine(cx,cy-15,cx,cy+15,ink);
      d.drawLine(cx,cy-15,cx+10,cy-7,ink);
      d.drawLine(cx+10,cy-7,cx-8,cy+8,ink);
      d.drawLine(cx-8,cy-8,cx+10,cy+7,ink);
      d.drawLine(cx+10,cy+7,cx,cy+15,ink);
    } else if (i == 5) { // Infrared: emitter and signal waves
      d.fillCircle(cx-9,cy,4,ink);
      d.drawArc(cx-10,cy,11,9,300,60,ink);
      d.drawArc(cx-10,cy,18,16,300,60,ink);
    } else if (i == 6) { // Status: gauge
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
  d.drawCenterString(String(homeIndex+1) + " / 8", 120, 104);
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
  const bool connected=WiFi.status()==WL_CONNECTED;
  String ssid=connected?WiFi.SSID():"Not connected";
  if(ssid.length()>30)ssid=ssid.substring(0,27)+"...";
  String rows[]={
    "WiFi: "+ssid,
    "IP: "+(connected?WiFi.localIP().toString():String("Not connected")),
    "WiFi MAC:",
    hardwareAddress(ESP_MAC_WIFI_STA),
    "Bluetooth MAC:",
    hardwareAddress(ESP_MAC_BT),
    "SD card:",
    sdReady?String((uint32_t)(SD.cardSize()/1048576ULL))+" MiB total":"Not mounted",
    sdReady?String((uint32_t)(SD.usedBytes()/1048576ULL))+" MiB used":"--",
    sdReady?String((uint32_t)((SD.cardSize()-SD.usedBytes())/1048576ULL))+" MiB free":"--",
    "Free heap: "+String(ESP.getFreeHeap()/1024)+" KiB",
    "Min heap: "+String(ESP.getMinFreeHeap()/1024)+" KiB",
    "Heap size: "+String(ESP.getHeapSize()/1024)+" KiB",
    "Battery: "+String(M5.Power.getBatteryLevel())+"%",
    "Battery: "+String(M5.Power.getBatteryVoltage())+" mV"
  };
  const int total=sizeof(rows)/sizeof(rows[0]);
  if(statusScroll>total-7)statusScroll=total-7;
  if(statusScroll<0)statusScroll=0;
  for(int i=0;i<7 && statusScroll+i<total;++i){
    const int n=statusScroll+i;
    line(22+i*14,rows[n].substring(0,37),false,
      (n==0||n==6||n==10||n==13)?ACCENT:FG);
  }
  footer("W/S scroll  R refresh  B back");
}
static void drawAbout() {
  header("ABOUT");
  const char* rows[]={
    "M5Knife",
    "Version 0.4.5",
    "M5Stack Cardputer Adv",
    "Multi-function toolkit",
    "",
    "Developed by:",
    "Tony Blews",
    "",
    "License: MIT",
    "",
    "GitHub repository:",
    "ReconditeBox/M5Knife",
    "",
    "Built with:",
    "Arduino / PlatformIO",
    "M5Cardputer / M5Unified",
    "M5GFX / IRremote",
    "",
    "github.com/ReconditeBox/",
    "M5Knife"
  };
  constexpr int count=sizeof(rows)/sizeof(rows[0]);
  if(aboutScroll>count-7)aboutScroll=count-7;
  if(aboutScroll<0)aboutScroll=0;
  for(int i=0;i<7 && aboutScroll+i<count;++i){
    int n=aboutScroll+i;
    line(22+i*14,rows[n],false,
      (n==0 || n==5 || n==8 || n==10 || n==13)?ACCENT:FG);
  }
  footer("W/S scroll  B/G0 back");
}
static void drawIrMenu() {
  header("INFRARED");
  line(26,"NEC / Samsung TX - GPIO44",false,ACCENT);
  const char* options[]={"Manual transmit","Remote control","Load SD profile","IR information"};
  for(int i=0;i<4;++i) line(45+i*16,options[i],irMenuIndex==i);
  footer("W/S select Enter  B/G0 back");
}
static void drawIrManual() {
  header("IR TRANSMIT");
  line(24,"Protocol: NEC",false,ACCENT);
  line(43,"Address: 0x"+String(irAddress,HEX));
  line(59,"Command: 0x"+String(irCommand,HEX));
  line(77,irNotice.substring(0,35),false,MUTED);
  footer("A addr C cmd Enter SEND B back");
}
static void drawIrEdit() {
  header(irEditField==0?"IR ADDRESS":"IR COMMAND");
  line(30,"Hex value:",false,ACCENT);
  line(52,irEditText);
  line(80,irEditField==0?"0x0000 to 0xFFFF":"0x00 to 0xFF",false,MUTED);
  footer("Enter OK  Del erase  Fn+` back");
}
static void drawIrRemote() {
  header("IR REMOTE");
  line(21,(irName+(irProfileSamsung?" [SAM]":" [NEC]")).substring(0,32),false,ACCENT);
  for(int i=0;i<6;++i) {
    String name=String(irButtonNames[i])+(irButtonPresent[i]?"":" (unset)");
    line(36+i*13,name,irRemoteSelection==i);
  }
  footer("W/S choose Enter send B back");
}
static void drawNetworkMenu() {
  header("NETWORK");
  const char* options[]={"Network info","DNS lookup","Ping","TCP port scan","LAN scanner"};
  for(int i=0;i<5;++i)line(23+i*18,options[i],i==netMenuIndex);
  if(WiFi.status()!=WL_CONNECTED)line(101,"WiFi not connected",false,WARN);
  footer("W/S select Enter  B/G0 back");
}
static void drawNetworkInfo() {
  header("NETWORK INFO");
  if(WiFi.status()!=WL_CONNECTED) {
    line(40,"WiFi is not connected",false,WARN);
  } else {
    line(21,"IP: "+WiFi.localIP().toString());
    line(34,"GW: "+WiFi.gatewayIP().toString());
    line(47,"Mask: "+WiFi.subnetMask().toString());
    line(60,"DNS: "+WiFi.dnsIP().toString());
    line(73,"MAC: "+WiFi.macAddress());
    line(86,"SSID: "+WiFi.SSID().substring(0,28));
    line(99,"RSSI: "+String(WiFi.RSSI())+" dBm");
  }
  footer("Enter/B back G0 home");
}
static void drawNetworkInput() {
  header(netTool==2?"PING HOST":netTool==1?"DNS LOOKUP":netTool==3?"TCP PORT SCAN":"LAN RANGE");
  line(27,netTool==4?"StartIP-EndIP:":netTool==3?"Host (common ports):":"Address or hostname:",false,ACCENT);
  line(51,netTarget.length()>35?netTarget.substring(netTarget.length()-35):netTarget);
  footer("Enter run  Del delete Fn+\x60 cancel");
}
static void drawNetworkResult() {
  header(netTool==2?"PING RESULT":netTool==1?"DNS RESULT":netTool==3?"PORT RESULTS":"LAN RESULTS");
  size_t pos=0, row=0; int y=23;
  while(pos<netOutput.length() && y<=108){
    int end=netOutput.indexOf('\n',pos);
    if(end<0)end=netOutput.length();
    if(row++>=netResultScroll){
      line(y,netOutput.substring(pos,end).substring(0,38));
      y+=14;
    }
    pos=end+1;
  }
  footer("W/S scroll  B back  G0 home");
}
static uint16_t netChecksum(const uint8_t *p,size_t len){
  uint32_t sum=0;
  for(size_t i=0;i+1<len;i+=2)sum+=((uint16_t)p[i]<<8)|p[i+1];
  if(len&1)sum+=(uint16_t)p[len-1]<<8;
  while(sum>>16)sum=(sum&65535)+(sum>>16);
  return (uint16_t)~sum;
}
// Short TCP connect attempts; intended for troubleshooting networks you control.
static bool netResolve(const String& host,IPAddress& address){
  return address.fromString(host) || WiFi.hostByName(host.c_str(),address);
}
static void netPortScan(){
  IPAddress ip;
  if(!netResolve(netTarget,ip)){netOutput="Host resolution failed";return;}
  const uint16_t ports[]={21,22,23,25,53,80,110,139,443,445,3389,8080};
  netOutput=ip.toString()+"\n";
  int opened=0;
  for(uint16_t port:ports){
    WiFiClient client;
    bool ok=client.connect(ip,port,180);
    client.stop();
    if(ok){netOutput+=String(port)+" OPEN\n";++opened;}
    delay(1);
  }
  if(!opened)netOutput+="No listed ports open";
}
static uint32_t netIpNumber(IPAddress ip){
  return ((uint32_t)ip[0]<<24)|((uint32_t)ip[1]<<16)|((uint32_t)ip[2]<<8)|ip[3];
}
static IPAddress netIpAddress(uint32_t value){
  return IPAddress((uint8_t)(value>>24),(uint8_t)(value>>16),(uint8_t)(value>>8),(uint8_t)value);
}
static String netPaddedIp(IPAddress ip){
  char buffer[16];
  snprintf(buffer,sizeof(buffer),"%03u.%03u.%03u.%03u",
    (unsigned)ip[0],(unsigned)ip[1],(unsigned)ip[2],(unsigned)ip[3]);
  return String(buffer);
}
static void netLanScan(){
  int separator=netTarget.indexOf('-');
  if(separator<0){netOutput="Use startIP-endIP";return;}
  IPAddress from,to;
  if(!from.fromString(netTarget.substring(0,separator)) ||
     !to.fromString(netTarget.substring(separator+1))){
    netOutput="Invalid IPv4 range";return;
  }
  uint32_t first=netIpNumber(from),last=netIpNumber(to);
  if(last<first || (uint64_t)last-first+1>256){
    netOutput="Select 1 to 256 addresses";return;
  }
  netOutput="Active hosts:\n";
  header("LAN SCANNING");
  line(22,"Checking:",false,ACCENT);
  line(52,"Active hosts / open ports:",false,MUTED);
  footer("Results update during scan");
  const uint16_t ports[]={21,22,23,53,80,139,443,445,3389,8080};
  int found=0;
  for(uint32_t number=first;;++number){
    IPAddress ip=netIpAddress(number);
    String openPorts;
    // The current address is transient; only responding hosts remain listed.
    M5Cardputer.Display.fillRect(0,35,240,15,BG);
    line(36,netPaddedIp(ip),false,ACCENT);
    // A device is considered active only if a TCP connect succeeds.
    // Silent devices or filtered ports cannot be discovered this way.
    for(uint16_t port:ports){
      WiFiClient client;
      bool open=client.connect(ip,port,60);
      client.stop();
      if(open){
        if(openPorts.length())openPorts+=",";
        openPorts+=String(port);
      }
    }
    if(openPorts.length()){
      String hostLine=netPaddedIp(ip)+"  "+openPorts;
      netOutput+=hostLine+"\n";
      ++found;
      // Show the newest four discoveries immediately as the scan continues.
      // Earlier hits remain in netOutput for scrolling on the results screen.
      const int slot=(found-1)%4;
      if(slot==0) M5Cardputer.Display.fillRect(0,65,240,54,BG);
      line(67+slot*13,hostLine.substring(0,38));
    }
    if(number==last)break;
    delay(1);
  }
  if(!found)netOutput+="None found (TCP only)";
  // runNetworkQuery() draws the final results, removing scan progress.
}
static void runNetworkQuery(){
  netOutput="";
  if(WiFi.status()!=WL_CONNECTED)netOutput="Connect to WiFi first.";
  else if(netTool==4)netLanScan();
  else if(netTool==3)netPortScan();

  else if(netTarget.isEmpty())netOutput="Enter a host.";
  else{
    IPAddress address;
    bool ok=address.fromString(netTarget);
    if(!ok)ok=WiFi.hostByName(netTarget.c_str(),address);
    if(!ok)netOutput="DNS resolution failed.";
    else if(!netPingMode)netOutput=netTarget+"\n"+address.toString();
    else{
      netOutput=netTarget+"\n"+address.toString()+"\n";
      int fd=socket(AF_INET,SOCK_RAW,IPPROTO_ICMP);
      if(fd<0)netOutput+="ICMP socket unavailable";
      else{
        sockaddr_in destination={};
        destination.sin_family=AF_INET;
        destination.sin_addr.s_addr=(uint32_t)address;
        struct timeval timeout={1,0};
        setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
        const uint16_t ident=(uint16_t)(millis()&65535);
        for(int seq=1;seq<=3;++seq){
          uint8_t packet[16]={8,0,0,0,(uint8_t)(ident>>8),(uint8_t)ident,0,(uint8_t)seq};
          for(int i=8;i<16;++i)packet[i]=(uint8_t)i;
          uint16_t checksum=netChecksum(packet,sizeof(packet));
          packet[2]=(uint8_t)(checksum>>8);packet[3]=(uint8_t)checksum;
          uint32_t began=millis();
          bool reply=false;
          if(sendto(fd,packet,sizeof(packet),0,(sockaddr*)&destination,sizeof(destination))>=0){
            uint8_t received[128];
            uint32_t deadline=began+1000;
            while((int32_t)(deadline-millis())>0){
              int len=recv(fd,received,sizeof(received),0);
              if(len<=0)break;
              int off=(received[0]>>4)==4 ? (received[0]&15)*4:0;
              if(len>=off+8 && received[off]==0 && received[off+1]==0 &&
                 received[off+4]==packet[4] && received[off+5]==packet[5] &&
                 received[off+6]==0 && received[off+7]==(uint8_t)seq){reply=true;break;}
            }
          }
          netOutput+=String(seq)+": "+(reply?String(millis()-began)+" ms":"timeout")+"\n";
          delay(100);
        }
        close(fd);
      }
    }
  }
  netResultScroll=0;
  screen=Screen::NetResult;draw();
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
    case Screen::Network: drawNetworkMenu(); break;
    case Screen::NetInfo: drawNetworkInfo(); break;
    case Screen::NetInput: drawNetworkInput(); break;
    case Screen::NetResult: drawNetworkResult(); break;
    case Screen::Bluetooth: drawScan(false); break;
    case Screen::Infrared: drawIrMenu(); break;
    case Screen::IrManual: drawIrManual(); break;
    case Screen::IrEdit: drawIrEdit(); break;
    case Screen::IrRemote: drawIrRemote(); break;
    case Screen::Status: drawStatus(); break;
    case Screen::About: drawAbout(); break;
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
    if(right && homeIndex < 7) ++homeIndex;
    if(k.enter) {
      // Explicit dispatch for all eight launcher tiles.
      switch (homeIndex) {
        case 0: openFiles(); break;
        case 1: startUsb(); break;
        case 2: scanWifi(); break;
        case 3:
          if(WiFi.status()==WL_CONNECTED) {
            screen=Screen::Network;netMenuIndex=0;draw();
          } else showMessage("Connect to WiFi first",Screen::Home);
          break;
        case 4: scanBluetooth(); break;
        case 5:
          screen = Screen::Infrared;
          draw();
          break;
        case 6:
          screen = Screen::Status;
          statusScroll=0;
          drawStatus();
          break;
        case 7: screen=Screen::About;aboutScroll=0;drawAbout();break;
      }
      return;
    }
  } else if(screen==Screen::About) {
    if(escape||backspace||typed(k,'b')){goHome();return;}
    if(up && aboutScroll>0)--aboutScroll;
    if(down && aboutScroll<13)++aboutScroll;
    drawAbout();return;
  } else if(screen==Screen::Network) {
    if(escape||backspace||typed(k,'b')){goHome();return;}
    if(up && netMenuIndex>0)--netMenuIndex;
    if(down && netMenuIndex<4)++netMenuIndex;
    if(k.enter){
      if(netMenuIndex==0){screen=Screen::NetInfo;draw();return;}
      netTool=netMenuIndex;
      netPingMode=(netTool==2);
      if(netTool==4){
        IPAddress ip=WiFi.localIP(),mask=WiFi.subnetMask();
        uint32_t base=netIpNumber(ip)&netIpNumber(mask);
        uint32_t broadcast=base|(~netIpNumber(mask));
        uint32_t first=base+1,last=broadcast>base+1?broadcast-1:base+1;
        if((uint64_t)last-first+1>256)last=first+255;
        netTarget=netIpAddress(first).toString()+"-"+netIpAddress(last).toString();
      } else netTarget=netTool==2?netDefaultTarget:netTool==3?WiFi.gatewayIP().toString():"";

      screen=Screen::NetInput;draw();return;
    }
  } else if(screen==Screen::NetInfo) {
    if(escape||backspace||typed(k,'b')||k.enter){screen=Screen::Network;draw();return;}
  } else if(screen==Screen::NetInput) {
    if(escape){screen=Screen::Network;draw();return;}
    if(k.enter){
      runNetworkQuery();return;
    }
    if(k.del){if(netTarget.length())netTarget.remove(netTarget.length()-1);}
    else if(!k.fn) for(char c:k.word)
      if(c>=33 && c<=126 && netTarget.length()<63)netTarget+=c;
  } else if(screen==Screen::NetResult) {
    if(escape||backspace||typed(k,'b')||k.enter){screen=Screen::Network;draw();return;}
    if(up && netResultScroll>0)--netResultScroll;
    if(down && netResultScroll<200)++netResultScroll;
  } else if(screen==Screen::Infrared) {
    if(escape||backspace||typed(k,'b')) {goHome();return;}
    if(up && irMenuIndex>0)--irMenuIndex;
    if(down && irMenuIndex<3)++irMenuIndex;
    if(k.enter){
      if(irMenuIndex==0){screen=Screen::IrManual;irNotice="";draw();return;}
      if(irMenuIndex==1){screen=Screen::IrRemote;irRemoteSelection=0;draw();return;}
      if(irMenuIndex==2){irLoadProfile();draw();return;}
      showMessage("IR TX: GPIO44\\nProtocols: NEC, Samsung\\nProfiles: /IR.CFG\\nRX requires extra hardware",Screen::Infrared);return;
    }
  } else if(screen==Screen::IrManual) {
    if(escape||backspace||typed(k,'b')){screen=Screen::Infrared;draw();return;}
    if(typed(k,'a')||typed(k,'c')){
      irEditField=typed(k,'a')?0:1;
      irEditText=irEditField==0?String(irAddress,HEX):String(irCommand,HEX);
      screen=Screen::IrEdit;draw();return;
    }
    if(k.enter){irSend(irAddress,irCommand);draw();return;}
  } else if(screen==Screen::IrEdit) {
    if(escape){screen=Screen::IrManual;draw();return;}
    if(k.enter) {
      uint32_t value=0;
      if(parseHexValue(irEditText,value,irEditField==0?65535:255)){
        if(irEditField==0)irAddress=(uint16_t)value;else irCommand=(uint8_t)value;
        irNotice="Value updated";screen=Screen::IrManual;draw();return;
      }
      irNotice="Invalid hex";screen=Screen::IrManual;draw();return;
    }
    if(k.del && irEditText.length())irEditText.remove(irEditText.length()-1);
    else if(!k.fn)for(char c:k.word)if(irEditText.length()<6 && ((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')))irEditText+=c;
  } else if(screen==Screen::IrRemote) {
    if(escape||backspace||typed(k,'b')){screen=Screen::Infrared;draw();return;}
    if(up && irRemoteSelection>0)--irRemoteSelection;
    if(down && irRemoteSelection<5)++irRemoteSelection;
    if(k.enter){
      if(irProfileLoaded && irButtonPresent[irRemoteSelection])
        irSend(irProfileAddress,irButtonCommands[irRemoteSelection],irProfileSamsung);
      else irNotice="Load IR.CFG / code unset";
      draw();return;
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
    if(up && statusScroll>0)--statusScroll;
    if(down && statusScroll<8)++statusScroll;
    drawStatus();return;
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
  IrSender.begin(DISABLE_LED_FEEDBACK);
  IrSender.setSendPin(IR_TX_PIN);
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
  if((screen==Screen::Network || screen==Screen::NetInfo ||
      screen==Screen::NetInput || screen==Screen::NetResult) &&
      WiFi.status()!=WL_CONNECTED) {
    showMessage("WiFi disconnected",Screen::Home);
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
