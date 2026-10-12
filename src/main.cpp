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
#include <WebServer.h>
#include <Preferences.h>
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
#include <time.h>

// M5Stack Cardputer Adv microSD slot, shared with the original Cardputer.
static constexpr int SD_CS = 12, SD_MOSI = 14, SD_MISO = 39, SD_SCK = 40;
static constexpr uint16_t BG = 0x0841, FG = 0xDFBE, ACCENT = 0x07E0;
static constexpr uint16_t MUTED = 0x8410, SEL = 0x020E, WARN = 0xFD20;

using Keys = Keyboard_Class::KeysState;
enum class Screen { Home, Files, Viewer, Disk, Usb, Input, Confirm, Message, Wifi, Network, NetInfo, NetInput, NetResult, Bluetooth, Infrared, IrManual, IrEdit, IrRemote, Status, WifiPassword, WifiConnecting, About, Settings, Editor, EditorSearch, HttpFiles };
enum class Action { None, Mkdir, Rename, Delete, NewText };
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
static int settingsIndex=0, viewerTop=0, editorLine=0, editorColumn=0;
static bool autoWifi=true, ntpEnabled=true;
static int brightness=128, sleepMinutes=0, tzHours=0;
static uint32_t lastInteraction=0;
static String editorText, searchText, editorNotice;
static bool editorDirty=false, editorSearchMode=false;
static bool ntpStarted=false;
static bool waitingKeyRelease = false;
struct ScanEntry { String label; int rssi; bool secured; };
static std::vector<ScanEntry> scanEntries;
static size_t scanSelection = 0, scanScroll = 0;
static String scanStatus;
static bool bleInitialized = false;
static int netMenuIndex=0;
static WebServer httpFiles(80);
static bool httpRunning=false,httpUploadFailed=false,httpRoutesRegistered=false;
static bool httpHotspotActive=false;
static uint8_t httpQrMode=0; // 0 status, 1 WiFi join QR, 2 HTTP URL QR
static String hotspotSsid="M5Knife", hotspotPassword="M5Knife1234";
static String httpPassword="1234",httpUploadPath;
static File httpUploadFile;
static uint32_t httpRequests=0;
static bool netPingMode=false;
static int netTool=0;
static size_t netResultScroll=0;
static String netTarget, netOutput;
static String netDefaultTarget="8.8.8.8";
static void startHttpFiles(bool forceHotspot=false);
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
static void showMessage(const String&, Screen);

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
  d.drawString("M5KNIFE 0.5.13", 5, 5);
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
// Unified SD configuration. Old files are kept as migration backups.
static const char* CONFIG_FILE="/M5KNIFE.CFG";
static bool configWriteAtomic(const String& data);
static bool configReadFile(const char* path,String& out);
static String activeConfig;
static bool nvsReady=false;
static bool configStoreNvs(const String& data){
  if(!nvsReady||data.length()>16384)return false;
  Preferences prefs;
  if(!prefs.begin("m5knife",false))return false;
  size_t done=prefs.putBytes("config",data.c_str(),data.length()+1);
  prefs.end();return done==data.length()+1;
}
static bool configLoadNvs(String& data){
  if(!nvsReady)return false;
  Preferences prefs;
  if(!prefs.begin("m5knife",true))return false;
  size_t count=prefs.getBytesLength("config");
  if(count<2||count>16385){prefs.end();return false;}
  std::vector<char> bytes(count);
  size_t got=prefs.getBytes("config",bytes.data(),count);
  prefs.end();if(got!=count||bytes.back()!=0)return false;
  data=String(bytes.data());return data.length()==count-1;
}
static bool configApply(const String& data){
  if(data.indexOf("[SETTINGS]")<0||data.indexOf("[HTTPD]")<0||data.length()>16384)return false;
  if(!configStoreNvs(data))return false;
  activeConfig=data;return true;
}
static bool configExportSd(){
  if(usbActive||(!sdReady&&!mountSd()))return false;
  return configWriteAtomic(activeConfig);
}
static bool configImportSd(){
  if(usbActive||(!sdReady&&!mountSd()))return false;
  String data;
  if(!configReadFile(CONFIG_FILE,data))return false;
  return configApply(data);
}

