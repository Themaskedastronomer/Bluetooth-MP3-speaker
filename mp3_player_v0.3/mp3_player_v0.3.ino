/* MADE USING AI ASSISTED CODE
 * ESP32 MP3 Player v3.0
 * Landscape 320x240 — True Dark Mode
 *
 * TFT ST7789  : MOSI=23, SCLK=18, CS=15, DC=2, RST=4, BL=21
 * SD Card     : MOSI=23, SCLK=18, MISO=19, CS=5
 * MAX98357A   : BCLK=26, LRC=25, DIN=22, SD_MODE=16, GAIN=GND
 * Buttons     : UP=32, DOWN=33, LEFT=34, RIGHT=27, DISPLAY=13, OK=14
 *
 * Libraries:
 *   Adafruit ST7735 and ST7789
 *   Adafruit GFX
 *   ESP32-audioI2S (schreibfaul1 v1.0)
 *   ESP32-A2DP (pschatzmann)
 *   JPEGDEC (Larry Bank)
 *   SD (built-in esp32)
 *
 * Core: 2.0.17 | Partition: Huge APP 3MB
 *
 * v3.0 changes:
 *   - Swapped UDA1334A + PAM8406 for MAX98357A (I2S direct, 5V, 3W/4Ω)
 *   - SD_MODE pin (GPIO16): hardware mute when idle, eliminates hiss
 *   - Default startup volume changed to 13
 */

#include <SPI.h>
#include <SD.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Audio.h>
#include <BluetoothA2DPSink.h>
#include <JPEGDEC.h>
JPEGDEC jpeg;

// ── Pins ──────────────────────────────────────────────────────────────────────
#define TFT_MOSI  23
#define TFT_SCLK  18
#define TFT_CS    15
#define TFT_DC     2
#define TFT_RST    4
#define TFT_BL    21
#define SD_CS      5
#define SD_MISO   19
#define I2S_BCLK  26
#define I2S_WSEL  25
#define I2S_DOUT  22
#define SD_MODE   16   // MAX98357A hardware mute: HIGH=on, LOW=mute
#define BTN_UP     32
#define BTN_DOWN   33
#define BTN_LEFT   34
#define BTN_RIGHT  27
#define BTN_DISP   13
#define BTN_OK     14

// ── Screen ────────────────────────────────────────────────────────────────────
#define SW 320
#define SH 240
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// ── Audio / BT ────────────────────────────────────────────────────────────────
Audio audio;
BluetoothA2DPSink a2dp;

// ── TRUE Dark Mode Colors ─────────────────────────────────────────────────────
#define BLACK      0xFFFF
#define DARKPANEL  0xEF7D
#define WHITE      0x0000
#define CYAN       0xF800
#define YELLOW     0x001F
#define GREEN      0xF81F
#define RED        0x07FF
#define GRAY       0x7BEF
#define DIMGRAY    0xBDF7
#define DARKGRAY   0xDEFB

// ── State ─────────────────────────────────────────────────────────────────────
enum Mode    { MENU, SETTINGS, MP3, BLUETOOTH };
enum SetPage { S_MENU, S_SPECS, S_BRIGHT, S_VOL, S_BTHIST };

Mode    mode       = MENU;
SetPage setPage    = S_MENU;
int     menuIdx    = 0;
int     setIdx     = 0;
int     bright     = 80;
int     vol        = 13;
bool    playing    = false;
bool    playerView = false;
bool    looping    = false;
bool    dispOn     = true;
bool    btActive   = false;
bool    btConn     = false;
String  btName     = "";

// MP3
String songs[50];
int    songCount   = 0;
int    songIdx     = 0;
int    songScroll  = 0;

// BT History
String btHist[10];
int    btHistCount = 0;

// Album art
bool   artLoaded   = false;

// Fade
bool   fadeActive  = false;
int    fadeVol     = 0;
bool   fadeIn      = false;
unsigned long fadeTime = 0;
#define FADE_STEP_MS 20
#define FADE_STEPS   10