static String httpUsername="m5knife";
static bool configReadFile(const char* path,String& out){
  File f=SD.open(path,FILE_READ);
  if(!f)return false;
  if(f.size()>16384){f.close();return false;}
  out="";out.reserve(f.size()+1);
  while(f.available())out+=(char)f.read();
  f.close();return true;
}
static String configSection(const String& data,const String& section){
  String tag="["+section+"]";
  int pos=data.indexOf(tag);
  while(pos>=0){
    if((pos==0||data[pos-1]=='\n') &&
      (pos+(int)tag.length()==(int)data.length()||data[pos+tag.length()]=='\r'||data[pos+tag.length()]=='\n')){
      int begin=data.indexOf('\n',pos);
      if(begin<0)return "";
      ++begin;
      int next=data.indexOf("\n[",begin);
      return data.substring(begin,next<0?data.length():next+1);
    }
    pos=data.indexOf(tag,pos+1);
  }
  return "";
}
static bool configWriteAtomic(const String& data){
  if(data.length()>16384)return false;
  const char* tmp="/m5knife.tmp";
  const char* backup="/m5knife.bak";
  SD.remove(tmp);
  File f=SD.open(tmp,FILE_WRITE);if(!f)return false;
  size_t written=f.print(data);f.flush();f.close();
  if(written!=data.length()){SD.remove(tmp);return false;}
  SD.remove(backup);
  bool hadOriginal=SD.exists(CONFIG_FILE);
  if(hadOriginal&&!SD.rename(CONFIG_FILE,backup)){SD.remove(tmp);return false;}
  if(!SD.rename(tmp,CONFIG_FILE)){
    if(hadOriginal)SD.rename(backup,CONFIG_FILE);
    SD.remove(tmp);return false;
  }
  SD.remove(backup);
  return true;
}
static bool configReplaceSection(const String& name,const String& body){
  String original=activeConfig;
  String header="["+name+"]",replacement=header+"\n"+body;
  if(!replacement.endsWith("\n"))replacement+="\n";
  int pos=original.indexOf(header);
  if(pos>=0&&(pos==0||original[pos-1]=='\n')){
    int next=original.indexOf("\n[",pos+header.length());
    if(next>=0)original=original.substring(0,pos)+replacement+original.substring(next+1);
    else original=original.substring(0,pos)+replacement;
  }else{
    if(original.length()&&!original.endsWith("\n"))original+="\n";
    original+=replacement;
  }
  return configApply(original);
}
// [WIFI] in /M5KNIFE.CFG uses SSID1/PASS1, SSID2/PASS2, etc.
// Preserve spaces and punctuation in values; no trimming or comment parsing.
struct WifiRecord { String ssid, pass; };
static constexpr size_t MAX_WIFI_RECORDS=16;
static bool wifiConfigSdReady() {
  return nvsReady;
}
static bool readWifiRecords(std::vector<WifiRecord>& records) {
  records.clear();
  String data;
  data=activeConfig;
  String wifiBlock=configSection(data,"WIFI");
  if(wifiBlock.isEmpty())return true;
  size_t offset=0;
  String names[MAX_WIFI_RECORDS], passwords[MAX_WIFI_RECORDS];
  bool hasName[MAX_WIFI_RECORDS]={}, hasPassword[MAX_WIFI_RECORDS]={};
  while(offset<wifiBlock.length()) {
    int eol=wifiBlock.indexOf('\n',offset);
    String v=eol<0?wifiBlock.substring(offset):wifiBlock.substring(offset,eol);
    offset=eol<0?wifiBlock.length():eol+1;
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
  for(size_t i=0;i<MAX_WIFI_RECORDS;++i)
    if(hasName[i]&&hasPassword[i]&&names[i].length()>0&&names[i].length()<=32&&passwords[i].length()<=63)
      records.push_back({names[i],passwords[i]});
  return true;
}
static bool writeWifiRecords(const std::vector<WifiRecord>& records) {
  String body;
  for(size_t i=0;i<records.size();++i)
    body+=String("SSID")+String(i+1)+"="+records[i].ssid+"\n"+
      "PASS"+String(i+1)+"="+records[i].pass+"\n";
  return configReplaceSection("WIFI",body);
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
  if(!readWifiRecords(records)) {
    scanStatus="Cannot read M5KNIFE.CFG";draw();return;
  }
  bool found=false;
  for(auto& item:records)
    if(item.ssid==wifiTarget) {item.pass=wifiPassword;found=true;break;}
  if(!found) {
    if(records.size()>=MAX_WIFI_RECORDS) {scanStatus="WiFi config full";draw();return;}
    records.push_back({wifiTarget,wifiPassword});
  }
  scanStatus=writeWifiRecords(records) ? "WiFi saved in NVS" : "NVS save failed";
  draw();
}
static void loadWifiConfig() {
  if(!wifiConfigSdReady()) {scanStatus="SD card unavailable";draw();return;}
  std::vector<WifiRecord> records;
  if(!readWifiRecords(records)) {scanStatus="No /M5KNIFE.CFG on SD";draw();return;}
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
  scanStatus="Selected WiFi not in M5KNIFE.CFG";draw();
}

static void readSettings(){
  String data=activeConfig;
  if(data.isEmpty())return;
  String block=configSection(data,"SETTINGS");
  int offset=0;
  while(offset<(int)block.length()){
    int end=block.indexOf('\n',offset);
    String row=end<0?block.substring(offset):block.substring(offset,end);
    offset=end<0?block.length():end+1;row.trim();
    int equal=row.indexOf('=');if(equal<0)continue;
    String key=row.substring(0,equal),value=row.substring(equal+1);
    if(key=="AUTO_WIFI")autoWifi=value!="0";
    else if(key=="NTP")ntpEnabled=value!="0";
    else if(key=="BRIGHTNESS")brightness=constrain(value.toInt(),20,255);
    else if(key=="SLEEP_MIN")sleepMinutes=constrain(value.toInt(),0,60);
    else if(key=="TZ_HOURS")tzHours=constrain(value.toInt(),-12,14);
  }
  block=configSection(data,"HTTPD");offset=0;
  while(offset<(int)block.length()){
    int end=block.indexOf('\n',offset);
    String row=end<0?block.substring(offset):block.substring(offset,end);
    offset=end<0?block.length():end+1;
    if(row.endsWith("\r"))row.remove(row.length()-1);
    int equal=row.indexOf('=');if(equal<0)continue;
    String key=row.substring(0,equal),value=row.substring(equal+1);
    if(key=="USERNAME"&&value.length()&&value.length()<=64)httpUsername=value;
    if(key=="PASSWORD"&&value.length()&&value.length()<=64)httpPassword=value;
  }
  // Optional [HOTSPOT] config in the same NVS-backed M5KNIFE.CFG.
  // Missing or invalid values use the built-in defaults, including on re-import.
  hotspotSsid="M5Knife";
  hotspotPassword="M5Knife1234";
  block=configSection(data,"HOTSPOT");offset=0;
  while(offset<(int)block.length()){
    int end=block.indexOf('\n',offset);
    String row=end<0?block.substring(offset):block.substring(offset,end);
    offset=end<0?block.length():end+1;
    if(row.endsWith("\r"))row.remove(row.length()-1);
    int equal=row.indexOf('=');if(equal<0)continue;
    String key=row.substring(0,equal),value=row.substring(equal+1);
    if(key=="SSID"&&value.length()>=1&&value.length()<=32 &&
       value.indexOf('\r')<0 && value.indexOf('\n')<0)hotspotSsid=value;
    else if(key=="PASSWORD"&&value.length()>=8&&value.length()<=63 &&
       value.indexOf('\r')<0 && value.indexOf('\n')<0)hotspotPassword=value;
  }
}
static bool saveSettings(){
  String body=String("AUTO_WIFI=")+(autoWifi?"1":"0")+"\n"+
    "NTP="+(ntpEnabled?"1":"0")+"\n"+
    "BRIGHTNESS="+String(brightness)+"\n"+
    "SLEEP_MIN="+String(sleepMinutes)+"\n"+
    "TZ_HOURS="+String(tzHours)+"\n";
  return configReplaceSection("SETTINGS",body);
}
static void applySettings(){
  M5Cardputer.Display.setBrightness(brightness);
}
static void startNtp(){
  if(ntpEnabled && !ntpStarted && WiFi.status()==WL_CONNECTED){
    configTime(tzHours*3600,0,"pool.ntp.org","time.google.com");
    ntpStarted=true;
  }
}
static void autoConnectWifi(){
  if(!autoWifi||WiFi.status()==WL_CONNECTED)return;
  std::vector<WifiRecord> saved;
  if(!readWifiRecords(saved)||saved.empty())return;
  wifiTarget=saved[0].ssid;wifiPassword=saved[0].pass;
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiTarget.c_str(),wifiPassword.c_str());
  wifiConnectStarted=millis();
  // Startup connection is asynchronous; the user can keep using the menu.
}
static void drawSettings(){
  header("SETTINGS");
  String rows[]={
    String("Auto WiFi: ")+(autoWifi?"ON":"OFF"),
    String("NTP sync: ")+(ntpEnabled?"ON":"OFF"),
    "Timezone: UTC"+String(tzHours>=0?"+":"")+String(tzHours),
    "Brightness: "+String(brightness),
    "Sleep: "+(sleepMinutes?String(sleepMinutes)+" min":String("Never")),
    "Save settings",
    "Sync time now",
    "Import config from SD",
    "Export config to SD"
  };
  const int first=std::max(0,settingsIndex-5);
  for(int i=first;i<9;i++)line(22+(i-first)*14,rows[i],i==settingsIndex);
  footer("W/S select A/D change Enter");
}
static void settingsActivate(int delta){
  if(settingsIndex==0)autoWifi=!autoWifi;
  else if(settingsIndex==1){ntpEnabled=!ntpEnabled;ntpStarted=false;}
  else if(settingsIndex==2){tzHours=constrain(tzHours+delta,-12,14);ntpStarted=false;}
  else if(settingsIndex==3)brightness=constrain(brightness+delta*16,20,255);
  else if(settingsIndex==4)sleepMinutes=constrain(sleepMinutes+delta,0,60);
  else if(settingsIndex==5){showMessage(saveSettings()?"Settings saved in NVS":"NVS save failed",Screen::Settings);return;}
  else if(settingsIndex==6){ntpStarted=false;startNtp();}
  else if(settingsIndex==7){
    if(configImportSd()){readSettings();applySettings();showMessage("Config imported to NVS",Screen::Settings);}
    else showMessage("Import failed / no SD",Screen::Settings);
    return;
  }
  else if(settingsIndex==8){showMessage(configExportSd()?"Exported M5KNIFE.CFG":"Export failed / no SD",Screen::Settings);return;}
  applySettings();drawSettings();
}