// ── Buttons ───────────────────────────────────────────────────────────────────
#define NBTNS 6
#define DEB   220
#define HOLD  700
const int BPINS[NBTNS] = {BTN_UP,BTN_DOWN,BTN_LEFT,BTN_RIGHT,BTN_DISP,BTN_OK};
unsigned long bTime[NBTNS]={0}, hTime[NBTNS]={0};
bool bLow[NBTNS]={false}, bHeld[NBTNS]={false};

int bIdx(int p){ for(int i=0;i<NBTNS;i++) if(BPINS[i]==p) return i; return 0; }

bool pressed(int p){
  int i=bIdx(p); bool lo=(digitalRead(p)==LOW);
  if(lo&&!bLow[i]&&millis()-bTime[i]>DEB){ bTime[i]=millis(); bLow[i]=true; return true; }
  if(!lo) bLow[i]=false;
  return false;
}

bool held(int p){
  int i=bIdx(p);
  if(digitalRead(p)==LOW){
    if(!hTime[i]) hTime[i]=millis();
    if(millis()-hTime[i]>HOLD&&!bHeld[i]){ bHeld[i]=true; return true; }
  } else { hTime[i]=0; bHeld[i]=false; }
  return false;
}

// ── Draw Helpers ──────────────────────────────────────────────────────────────
void hdr(const char* t){
  tft.fillRect(0,0,SW,26,CYAN);
  tft.setTextColor(BLACK); tft.setTextSize(2);
  int x=(SW-strlen(t)*12)/2;
  tft.setCursor(max(8,x),4); tft.print(t);
}

void ftr(const char* t){
  tft.fillRect(0,SH-16,SW,16,DARKGRAY);
  tft.setTextColor(DIMGRAY); tft.setTextSize(1);
  tft.setCursor(4,SH-10); tft.print(t);
}

void divl(int y){ tft.drawFastHLine(0,y,SW,DARKGRAY); }

// ── Album Art (JPEGDEC) ───────────────────────────────────────────────────────
// Art box: x=8, y=28, size=184x184
#define ART_X 8
#define ART_Y 28
#define ART_SIZE 184

// JPEGDEC draw callback — blits decoded MCU blocks to TFT
int jpegDrawCallback(JPEGDRAW *pDraw){
  uint16_t* pixels = pDraw->pPixels;
  int count = pDraw->iWidth * pDraw->iHeight;
  for(int i = 0; i < count; i++){
    uint16_t p = (pixels[i] >> 8) | (pixels[i] << 8);
    pixels[i] = ~p;
  }
  tft.drawRGBBitmap(ART_X + pDraw->x, ART_Y + pDraw->y,
                    pixels, pDraw->iWidth, pDraw->iHeight);
  return 1;
}

void drawAlbumArt(int idx){
  artLoaded = false;
  String songName = songs[idx];
  String jpgPath = "/" + songName.substring(0, songName.lastIndexOf('.')) + ".jpg";

  auto drawPlaceholder = [](){
    tft.fillRoundRect(ART_X, ART_Y, ART_SIZE, ART_SIZE, 6, DARKPANEL);
    tft.drawRoundRect(ART_X, ART_Y, ART_SIZE, ART_SIZE, 6, DARKGRAY);
    tft.setTextColor(DIMGRAY); tft.setTextSize(4);
    tft.setCursor(ART_X + ART_SIZE/2 - 16, ART_Y + ART_SIZE/2 - 20);
    tft.print("P");
  };

  if(!SD.exists(jpgPath)){ drawPlaceholder(); return; }

  File jpgFile = SD.open(jpgPath);
  if(!jpgFile){ drawPlaceholder(); return; }

  size_t fsize = jpgFile.size();
  uint8_t* buf = (uint8_t*)malloc(fsize);
  if(!buf){ jpgFile.close(); drawPlaceholder(); return; }
  jpgFile.read(buf, fsize);
  jpgFile.close();

  if(jpeg.openRAM(buf, fsize, jpegDrawCallback)){
    // Scale down to fit 184x184 box
    int w = jpeg.getWidth(), h = jpeg.getHeight();
    int scale = 0; // 0=full, 1=half, 2=quarter, 3=eighth
    if(w > 368 || h > 368) scale = 2;
    else if(w > 184 || h > 184) scale = 1;
    jpeg.setPixelType(RGB565_BIG_ENDIAN);
    jpeg.decode(0, 0, scale);
    jpeg.close();
    artLoaded = true;
  } else {
    drawPlaceholder();
  }
  free(buf);
}

// ── Fade Helpers ──────────────────────────────────────────────────────────────
void startFadeIn(){
  fadeIn = true; fadeActive = true;
  fadeVol = 0; audio.setVolume(0);
  fadeTime = millis();
}

void startFadeOut(){
  fadeIn = false; fadeActive = true;
  fadeVol = vol; audio.setVolume(vol);
  fadeTime = millis();
}

void updateFade(){
  if(!fadeActive) return;
  if(millis() - fadeTime < FADE_STEP_MS) return;
  fadeTime = millis();
  if(fadeIn){
    fadeVol++;
    audio.setVolume(fadeVol);
    if(fadeVol >= vol){ audio.setVolume(vol); fadeActive = false; }
  } else {
    fadeVol--;
    audio.setVolume(fadeVol < 0 ? 0 : fadeVol);
    if(fadeVol <= 0){ audio.setVolume(0); fadeActive = false; }
  }
}

// ── Idle I2S Mute ─────────────────────────────────────────────────────────────
// SD_MODE LOW = hardware mute (kills hiss), HIGH = amp on
void muteI2S(){
  digitalWrite(SD_MODE, LOW);
  if(playing || playerView) audio.setVolume(0);
}

void unmuteI2S(){
  digitalWrite(SD_MODE, HIGH);
  audio.setVolume(vol);
}

// ── MENU ──────────────────────────────────────────────────────────────────────
void drawMenu(){
  tft.fillScreen(BLACK);
  hdr("MP3 PLAYER");
  const char* lb[]={"Settings","MP3","Bluetooth"};
  uint16_t    co[]={YELLOW,GREEN,CYAN};
  const char* ic[]={"*","P","B"};
  int cw=88,ch=140,cy=36,gap=8;
  int sx=(SW-(3*cw+2*gap))/2;
  for(int i=0;i<3;i++){
    int x=sx+i*(cw+gap);
    if(i==menuIdx){
      tft.fillRoundRect(x,cy,cw,ch,8,co[i]);
      tft.setTextColor(BLACK);
    } else {
      tft.fillRoundRect(x,cy,cw,ch,8,DARKPANEL);
      tft.drawRoundRect(x,cy,cw,ch,8,co[i]);
      tft.setTextColor(co[i]);
    }
    tft.setTextSize(3);
    tft.setCursor(x+cw/2-12,cy+28); tft.print(ic[i]);
    tft.setTextSize(1);
    tft.setCursor(x+(cw-strlen(lb[i])*6)/2,cy+ch-18); tft.print(lb[i]);
  }
  ftr("LEFT/RIGHT:select  OK:enter");
}

void menuInput(){
  if(pressed(BTN_LEFT))  { menuIdx=(menuIdx-1+3)%3; drawMenu(); }
  if(pressed(BTN_RIGHT)) { menuIdx=(menuIdx+1)%3;   drawMenu(); }
  if(pressed(BTN_UP))    { menuIdx=(menuIdx-1+3)%3; drawMenu(); }
  if(pressed(BTN_DOWN))  { menuIdx=(menuIdx+1)%3;   drawMenu(); }
  if(pressed(BTN_DISP))  { dispOn=!dispOn; ledcWrite(0, dispOn ? map(bright, 0, 100, 0, 255) : 0); }
  if(pressed(BTN_OK)){
    if(menuIdx==0){ mode=SETTINGS; setPage=S_MENU; setIdx=0; drawSetMenu(); }
    if(menuIdx==1){ stopBT(); mode=MP3; initMP3(); }
    if(menuIdx==2){ stopMP3(); mode=BLUETOOTH; initBT(); }
  }
}