static bool irLoadProfile() {
  String data=activeConfig;
  if(data.isEmpty()){irNotice="No stored config";return false;}
  String block=configSection(data,"REMOTE");
  if(block.isEmpty()){irNotice="No [REMOTE] profile";return false;}
  int offset=0;
  String name="";uint32_t address=0;bool addressOk=false;
  uint8_t commands[6]={};bool found[6]={};
  bool inRemote=true,nec=false,samsung=false;
  while(offset<(int)block.length()) {
    int end=block.indexOf('\n',offset);
    String entry=end<0?block.substring(offset):block.substring(offset,end);
    offset=end<0?block.length():end+1;entry.trim();
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
  if((!nec && !samsung) || !addressOk){irNotice="IR profile: protocol/address?";return false;}
  irName=name.length()?name:"IR remote";
  irProfileAddress=(uint16_t)address;
  irProfileSamsung=samsung;
  for(int i=0;i<6;i++){irButtonCommands[i]=commands[i];irButtonPresent[i]=found[i];}
  irProfileLoaded=true; irNotice="Loaded NVS profile";
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
  auto& d=M5Cardputer.Display;
  const char* labels[9]={"FILES","USB","WIFI","NETWORK","BLUETOOTH","INFRARED","SETTINGS","STATUS","ABOUT"};
  const uint16_t colors[9]={0xFDE0,0x07FF,0x07E0,0x07FF,0x4DFF,0xFB26,0xFFFF,0x07E0,0x4DFF};
  // Three visible tiles; preserve existing scrolling and selection behaviour.
  const int first=std::min(6,std::max(0,homeIndex-1));
  for(int slot=0;slot<3;++slot){
    const int i=first+slot,x=8+slot*78,y=29,cx=x+34,cy=y+27;
    const bool active=homeIndex==i;
    const uint16_t ink=colors[i],frame=active?ACCENT:0x2308,fill=active?0x0923:0x0841;
    d.fillRoundRect(x,y,68,70,6,fill);
    d.drawRoundRect(x,y,68,70,6,frame);
    if(active){
      d.drawRoundRect(x+2,y+2,64,66,5,0x0544);
      d.drawFastHLine(x+12,y,44,ACCENT);
    }
    // Outline-only iconography, bold enough to survive a 240x135 display.
    if(i==0){ // folder outline
      d.drawRoundRect(cx-16,cy-8,32,23,3,ink);
      d.drawLine(cx-16,cy-8,cx-12,cy-12,ink);
      d.drawLine(cx-12,cy-12,cx-3,cy-12,ink);
      d.drawLine(cx-3,cy-12,cx+2,cy-8,ink);
      d.drawFastHLine(cx-12,cy-2,24,ink);
    }else if(i==1){ // USB trident
      d.drawLine(cx,cy-15,cx,cy+13,ink);
      d.drawLine(cx,cy-15,cx-5,cy-9,ink);
      d.drawLine(cx,cy-15,cx+5,cy-9,ink);
      d.drawLine(cx,cy,cx-12,cy-7,ink);
      d.drawCircle(cx-12,cy-7,3,ink);
      d.drawLine(cx,cy+6,cx+12,cy-2,ink);
      d.drawRect(cx+9,cy-5,6,6,ink);
      d.drawCircle(cx,cy+14,2,ink);
    }else if(i==2){ // WiFi signal
      d.drawArc(cx,cy+9,23,20,220,320,ink);
      d.drawArc(cx,cy+9,16,13,220,320,ink);
      d.drawArc(cx,cy+9,9,7,220,320,ink);
      d.fillCircle(cx,cy+9,3,ink);
    }else if(i==3){ // wired network
      d.drawLine(cx,cy-12,cx,cy-3,ink);
      d.drawFastHLine(cx-13,cy-3,26,ink);
      d.drawLine(cx-13,cy-3,cx-13,cy+9,ink);
      d.drawLine(cx+13,cy-3,cx+13,cy+9,ink);
      d.drawRoundRect(cx-5,cy-17,10,9,2,ink);
      d.drawRoundRect(cx-18,cy+8,10,9,2,ink);
      d.drawRoundRect(cx+8,cy+8,10,9,2,ink);
    }else if(i==4){ // Bluetooth rune
      d.drawLine(cx,cy-17,cx,cy+17,ink);
      d.drawLine(cx,cy-17,cx+12,cy-7,ink);
      d.drawLine(cx+12,cy-7,cx-11,cy+10,ink);
      d.drawLine(cx-11,cy-10,cx+12,cy+7,ink);
      d.drawLine(cx+12,cy+7,cx,cy+17,ink);
    }else if(i==5){ // infrared waves
      d.drawRoundRect(cx-18,cy-6,12,12,3,ink);
      d.fillCircle(cx-12,cy,2,ink);
      d.drawArc(cx-13,cy,16,12,315,45,ink);
      d.drawArc(cx-13,cy,24,20,315,45,ink);
    }else if(i==6){ // settings gear
      d.drawCircle(cx,cy,12,ink);
      d.drawCircle(cx,cy,5,ink);
      for(int a=0;a<8;++a){
        const float ang=a*3.1415926f/4;
        d.drawLine(cx+(int)(12*cos(ang)),cy+(int)(12*sin(ang)),
                   cx+(int)(17*cos(ang)),cy+(int)(17*sin(ang)),ink);
      }
    }else if(i==7){ // status graph bars
      d.drawRect(cx-16,cy+3,7,12,ink);
      d.drawRect(cx-3,cy-4,7,19,ink);
      d.drawRect(cx+10,cy-13,7,28,ink);
      d.drawFastHLine(cx-18,cy+17,37,ink);
    }else{ // outlined info
      d.drawCircle(cx,cy,14,ink);
      d.fillCircle(cx,cy-8,2,ink);
      d.drawFastVLine(cx,cy-2,11,ink);
      d.drawFastHLine(cx-3,cy+9,7,ink);
    }
    d.setTextColor(active?ACCENT:FG,fill);
    d.drawCenterString(labels[i],cx,y+53);
  }
  d.setTextColor(MUTED,BG);
  d.drawCenterString(String(homeIndex+1)+" / 9",120,104);
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
    "Battery: "+String(M5.Power.getBatteryVoltage())+" mV",
    "Clock:",
    []()->String {struct tm t; if(!getLocalTime(&t,10))return "Not synchronised";char b[28];strftime(b,sizeof(b),"%Y-%m-%d %H:%M:%S",&t);return String(b);}()
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
    "Version 0.5.13",
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
  const char* options[]={"Network info","DNS lookup","Ping","TCP port scan","LAN scanner","HTTP File Server","HTTP Hotspot"};
  for(int i=0;i<7;++i)line(20+i*14,options[i],i==netMenuIndex);
  footer("W/S select Enter  B/G0 back");
}
static String httpServerIp(){
  return (httpHotspotActive?WiFi.softAPIP():WiFi.localIP()).toString();
}
static String hotspotQrEscape(String value){
  value.replace("\\","\\\\");
  value.replace(";","\\;");
  value.replace(",","\\,");
  value.replace(":","\\:");
  return value;
}
static void drawHttpQr(){
  auto& d=M5Cardputer.Display;
  const bool join=httpHotspotActive && httpQrMode==1;
  const String ip=httpServerIp();
  const String qr=join?
    "WIFI:T:WPA;S:"+hotspotQrEscape(hotspotSsid)+";P:"+hotspotQrEscape(hotspotPassword)+";;":
    "http://"+ip;
  d.fillScreen(BG);
  d.fillRect(1,1,132,132,0xFFFF);
  // Use enough QR modules for long custom access-point credentials.
  const uint8_t version=join?(qr.length()<=50?4:qr.length()<=80?6:8):2;
  d.qrcode(qr.c_str(),17,17,100,version);
  d.setTextSize(1);
  d.setTextColor(ACCENT,BG);
  d.drawString(join?"JOIN WIFI":"HTTP FILES",140,8);
  d.setTextColor(FG,BG);
  d.drawString(join?"Scan to join":"Scan to open",140,27);
  if(join){
    d.drawString(hotspotSsid.substring(0,16),140,46);
    d.drawString("Then Q for URL",140,64);
  }else{
    d.drawString("http://",140,46);
    d.drawString(ip,140,60);
  }
  d.setTextColor(MUTED,BG);
  d.drawString("Q/Enter next",140,90);
  d.drawString("B/G0 stop",140,107);
}
static void drawHttpFiles(){
  if(httpQrMode!=0){drawHttpQr();return;}
  if(httpHotspotActive){
    header("HTTP HOTSPOT");
    line(22,"WiFi: "+hotspotSsid.substring(0,24),false,ACCENT);
    line(36,"Key: "+hotspotPassword.substring(0,25));
    line(50,"URL: http://"+httpServerIp());
    line(64,"HTTP user: "+httpUsername.substring(0,18));
    line(78,"HTTP pass: "+httpPassword.substring(0,18));
    line(92,"Requests: "+String(httpRequests));
    footer("Q join QR / URL QR  B/G0 stop");
  }else{
    header("HTTP FILES");
    line(24,"Server running",false,ACCENT);
    line(42,"http://"+httpServerIp());
    line(61,"User: "+httpUsername,false,ACCENT);
    line(78,"Pass: "+httpPassword,false,ACCENT);
    line(96,"Requests: "+String(httpRequests));
    footer("Q QR code  R refresh  B/G0 stop");
  }
}
static void stopHttpFiles(){
  httpQrMode=0;
  if(httpRunning){
    if(httpUploadFile){httpUploadFile.close();SD.remove(httpUploadPath.c_str());}
    httpFiles.stop();
    httpRunning=false;
  }
  if(httpHotspotActive){
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    httpHotspotActive=false;
    autoConnectWifi();
  }
}
static bool httpValidPath(const String& path){
  if(!path.startsWith("/")||path.length()>180||path.indexOf('\\')>=0)return false;
  String part="";
  for(size_t i=1;i<=path.length();++i){
    char c=i<path.length()?path[i]:'/';
    if(c=='/'){
      if(part=="."||part==".."||part.length()>100)return false;
      part="";
    }else{
      if((unsigned char)c<32||c==':'||c=='*'||c=='?'||c=='<'||c=='>'||c=='|'||c=='"')return false;
      part+=c;
    }
  }
  return true;
}
static bool httpValidName(const String& name){
  return name.length()>0 && name.length()<=80 && name!="." && name!=".." &&
    name.indexOf('/')<0 && httpValidPath("/"+name);
}
static String httpChild(const String& parent,const String& name){
  return parent=="/"?"/"+name:parent+"/"+name;
}
static String httpEscape(const String& input){
  String out;
  for(char c:input){
    if(c=='&')out+="&amp;";
    else if(c=='<')out+="&lt;";
    else if(c=='>')out+="&gt;";
    else if(c=='"')out+="&quot;";
    else if(c=='\'')out+="&#39;";
    else out+=c;
  }
  return out;
}
static String httpUrlEncode(const String& input){
  const char* hex="0123456789ABCDEF";String out;
  for(unsigned char c:input){
    if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||
       (c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~'||c=='/')
      out+=(char)c;
    else {out+='%';out+=hex[c>>4];out+=hex[c&15];}
  }
  return out;
}
static bool httpAuth(){
  if(httpFiles.authenticate(httpUsername.c_str(),httpPassword.c_str()))return true;
  httpFiles.requestAuthentication();
  return false;
}
static String httpFolderArg(){
  String path=httpFiles.hasArg("dir")?httpFiles.arg("dir"):"/";
  return httpValidPath(path)?path:String("");
}
static void httpRedirect(const String& directory){
  httpFiles.sendHeader("Location","/?dir="+httpUrlEncode(directory),true);
  httpFiles.send(303,"text/plain","");
}
static String httpJsonQuote(const String& value){
  String out="\"";
  for(unsigned char c:value){
    if(c=='"'||c=='\\'){out+='\\';out+=(char)c;}
    else if(c=='\n')out+="\\n";
    else if(c=='\r')out+="\\r";
    else if(c<32){char temp[7];snprintf(temp,sizeof(temp),"\\u%04x",(unsigned)c);out+=temp;}
    else out+=(char)c;
  }
  return out+"\"";
}
static void httpTree(){
  if(!httpAuth())return;
  String dir=httpFolderArg();
  if(dir.isEmpty()){httpFiles.send(400,"application/json","[]");return;}
  File folder=SD.open(dir.c_str(),FILE_READ);
  if(!folder||!folder.isDirectory()){
    if(folder)folder.close();
    httpFiles.send(404,"application/json","[]");return;
  }
  String result="[";
  File item=folder.openNextFile();
  int count=0;
  while(item && count<128){
    if(item.isDirectory()){
      String name=String(item.name());
      int slash=name.lastIndexOf('/');if(slash>=0)name=name.substring(slash+1);
      if(httpValidName(name)){
        if(count++)result+=",";
        result+="{\"name\":"+httpJsonQuote(name)+",\"path\":"+httpJsonQuote(httpChild(dir,name))+"}";
      }
    }
    item.close();item=folder.openNextFile();
  }
  folder.close();
  result+="]";
  httpFiles.send(200,"application/json; charset=utf-8",result);
}
static const char* httpInlineType(const String& filename);
static void httpPage(){
  if(!httpAuth())return;
  ++httpRequests;
  String dir=httpFolderArg();
  if(dir.isEmpty()){httpFiles.send(400,"text/plain","Invalid path");return;}
  File folder=SD.open(dir.c_str(),FILE_READ);
  if(!folder||!folder.isDirectory()){
    if(folder)folder.close();
    httpFiles.send(404,"text/plain","Directory not found");return;
  }
  String html=R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>M5Knife File Server</title><style>
:root{color-scheme:dark}*{box-sizing:border-box}
body{margin:0;background:#0a1118;color:#e3ebed;font:14px system-ui,Arial,sans-serif}
header{height:65px;display:flex;align-items:center;gap:20px;padding:0 22px;border-bottom:1px solid #253743;background:#101d25}
.brand{color:#63e6a0;font-size:24px;font-weight:700}.sub{color:#9baeba;font-size:17px}
.layout{display:grid;grid-template-columns:265px minmax(0,1fr);min-height:calc(100vh - 65px)}
aside{background:#101b23;border-right:1px solid #253743;padding:18px 12px;overflow:auto}
main{padding:20px;min-width:0}.card{border:1px solid #293c48;border-radius:8px;background:#111e27}
.muted{color:#a2b3bd}.capacity{height:9px;background:#273944;border-radius:8px;margin:10px 0;overflow:hidden}
.capacity>div{background:#46c88b;height:100%}
.tree{margin:10px 0;padding:0;list-style:none}.tree ul{padding-left:18px;margin:3px 0;list-style:none}
.tree li{padding:2px 0}.node{display:flex;align-items:center;gap:5px}
.toggle{border:0;background:none;color:#a4b9c7;cursor:pointer;width:24px;padding:3px;font-size:13px}
.folder{color:#e7bb62}.tree a{padding:6px 5px;text-decoration:none;color:#d9e7e9;border-radius:4px;flex:1;overflow-wrap:anywhere}
.tree a.current{background:#184b3b;color:#78f0b1}
.path{padding:13px;margin-bottom:14px;color:#c5d9e0;overflow-wrap:anywhere}
a{color:#65e8aa}.toolbar{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:16px}
button,.btn,input{font:inherit;border:1px solid #415765;border-radius:5px;background:#1a2933;color:#e4eff1;padding:8px 11px}
button,.btn{cursor:pointer}.primary{background:#16864f;border-color:#29b977;color:white}
input{max-width:180px}.toolbar form{display:flex;align-items:center;gap:5px;flex-wrap:wrap}
table{width:100%;border-collapse:collapse}th,td{padding:12px;border-bottom:1px solid #263743;text-align:left}
th{background:#1b2a35;color:#b5c6cd}td.actions{min-width:190px}td.actions form{display:inline-flex;margin:2px;gap:4px;align-items:center}
td.actions input{width:112px}tbody tr:hover{background:#152a32}
.file{color:#e5edf0;text-decoration:none}.filename{overflow-wrap:anywhere}.tiny{font-size:12px}
@media(max-width:750px){.layout{grid-template-columns:1fr}aside{border-right:0;border-bottom:1px solid #253743}main{padding:12px}td.actions{min-width:0}table{font-size:12px}th,td{padding:7px}}
</style></head><body><header><div class="brand">M5Knife</div><span class="sub">HTTP File Server</span></header>
<div class="layout"><aside><b>▣ SD Card</b>)HTML";
  const uint64_t capacity=SD.cardSize(),used=SD.usedBytes();
  html+="<p class='muted'>"+String((uint32_t)(used/1048576ULL))+" / "+String((uint32_t)(capacity/1048576ULL))+" MiB used</p>";
  html+="<div class='capacity'><div style='width:"+String(capacity?(unsigned)(used*100/capacity):0)+"%'></div></div>";
  html+=R"HTML(<b>Folders</b><ul class="tree" id="folderTree"><li><div class="node"><button class="toggle" type="button" id="rootToggle" aria-label="Expand root">▸</button><span class="folder">📁</span><a href="/?dir=%2F" id="rootLink">SD card /</a></div><ul id="rootChildren" hidden></ul></li></ul>
<p class="muted tiny">Click a triangle to expand a branch; click a name to open it.</p></aside><main>)HTML";
  html+="<div class='card path'>▱ SD card "+httpEscape(dir)+"</div>";
  html+="<div class='toolbar'><form action='/upload?dir="+httpUrlEncode(dir)+
    "' method='post' enctype='multipart/form-data'><input type='file' name='file' required><button class='primary'>↑ Upload</button></form>";
  html+="<form action='/mkdir' method='post'><input type='hidden' name='dir' value='"+httpEscape(dir)+
    "'><input name='name' placeholder='New folder' maxlength='80' required><button>+ New folder</button></form>";
  html+="<a class='btn' href='/?dir="+httpUrlEncode(dir)+"'>↻ Refresh</a></div>";
  if(dir!="/"){
    int last=dir.lastIndexOf('/');String parent=last<=0?"/":dir.substring(0,last);
    html+="<p><a href='/?dir="+httpUrlEncode(parent)+"'>↑ Parent directory</a></p>";
  }
  html+="<div class='card'><table><thead><tr><th>Name</th><th>Size</th><th>Actions</th></tr></thead><tbody>";
  File item=folder.openNextFile();
  int count=0;
  while(item && count<256){
    String name=String(item.name());int slash=name.lastIndexOf('/');
    if(slash>=0)name=name.substring(slash+1);
    if(httpValidName(name)){
      ++count;
      String path=httpChild(dir,name),url=httpUrlEncode(path);
      html+="<tr><td class='filename'>";
      if(item.isDirectory())html+="📁 <a class='file' href='/?dir="+url+"'>"+httpEscape(name)+"</a>";
      else {
        const bool preview=httpInlineType(name)!=nullptr;
        html+="▤ <a class='file' href='/download?path="+url+(preview?"&view=1":"")+
          "'"+(preview?" target='_blank' rel='noopener'":"")+">"+httpEscape(name)+"</a>";
      }
      html+="</td><td class='muted'>";
      if(item.isDirectory())html+="—";
      else html+=String((uint32_t)item.size())+" B";
      html+="</td><td class='actions'>";
      if(!item.isDirectory())html+="<a href='/download?path="+url+"'>↓ Download</a> ";
      html+="<form action='/rename' method='post'><input type='hidden' name='path' value='"+
        httpEscape(path)+"'><input name='name' placeholder='Rename to...' required maxlength='80'><button>Rename</button></form>";
      html+="<form action='/delete' method='post' onsubmit='return confirm(\"Delete selected item?\")'><input type='hidden' name='path' value='"+
        httpEscape(path)+"'><button>Delete</button></form></td></tr>";
    }
    item.close();item=folder.openNextFile();
  }
  folder.close();
  html+="</tbody></table></div><p class='muted tiny'>"+String(count)+" items • Full file management available while server is running</p></main></div>";
  html+=R"HTML(<script>
const selected=new URLSearchParams(location.search).get('dir')||'/';
const root=document.getElementById('rootChildren');
const rootToggle=document.getElementById('rootToggle');
const rootLink=document.getElementById('rootLink');
if(selected==='/')rootLink.classList.add('current');
function node(label,path){
 const li=document.createElement('li'),div=document.createElement('div');
 div.className='node';
 const toggle=document.createElement('button');toggle.className='toggle';toggle.type='button';toggle.textContent='▸';
 const icon=document.createElement('span');icon.className='folder';icon.textContent='📁';
 const link=document.createElement('a');link.textContent=label;link.href='/?dir='+encodeURIComponent(path);
 if(path===selected)link.classList.add('current');
 const children=document.createElement('ul');children.hidden=true;
 toggle.onclick=()=>expand(path,children,toggle);
 div.append(toggle,icon,link);li.append(div,children);return {li,children,toggle};
}
async function expand(path,ul,button){
 if(!ul.hidden){ul.hidden=true;button.textContent='▸';return;}
 if(!ul.dataset.loaded){
  button.textContent='…';
  try{
   const response=await fetch('/tree?dir='+encodeURIComponent(path));
   if(!response.ok)throw Error('Cannot load folders');
   const folders=await response.json();
   for(const folder of folders){const item=node(folder.name,folder.path);ul.append(item.li);}
   ul.dataset.loaded='1';
  }catch(e){button.textContent='!';return;}
 }
 ul.hidden=false;button.textContent='▾';
}
(async()=>{
 await expand('/',root,rootToggle);
 let current='',parts=selected.split('/').filter(Boolean);
 for(const part of parts){
   current+='/'+part;
   const link=Array.from(document.querySelectorAll('.tree a')).find(a=>decodeURIComponent(new URL(a.href).searchParams.get('dir')||'/')===current);
   if(!link)break;
   const li=link.closest('li');
   const children=li.querySelector(':scope > ul'),button=li.querySelector(':scope > .node > .toggle');
   if(children&&button)await expand(current,children,button);
 }
})();
</script></body></html>)HTML";
  httpFiles.send(200,"text/html; charset=utf-8",html);
}
static const char* httpInlineType(const String& filename){
  String ext=filename.substring(filename.lastIndexOf('.')+1);
  ext.toLowerCase();
  if(ext=="txt"||ext=="log"||ext=="cfg"||ext=="ini"||ext=="md"||
     ext=="c"||ext=="cpp"||ext=="h"||ext=="hpp"||ext=="py"||ext=="bas"||
     ext=="asm"||ext=="mac"||ext=="lst")return "text/plain; charset=utf-8";
  if(ext=="csv")return "text/csv; charset=utf-8";
  if(ext=="json")return "application/json";
  if(ext=="png")return "image/png";
  if(ext=="jpg"||ext=="jpeg")return "image/jpeg";
  if(ext=="gif")return "image/gif";
  if(ext=="webp")return "image/webp";
  if(ext=="bmp")return "image/bmp";
  if(ext=="ico")return "image/x-icon";
  if(ext=="pdf")return "application/pdf";
  if(ext=="mp3")return "audio/mpeg";
  if(ext=="wav")return "audio/wav";
  if(ext=="ogg")return "audio/ogg";
  if(ext=="mp4")return "video/mp4";
  if(ext=="webm")return "video/webm";
  // HTML, SVG, JavaScript, and unknown files stay downloads rather than executing.
  return nullptr;
}
static void httpDownload(){
  if(!httpAuth())return;
  String path=httpFiles.arg("path");
  if(!httpValidPath(path)||path=="/"){httpFiles.send(400,"text/plain","Invalid path");return;}
  File f=SD.open(path.c_str(),FILE_READ);
  if(!f||f.isDirectory()){if(f)f.close();httpFiles.send(404,"text/plain","File not found");return;}
  String name=path.substring(path.lastIndexOf('/')+1);
  name.replace("\"","_");
  const char* mime=httpInlineType(name);
  const bool inlineView=httpFiles.arg("view")=="1" && mime;
  httpFiles.sendHeader("X-Content-Type-Options","nosniff");
  httpFiles.sendHeader("Content-Disposition",
    String(inlineView?"inline":"attachment")+"; filename=\""+name+"\"");
  httpFiles.streamFile(f,inlineView?mime:"application/octet-stream");
  f.close();
}
static void httpMkdir(){
  if(!httpAuth())return;
  String dir=httpFolderArg(),name=httpFiles.arg("name");
  if(dir.isEmpty()||!httpValidName(name)){httpFiles.send(400,"text/plain","Invalid folder");return;}
  String path=httpChild(dir,name);
  if(SD.exists(path.c_str())||!SD.mkdir(path.c_str())){httpFiles.send(409,"text/plain","Cannot create folder");return;}
  httpRedirect(dir);
}
static void httpRename(){
  if(!httpAuth())return;
  String oldPath=httpFiles.arg("path"),name=httpFiles.arg("name");
  if(!httpValidPath(oldPath)||oldPath=="/"||!httpValidName(name)){
    httpFiles.send(400,"text/plain","Invalid rename");return;
  }
  int last=oldPath.lastIndexOf('/');
  String parent=last<=0?"/":oldPath.substring(0,last);
  String dest=httpChild(parent,name);
  if(!SD.exists(oldPath.c_str())||SD.exists(dest.c_str())||
     !SD.rename(oldPath.c_str(),dest.c_str())){httpFiles.send(409,"text/plain","Rename failed");return;}
  httpRedirect(parent);
}
static void httpDelete(){
  if(!httpAuth())return;
  String path=httpFiles.arg("path");
  if(!httpValidPath(path)||path=="/"){httpFiles.send(400,"text/plain","Invalid path");return;}
  File f=SD.open(path.c_str(),FILE_READ);
  if(!f){httpFiles.send(404,"text/plain","Not found");return;}
  bool directory=f.isDirectory();f.close();
  bool ok=directory?SD.rmdir(path.c_str()):SD.remove(path.c_str());
  if(!ok){httpFiles.send(409,"text/plain","Delete failed (folder must be empty)");return;}
  int last=path.lastIndexOf('/');
  httpRedirect(last<=0?"/":path.substring(0,last));
}
static void httpUploadData(){
  if(!httpFiles.authenticate(httpUsername.c_str(),httpPassword.c_str())){httpUploadFailed=true;return;}
  HTTPUpload& upload=httpFiles.upload();
  if(upload.status==UPLOAD_FILE_START){
    httpUploadPath="";
    httpUploadFailed=true;
    String dir=httpFolderArg();
    String name=upload.filename;
    // Reject directory separators rather than trusting multipart filenames.
    if(dir.isEmpty()||!httpValidName(name))return;
    String path=httpChild(dir,name);
    if(SD.exists(path.c_str()))return; // No silent overwrites.
    httpUploadPath=path;
    httpUploadFile=SD.open(path.c_str(),FILE_WRITE);
    httpUploadFailed=!httpUploadFile;
  }else if(upload.status==UPLOAD_FILE_WRITE){
    if(!httpUploadFailed && httpUploadFile &&
       httpUploadFile.write(upload.buf,upload.currentSize)!=upload.currentSize)
       httpUploadFailed=true;
  }else if(upload.status==UPLOAD_FILE_END || upload.status==UPLOAD_FILE_ABORTED){
    if(httpUploadFile)httpUploadFile.close();
    if(httpUploadFailed||upload.status==UPLOAD_FILE_ABORTED){
      if(httpUploadPath.length())SD.remove(httpUploadPath.c_str());
      httpUploadFailed=true;
    }
  }
}
static void httpUploadDone(){
  if(!httpAuth())return;
  if(httpUploadFailed){httpFiles.send(409,"text/plain","Upload failed or file exists");return;}
  String dir=httpFolderArg();httpRedirect(dir.length()?dir:"/");
}
static void startHttpFiles(bool forceHotspot){
  if(usbActive || (!sdReady && !mountSd())){showMessage("SD unavailable",Screen::Network);return;}
  // Keep the existing router-connected mode. Otherwise create a private Wi-Fi
  // access point for direct phone/computer access to the HTTP file manager.
  if(forceHotspot || WiFi.status()!=WL_CONNECTED){
    WiFi.disconnect(false,false);
    WiFi.mode(WIFI_AP);
    if(!WiFi.softAP(hotspotSsid.c_str(),hotspotPassword.c_str())){
      WiFi.mode(WIFI_STA);
      autoConnectWifi();
      showMessage("Hotspot start failed",Screen::Network);
      return;
    }
    httpHotspotActive=true;
  }else httpHotspotActive=false;
  // Credentials loaded from NVS [HTTPD], otherwise defaults are used.
  if(httpPassword.isEmpty())httpPassword="1234";
  httpRequests=0;httpUploadFailed=false;httpUploadPath="";
  if(!httpRoutesRegistered){
  httpFiles.on("/",HTTP_GET,httpPage);
  httpFiles.on("/tree",HTTP_GET,httpTree);
  httpFiles.on("/download",HTTP_GET,httpDownload);
  httpFiles.on("/mkdir",HTTP_POST,httpMkdir);
  httpFiles.on("/rename",HTTP_POST,httpRename);
  httpFiles.on("/delete",HTTP_POST,httpDelete);
  httpFiles.on("/upload",HTTP_POST,httpUploadDone,httpUploadData);
  httpFiles.onNotFound([](){httpFiles.send(404,"text/plain","Not found");});
  httpRoutesRegistered=true;
  }
  httpFiles.begin();
  httpRunning=true;
  httpQrMode=0;
  screen=Screen::HttpFiles;draw();
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
  header(action==Action::Mkdir?"NEW FOLDER":action==Action::NewText?"NEW TEXT FILE":"RENAME");
  line(32, action==Action::Mkdir?"Folder name:":action==Action::NewText?"File name:":"New name:", false, ACCENT);
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
static String textBuffer;
static int textTop=0, textCursor=0;
static bool textEdit=false;
static bool textDirty=false;
static String textFind;
static bool loadText(){
  File f=SD.open(selectedPath.c_str(),FILE_READ);
  if(!f)return false;
  if(f.size()>8192){f.close();return false;}
  textBuffer="";
  while(f.available()){char c=f.read();if(c!='\r')textBuffer+=c;}
  f.close();textTop=0;textCursor=0;textEdit=false;textDirty=false;
  return true;
}
static void drawViewer(){
  header(textEdit?"TEXT EDITOR":"TEXT VIEWER");
  int cursorLine=0,cursorCol=0;
  if(textEdit)for(int i=0;i<textCursor && i<(int)textBuffer.length();++i){
    if(textBuffer[i]=='\n'){++cursorLine;cursorCol=0;}else ++cursorCol;
  }
  int offset=0,number=0,y=24;
  // Include the final empty line after a trailing newline, so the caret
  // remains visible when Enter is pressed at the end of a file.
  while(offset<=(int)textBuffer.length() && y<=108){
    int next=textBuffer.indexOf('\n',offset);
    if(next<0)next=textBuffer.length();
    String row=textBuffer.substring(offset,next);
    if(number>=textTop){
      const bool here=textEdit && cursorLine==number;
      const int start=(here && cursorCol>=29)?cursorCol-28:0;
      const String prefix=String(number+1)+": ";
      String visible=row.substring(start,start+29);
      line(y,prefix+visible,here);
      if(here){
        auto& d=M5Cardputer.Display;
        // Font0 uses 6px character cells at text size 1.
        int column=cursorCol-start;
        int x=7+(prefix.length()+column)*6;
        if(x>234)x=234;
        d.fillRect(x,y+10,5,2,ACCENT);
      }
      y+=14;
    }
    if(next==(int)textBuffer.length())break;
    offset=next+1;++number;
  }
  footer(textEdit?"Fn+arrows move Fn+S save Esc exit":"W/S scroll F find E edit B back");
}
static bool saveText(){
  if(!sdReady||usbActive)return false;
  String temporary=selectedPath+".tmp",backup=selectedPath+".bak";
  SD.remove(temporary.c_str());
  File f=SD.open(temporary.c_str(),FILE_WRITE);
  if(!f)return false;
  size_t written=f.print(textBuffer);f.flush();f.close();
  if(written!=textBuffer.length()){SD.remove(temporary.c_str());return false;}
  SD.remove(backup.c_str());
  if(!SD.rename(selectedPath.c_str(),backup.c_str())){SD.remove(temporary.c_str());return false;}
  if(!SD.rename(temporary.c_str(),selectedPath.c_str())){
    SD.rename(backup.c_str(),selectedPath.c_str());return false;
  }
  SD.remove(backup.c_str());textDirty=false;return true;
}
static void drawEditorSearch(){
  header("FIND TEXT");line(30,"Find:",false,ACCENT);line(55,textFind);
  footer("Enter search  Esc cancel");
}
static void draw() {
  switch(screen) {
    case Screen::Home: drawHome(); break;
    case Screen::Files: drawFiles(); break;
    case Screen::Viewer: drawViewer(); break;
    case Screen::EditorSearch: drawEditorSearch(); break;
    case Screen::Disk: drawDisk(); break;
    case Screen::Usb: drawUsb(); break;
    case Screen::Input: drawInput(); break;
    case Screen::Confirm: drawConfirm(); break;
    case Screen::Message: drawMessage(); break;
    case Screen::Wifi: drawScan(true); break;
    case Screen::Network: drawNetworkMenu(); break;
    case Screen::HttpFiles: drawHttpFiles(); break;
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
    case Screen::Settings: drawSettings(); break;
    case Screen::WifiPassword: drawWifiPassword(); break;
    case Screen::WifiConnecting: drawWifiConnecting(); break;
  }
}
static void goHome() { if(httpRunning)stopHttpFiles(); if(screen==Screen::WifiConnecting) WiFi.disconnect(false,false); wifiPassword=""; screen=Screen::Home; action=Action::None; draw(); }
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
  if (action==Action::NewText) {
    File file=SD.open(to.c_str(),FILE_WRITE);
    if(file){file.close();ok=true;}
  }
  if (action==Action::Rename) ok=SD.rename(selectedPath.c_str(),to.c_str());
  action=Action::None; scanDir(); screen=Screen::Files; draw();
  if (!ok) showMessage("SD operation failed.");
}
static void activateFile() {
  if (selected>=entries.size()) return;
  const auto& e=entries[selected];
  selectedPath=childPath(e.name);
  if (e.dir) { cwd=selectedPath; scanDir(); draw(); }
  else {if(!loadText()){showMessage("File too large or unreadable",Screen::Files);return;}screen=Screen::Viewer;draw();}
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
    if(right && homeIndex < 8) ++homeIndex;
    if(k.enter) {
      // Explicit dispatch for all eight launcher tiles.
      switch (homeIndex) {
        case 0: openFiles(); break;
        case 1: startUsb(); break;
        case 2: scanWifi(); break;
        case 3:
          screen=Screen::Network;netMenuIndex=0;draw();
          break;
        case 4: scanBluetooth(); break;
        case 5:
          screen = Screen::Infrared;
          draw();
          break;
        case 6: screen=Screen::Settings;settingsIndex=0;drawSettings();break;
        case 7:
          screen = Screen::Status;
          statusScroll=0;
          drawStatus();
          break;
        case 8: screen=Screen::About;aboutScroll=0;drawAbout();break;
      }
      return;
    }
  } else if(screen==Screen::Settings) {
    if(escape||backspace||typed(k,'b')){goHome();return;}
    if(up && settingsIndex>0)--settingsIndex;
    if(down && settingsIndex<8)++settingsIndex;
    if(left)settingsActivate(-1);
    else if(right || k.enter)settingsActivate(1);
    else drawSettings();
    return;
  } else if(screen==Screen::About) {
    if(escape||backspace||typed(k,'b')){goHome();return;}
    if(up && aboutScroll>0)--aboutScroll;
    if(down && aboutScroll<13)++aboutScroll;
    drawAbout();return;
  } else if(screen==Screen::Network) {
    if(escape||backspace||typed(k,'b')){goHome();return;}
    if(up && netMenuIndex>0)--netMenuIndex;
    if(down && netMenuIndex<6)++netMenuIndex;
    if(k.enter){
      if(netMenuIndex==0){screen=Screen::NetInfo;draw();return;}
      if(netMenuIndex==5||netMenuIndex==6){startHttpFiles(netMenuIndex==6);return;}
      if(WiFi.status()!=WL_CONNECTED){
        showMessage("Connect to WiFi first",Screen::Network);return;
      }
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
  } else if(screen==Screen::HttpFiles){
    if(escape||backspace||typed(k,'b')){stopHttpFiles();screen=Screen::Network;draw();return;}
    if(typed(k,'q')||k.enter){
      if(httpHotspotActive)httpQrMode=(httpQrMode+1)%3;
      else httpQrMode=httpQrMode==0?2:0;
      drawHttpFiles();return;
    }
    if(typed(k,'r'))drawHttpFiles();
    return;
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
      showMessage("IR TX: GPIO44\\nProtocols: NEC, Samsung\\nProfiles: /M5KNIFE.CFG\\nRX requires extra hardware",Screen::Infrared);return;
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
      else irNotice="Load M5KNIFE.CFG / code unset";
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
    if(down && statusScroll<10)++statusScroll;
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
    if(typed(k,'t') && sdReady) {action=Action::NewText;inputText="";screen=Screen::Input;draw();return;}
    if(typed(k,'e') && selected<entries.size()) {action=Action::Rename;selectedPath=childPath(entries[selected].name);inputText=entries[selected].name;screen=Screen::Input;draw();return;}
    if(typed(k,'x') && selected<entries.size()) {action=Action::Delete;selectedPath=childPath(entries[selected].name);screen=Screen::Confirm;draw();return;}
  } else if(screen==Screen::Viewer) {
    if(escape || (k.fn && typed(k,'b'))){
      if(textEdit && textDirty){showMessage("Unsaved changes: Fn+S save",Screen::Viewer);return;}
      textEdit=false;screen=Screen::Files;draw();return;
    }
    if(k.fn && typed(k,'s') && textEdit){showMessage(saveText()?"Saved":"Save failed",Screen::Viewer);return;}
    if(!textEdit && typed(k,'e')){textEdit=true;textCursor=0;draw();return;}
    if(!textEdit && typed(k,'f')){textFind="";screen=Screen::EditorSearch;draw();return;}
    if(textEdit){
      if(left && textCursor>0)--textCursor;
      else if(right && textCursor<(int)textBuffer.length())++textCursor;
      else if(up || down){
        int lineStart=textBuffer.lastIndexOf('\n',textCursor-1)+1;
        int column=textCursor-lineStart;
        if(up && lineStart>0){
          int previousEnd=lineStart-1;
          int previousStart=textBuffer.lastIndexOf('\n',previousEnd-1)+1;
          textCursor=previousStart+std::min(column,previousEnd-previousStart);
        }else if(down){
          int next=textBuffer.indexOf('\n',textCursor);
          if(next>=0){
            int nextStart=next+1;
            int nextEnd=textBuffer.indexOf('\n',nextStart);
            if(nextEnd<0)nextEnd=textBuffer.length();
            textCursor=nextStart+std::min(column,nextEnd-nextStart);
          }
        }
      }else if(k.del && textCursor>0){textBuffer.remove(--textCursor,1);textDirty=true;}
      else if(k.enter && textBuffer.length()<8192){
        textBuffer=textBuffer.substring(0,textCursor)+"\n"+textBuffer.substring(textCursor);
        ++textCursor;textDirty=true;
      }else if(!k.fn)for(char c:k.word)if(c>=32&&c<=126&&textBuffer.length()<8192){
        textBuffer=textBuffer.substring(0,textCursor)+String(c)+textBuffer.substring(textCursor);
        ++textCursor;textDirty=true;
      }
      int cursorLine=0;
      for(int i=0;i<textCursor;++i)if(textBuffer[i]=='\n')++cursorLine;
      if(cursorLine<textTop)textTop=cursorLine;
      if(cursorLine>=textTop+6)textTop=cursorLine-5;
    } else {
      if(up && textTop>0)--textTop;
      if(down && textTop<1000)++textTop;
    }
    draw();return;
  } else if(screen==Screen::EditorSearch) {
    if(escape){screen=Screen::Viewer;draw();return;}
    if(k.enter){
      int pos=textBuffer.indexOf(textFind);
      if(pos>=0 && textFind.length()){
        textTop=0;
        for(int i=0;i<pos;++i)if(textBuffer[i]=='\n')++textTop;
      }
      screen=Screen::Viewer;draw();return;
    }
    if(k.del&&textFind.length())textFind.remove(textFind.length()-1);
    else if(!k.fn)for(char c:k.word)if(c>=32&&c<=126&&textFind.length()<40)textFind+=c;
    draw();return;
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
  Preferences nvsProbe;nvsReady=nvsProbe.begin("m5knife",false);if(nvsReady)nvsProbe.end();
  if(!configLoadNvs(activeConfig)){
    if(!sdReady||!configImportSd()){
      configApply("[SETTINGS]\nAUTO_WIFI=1\nNTP=1\nBRIGHTNESS=128\nSLEEP_MIN=0\nTZ_HOURS=0\n[WIFI]\n[REMOTE]\n[HTTPD]\nUSERNAME=m5knife\nPASSWORD=1234\n");
    }
  }
  readSettings();applySettings();autoConnectWifi();
  lastInteraction=millis();
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
  startNtp();
  if(httpRunning){
    const wifi_mode_t mode=WiFi.getMode();
    const bool connectionLost=httpHotspotActive?
      (mode!=WIFI_AP && mode!=WIFI_AP_STA):
      WiFi.status()!=WL_CONNECTED;
    if(connectionLost){stopHttpFiles();showMessage("HTTP server stopped: WiFi lost",Screen::Home);}
    else httpFiles.handleClient();
  }
  if(sleepMinutes>0 && (uint32_t)(millis()-lastInteraction)>(uint32_t)sleepMinutes*60000UL)
    M5Cardputer.Display.setBrightness(0);
  // G0 is globally handled BEFORE all tool-specific keyboard processing.
  if(M5Cardputer.BtnA.wasPressed()) {
    if (usbActive) {
      if ((!usbConnected || usbEjected) && !usbWriting) {stopUsb();goHome();}
      else drawUsb();
    } else goHome();
    lastInteraction=millis();applySettings();waitingKeyRelease=true;
  }
  if(M5Cardputer.BtnA.wasReleased()) waitingKeyRelease=false;
  if(waitingKeyRelease) {delay(5);return;}
  if(M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    lastInteraction=millis();applySettings();
    handleKeys(M5Cardputer.Keyboard.keysState());
  }
  if(screen==Screen::WifiConnecting) {
    wl_status_t state=WiFi.status();
    if(state==WL_CONNECTED) showWifiConnectionResult(true);
    else if((uint32_t)(millis()-wifiConnectStarted)>=WIFI_CONNECT_TIMEOUT_MS) {
      WiFi.disconnect(false,false);
      showWifiConnectionResult(false);
    }
  }
  if((screen==Screen::NetInput || screen==Screen::NetResult) &&
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