// ── SETTINGS ─────────────────────────────────────────────────────────────────
void drawSetMenu(){
  tft.fillScreen(BLACK);
  hdr("SETTINGS");
  const char* it[]={"Device Specs","Brightness","Volume","BT History"};
  uint16_t    co[]={CYAN,YELLOW,GREEN,RED};
  for(int i=0;i<4;i++){
    int y=30+i*46;
    if(i==setIdx){
      tft.fillRoundRect(8,y,SW-16,38,6,co[i]);
      tft.setTextColor(BLACK);
    } else {
      tft.fillRoundRect(8,y,SW-16,38,6,DARKPANEL);
      tft.drawRoundRect(8,y,SW-16,38,6,co[i]);
      tft.setTextColor(co[i]);
    }
    tft.setTextSize(2); tft.setCursor(20,y+10); tft.print(it[i]);
  }
  ftr("UP/DOWN:nav  OK:open  RIGHT:back");
}

void drawSpecs(){
  tft.fillScreen(BLACK); hdr("DEVICE SPECS");
  esp_chip_info_t chip; esp_chip_info(&chip);
  uint32_t freq=getCpuFrequencyMhz();
  uint32_t fh=ESP.getFreeHeap()/1024, th=ESP.getHeapSize()/1024;
  float tmp=temperatureRead();
  uint32_t sdt=0,sdu=0;
  if(SD.begin(SD_CS)){ sdt=SD.totalBytes()/(1024*1024); sdu=SD.usedBytes()/(1024*1024); }
  struct R{ const char* l; String v; };
  R rows[]={
    {"CPU",    String(freq)+" MHz"},
    {"Chip",   "rev "+String(chip.revision)},
    {"Temp",   String(tmp,1)+" C"},
    {"RAM",    String(fh)+"/"+String(th)+" KB"},
    {"SD",     String(sdt)+" MB total"},
    {"SD Used",String(sdu)+" MB"},
    {"Bright", String(bright)+"%"},
    {"Volume", String(vol)+"/21"},
  };
  int y=30;
  for(auto& r:rows){
    tft.setTextColor(DIMGRAY); tft.setTextSize(1); tft.setCursor(8,y); tft.print(r.l);
    tft.setTextColor(WHITE);                        tft.setCursor(120,y); tft.print(r.v);
    y+=24; divl(y-4);
  }
  ftr("RIGHT: back");
}

void drawBright(){
  tft.fillScreen(BLACK); hdr("BRIGHTNESS");
  tft.setTextColor(WHITE); tft.setTextSize(6);
  char b[8]; sprintf(b,"%d%%",bright);
  tft.setCursor((SW-strlen(b)*36)/2,65); tft.print(b);
  int bx=20,by=155,bw=SW-40,bh=20;
  tft.drawRoundRect(bx,by,bw,bh,5,DIMGRAY);
  tft.fillRoundRect(bx+2,by+2,(bw-4)*bright/100,bh-4,4,CYAN);
  tft.setTextColor(DIMGRAY); tft.setTextSize(1);
  tft.setCursor(bx,by+bh+6); tft.print("0");
  tft.setCursor(bx+bw-18,by+bh+6); tft.print("100");
  ftr("UP:+1  DOWN:-1  HOLD UP/DN:+10/-10  RIGHT:back");
}

void drawVol(){
  tft.fillScreen(BLACK); hdr("VOLUME");
  tft.setTextColor(WHITE); tft.setTextSize(6);
  char b[8]; sprintf(b,"%d",vol);
  tft.setCursor((SW-strlen(b)*36)/2,65); tft.print(b);
  tft.setTextColor(DIMGRAY); tft.setTextSize(1);
  tft.setCursor(SW/2-10,130); tft.print("0 - 21");
  int bx=20,by=155,bw=SW-40,bh=20;
  tft.drawRoundRect(bx,by,bw,bh,5,DIMGRAY);
  tft.fillRoundRect(bx+2,by+2,(bw-4)*vol/21,bh-4,4,GREEN);
  tft.setTextColor(DIMGRAY); tft.setTextSize(1);
  tft.setCursor(bx,by+bh+6); tft.print("0");
  tft.setCursor(bx+bw-12,by+bh+6); tft.print("21");
  ftr("UP:+1  DOWN:-1  HOLD UP/DN:+5/-5  RIGHT:back");
}

void drawBTHist(){
  tft.fillScreen(BLACK); hdr("BT HISTORY");
  if(btHistCount==0){
    tft.setTextColor(DIMGRAY); tft.setTextSize(1);
    tft.setCursor(90,110); tft.print("No devices yet");
  } else {
    for(int i=0;i<btHistCount&&i<8;i++){
      int y=30+i*24;
      tft.setTextColor(WHITE); tft.setTextSize(1);
      tft.setCursor(8,y+6); tft.print(String(i+1)+". "+btHist[i]);
      divl(y+22);
    }
  }
  ftr("RIGHT: back");
}

void setInput(){
  if(pressed(BTN_DISP)){ dispOn=!dispOn; ledcWrite(0, dispOn ? map(bright, 0, 100, 0, 255) : 0); return; }

  if(setPage==S_SPECS||setPage==S_BTHIST){
    if(pressed(BTN_RIGHT)){ setPage=S_MENU; drawSetMenu(); }
    return;
  }
  if(setPage==S_BRIGHT){
    if(held(BTN_UP))      { bright=min(100,bright+10); ledcWrite(0,map(bright,0,100,0,255)); drawBright(); return; }
    if(held(BTN_DOWN))    { bright=max(0,bright-10);   ledcWrite(0,map(bright,0,100,0,255)); drawBright(); return; }
    if(pressed(BTN_UP))   { bright=min(100,bright+1);  ledcWrite(0,map(bright,0,100,0,255)); drawBright(); return; }
    if(pressed(BTN_DOWN)) { bright=max(0,bright-1);    ledcWrite(0,map(bright,0,100,0,255)); drawBright(); return; }
    if(pressed(BTN_RIGHT)){ setPage=S_MENU; drawSetMenu(); }
    return;
  }
  if(setPage==S_VOL){
    if(held(BTN_UP))      { vol=min(21,vol+5); audio.setVolume(vol); drawVol(); return; }
    if(held(BTN_DOWN))    { vol=max(0,vol-5);  audio.setVolume(vol); drawVol(); return; }
    if(pressed(BTN_UP))   { vol=min(21,vol+1); audio.setVolume(vol); drawVol(); return; }
    if(pressed(BTN_DOWN)) { vol=max(0,vol-1);  audio.setVolume(vol); drawVol(); return; }
    if(pressed(BTN_RIGHT)){ setPage=S_MENU; drawSetMenu(); }
    return;
  }
  // S_MENU
  if(pressed(BTN_UP))    { setIdx=(setIdx-1+4)%4; drawSetMenu(); }
  if(pressed(BTN_DOWN))  { setIdx=(setIdx+1)%4;   drawSetMenu(); }
  if(pressed(BTN_OK)){
    if(setIdx==0){ setPage=S_SPECS;  drawSpecs();  }
    if(setIdx==1){ setPage=S_BRIGHT; drawBright(); }
    if(setIdx==2){ setPage=S_VOL;    drawVol();    }
    if(setIdx==3){ setPage=S_BTHIST; drawBTHist(); }
  }
  if(pressed(BTN_RIGHT)){ mode=MENU; drawMenu(); }
}

// ── MP3 ───────────────────────────────────────────────────────────────────────
void stopMP3(){
  if(!playing && !playerView) return; // audio never started, skip
  if(playing) startFadeOut();
  delay(FADE_STEPS * FADE_STEP_MS + 10); // let fade complete
  audio.stopSong();
  muteI2S();
  playing=false; playerView=false;
}

void scanSD(){
  songCount=0;
  File root=SD.open("/"); if(!root) return;
  File f=root.openNextFile();
  while(f&&songCount<50){
    String n=String(f.name());
    if(n.endsWith(".mp3")||n.endsWith(".MP3")||
       n.endsWith(".wav")||n.endsWith(".WAV")||
       n.endsWith(".aac")||n.endsWith(".AAC")||
       n.endsWith(".flac")||n.endsWith(".FLAC"))
      songs[songCount++]=n;
    f=root.openNextFile();
  }
}

void drawSongList(){
  tft.fillScreen(BLACK); hdr("MP3 MODE");
  if(songCount==0){
    tft.setTextColor(DIMGRAY); tft.setTextSize(1);
    tft.setCursor(80,110); tft.print("No audio files on SD");
    ftr("RIGHT: back"); return;
  }
  int vis=7;
  for(int i=0;i<vis&&(i+songScroll)<songCount;i++){
    int idx=i+songScroll, y=28+i*26;
    if(idx==songIdx){
      tft.fillRect(0,y,SW-10,24,DARKPANEL);
      tft.fillRect(0,y,3,24,CYAN);
      tft.setTextColor(CYAN);
    } else { tft.setTextColor(WHITE); }
    tft.setTextSize(1); tft.setCursor(8,y+8);
    String nm=songs[idx];
    if(nm.length()>40) nm=nm.substring(0,37)+"...";
    tft.print(nm);
    if(idx==songIdx&&playing)
      tft.fillTriangle(SW-12,y+6,SW-12,y+18,SW-4,y+12,GREEN);
    divl(y+25);
  }
  if(songCount>vis){
    int sh=(SH-44)*vis/songCount, sy=28+(SH-44)*songScroll/songCount;
    tft.fillRect(SW-5,28,5,SH-44,DARKGRAY);
    tft.fillRect(SW-5,sy,5,sh,CYAN);
  }
  ftr("UP/DOWN:scroll  OK:play  RIGHT:back");
}

void drawPlayer(){
  tft.fillScreen(BLACK);
  hdr(playing?(looping?"NOW PLAYING (LOOP)":"NOW PLAYING"):"PAUSED");
  int as=SH-56, ax=8, ay=28;
  int rx=ax+as+10, rw=SW-rx-6;

  // Draw UI first, album art last so nothing overwrites it
  tft.setTextColor(WHITE); tft.setTextSize(1);
  String nm=songs[songIdx]; if(nm.length()>22) nm=nm.substring(0,19)+"...";
  tft.setCursor(rx,ay+8); tft.print(nm);
  tft.setTextColor(DIMGRAY);
  tft.setCursor(rx,ay+24); tft.print(String(songIdx+1)+"/"+String(songCount));
  tft.setTextColor(looping?GREEN:DARKGRAY);
  tft.setCursor(rx,ay+40); tft.print(looping?"LOOP ON":"LOOP OFF");
  int cx=rx+rw/2, cy=ay+110;
  if(playing){
    tft.fillRect(cx-16,cy-18,10,36,CYAN);
    tft.fillRect(cx+6,cy-18,10,36,CYAN);
  } else {
    tft.fillTriangle(cx-14,cy-20,cx-14,cy+20,cx+18,cy,GREEN);
  }
  tft.fillTriangle(cx-40,cy,cx-28,cy-10,cx-28,cy+10,DIMGRAY);
  tft.fillTriangle(cx+40,cy,cx+28,cy-10,cx+28,cy+10,DIMGRAY);
  ftr("OK:play/pause  HOLD OK:loop  LEFT:prev  RIGHT:next  DISP:screen");
  drawAlbumArt(songIdx);
}

void playSong(int idx){
  if(idx<0||idx>=songCount) return;
  startFadeOut();
  delay(FADE_STEPS * FADE_STEP_MS + 10);
  digitalWrite(SD_MODE, HIGH);  // enable amp before playback
  audio.connecttoFS(SD,("/"+songs[idx]).c_str());
  playing=true;
  startFadeIn();
  drawPlayer();
}

// Auto advance / loop when song ends
void audio_eof_mp3(const char* info){
  if(looping){ playSong(songIdx); }
  else if(songIdx<songCount-1){ songIdx++; playSong(songIdx); }
  else { playing=false; muteI2S(); if(playerView) drawPlayer(); }
}

void initMP3(){
  audio.setPinout(I2S_BCLK,I2S_WSEL,I2S_DOUT);
  audio.setVolume(vol);
  // Bass boost disabled — setTone(0,0,0) flat response
  audio.setTone(0, 0, 0);
  if(!SD.begin(SD_CS)){
    tft.fillScreen(BLACK); hdr("MP3 MODE");
    tft.setTextColor(RED); tft.setTextSize(1);
    tft.setCursor(60,110); tft.print("SD Card not found!");
    ftr("RIGHT: back"); return;
  }
  scanSD(); songScroll=0; songIdx=0; playing=false; playerView=false;
  drawSongList();
}

void mp3Input(){
  audio.loop();
  updateFade();

  // ── DISP button: player view has short/long press logic ──────────────────
  if(playerView){
    // Long press DISP = back to song list
    if(held(BTN_DISP)){
      playerView=false;
      drawSongList();
      return;
    }
    // Short press DISP = toggle display
    if(pressed(BTN_DISP)){
      dispOn=!dispOn;
      ledcWrite(0, dispOn ? map(bright, 0, 100, 0, 255) : 0);
      return;
    }
  } else {
    // In list view: short press DISP = toggle display only
    if(pressed(BTN_DISP)){
      dispOn=!dispOn;
      ledcWrite(0, dispOn ? map(bright, 0, 100, 0, 255) : 0);
      return;
    }
  }

  if(!playerView){
    if(pressed(BTN_UP))   { if(songIdx>0){songIdx--;if(songIdx<songScroll)songScroll--;drawSongList();} }
    if(pressed(BTN_DOWN)) { if(songIdx<songCount-1){songIdx++;if(songIdx>=songScroll+7)songScroll++;drawSongList();} }
    if(pressed(BTN_OK))   { playerView=true; playSong(songIdx); }
    if(pressed(BTN_RIGHT)){ stopMP3(); mode=MENU; drawMenu(); }
  } else {
    if(held(BTN_OK))      { looping=!looping; drawPlayer(); return; }
    if(pressed(BTN_OK)){
      if(playing){ startFadeOut(); } else { startFadeIn(); }
      audio.pauseResume(); playing=!playing; drawPlayer();
    }
    if(pressed(BTN_LEFT)) { if(songIdx>0) songIdx--; playSong(songIdx); }
    if(pressed(BTN_RIGHT)){ if(songIdx<songCount-1) songIdx++; playSong(songIdx); }
  }
}

// ── BLUETOOTH ─────────────────────────────────────────────────────────────────
void stopBT(){
  if(btActive){
    a2dp.end();
    btActive=false; btConn=false; btName="";
    // Give BT stack time to clean up heap before next init
    delay(300);
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
  }
}

volatile bool btStateChanged = false;

void btConnCB(esp_a2d_connection_state_t s, void*){
  if(s==ESP_A2D_CONNECTION_STATE_CONNECTED){
    btConn=true;
    delay(500);
    const char* peer = a2dp.get_peer_name();
    if(peer && strlen(peer)>0){
      btName = String(peer);
    } else {
      esp_bd_addr_t addr;
      memcpy(addr, a2dp.get_last_peer_address(), sizeof(esp_bd_addr_t));
      char addrStr[18];
      sprintf(addrStr, "%02X:%02X:%02X:%02X:%02X:%02X",
        addr[0],addr[1],addr[2],addr[3],addr[4],addr[5]);
      btName = String(addrStr);
    }
    bool found=false;
    for(int i=0;i<btHistCount;i++) if(btHist[i]==btName){found=true;break;}
    if(!found&&btHistCount<10) btHist[btHistCount++]=btName;
  } else if(s==ESP_A2D_CONNECTION_STATE_DISCONNECTED){
    btConn=false; btName="";
  }
  btStateChanged=true;
}

void drawBTScreen(){
  tft.fillScreen(BLACK); hdr("BLUETOOTH");
  if(btConn){
    tft.fillRoundRect(16,45,SW-32,60,8,DARKPANEL);
    tft.drawRoundRect(16,45,SW-32,60,8,GREEN);
    tft.setTextColor(GREEN); tft.setTextSize(1);
    tft.setCursor(26,57); tft.print("Connected to:");
    tft.setTextColor(WHITE); tft.setTextSize(2);
    tft.setCursor(26,74); tft.print(btName);
    tft.fillRoundRect(16,118,SW-32,36,6,DARKPANEL);
    tft.setTextColor(CYAN); tft.setTextSize(1);
    tft.setCursor(26,128); tft.print("Streaming A2DP audio");
    tft.setTextColor(DIMGRAY);
    tft.setCursor(26,142); tft.print("Play music from your phone!");
  } else {
    tft.fillRoundRect(16,45,SW-32,70,8,DARKPANEL);
    tft.drawRoundRect(16,45,SW-32,70,8,CYAN);
    tft.setTextColor(CYAN); tft.setTextSize(2);
    tft.setCursor(36,58); tft.print("Discoverable");
    tft.setTextColor(DIMGRAY); tft.setTextSize(1);
    tft.setCursor(26,85); tft.print("Name: ESP32-Speaker");
    tft.setCursor(26,100); tft.print("Connect from your phone BT menu");
    tft.setTextColor(DARKGRAY);
    tft.setCursor(26,145); tft.print("Waiting for connection...");
  }
  ftr("RIGHT: back");
}

void initBT(){
  digitalWrite(SD_MODE, HIGH);  // enable amp for BT audio
  stopMP3();
  tft.fillScreen(BLACK); hdr("BLUETOOTH");
  tft.setTextColor(CYAN); tft.setTextSize(1);
  tft.setCursor(80,110); tft.print("Starting Bluetooth...");

  i2s_config_t i2s_config={
    .mode=(i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_TX),
    .sample_rate=44100,
    .bits_per_sample=I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format=I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format=I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags=ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count=8,    // reduced from 16 — less heap usage per session
    .dma_buf_len=512,
    .use_apll=true,
    .tx_desc_auto_clear=true,
    .fixed_mclk=0
  };
  i2s_pin_config_t pin_config={
    .bck_io_num=I2S_BCLK,
    .ws_io_num=I2S_WSEL,
    .data_out_num=I2S_DOUT,
    .data_in_num=I2S_PIN_NO_CHANGE
  };
  a2dp.set_i2s_config(i2s_config);
  a2dp.set_pin_config(pin_config);
  a2dp.set_on_connection_state_changed(btConnCB);
  a2dp.start("ESP32-Speaker");
  btActive=true; btConn=false;
  drawBTScreen();
}

void btInput(){
  if(btStateChanged){ btStateChanged=false; drawBTScreen(); }
  if(pressed(BTN_DISP)){ dispOn=!dispOn; ledcWrite(0, dispOn ? map(bright, 0, 100, 0, 255) : 0); return; }
  if(pressed(BTN_RIGHT)){ stopBT(); mode=MENU; drawMenu(); }
}

// ── SETUP ─────────────────────────────────────────────────────────────────────
void setup(){
  Serial.begin(115200);
  for(int i=0;i<NBTNS;i++) pinMode(BPINS[i],INPUT_PULLUP);
  pinMode(SD_MODE, OUTPUT);
  digitalWrite(SD_MODE, LOW);  // amp muted until playback starts
  delay(100);
  for(int i=0;i<NBTNS;i++) bLow[i]=(digitalRead(BPINS[i])==LOW);

  ledcSetup(0, 40000, 8); // 40kHz PWM — above audible range, kills backlight buzz
  ledcAttachPin(TFT_BL, 0);
  ledcWrite(0, 255);

  tft.init(240,320);
  tft.setRotation(1);
  tft.fillScreen(0xFFFF);

  tft.setTextColor(CYAN); tft.setTextSize(4);
  tft.setCursor(70,55); tft.print("MP3");
  tft.setCursor(45,105); tft.print("PLAYER");
  tft.setTextColor(DIMGRAY); tft.setTextSize(1);
  tft.setCursor(95,155); tft.print("v3.0");
  tft.setCursor(80,170); tft.print("Starting up...");
  delay(1500);

  a2dp.set_auto_reconnect(false);

  drawMenu();
}

// ── LOOP ──────────────────────────────────────────────────────────────────────
void loop(){
  switch(mode){
    case MENU:      menuInput(); break;
    case SETTINGS:  setInput();  break;
    case MP3:       mp3Input();  break;
    case BLUETOOTH: btInput();   break;
  }
}
