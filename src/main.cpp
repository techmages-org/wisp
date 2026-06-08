// ============================================================================
// APHound — ESP32-S3 Wi-Fi AP fox-hunter (Geiger locator)
// Board: LilyGo T-Display-S3 (ST7789 170x320, 2 buttons, LiPo). No speaker, so
// the Geiger rides a passive buzzer on a GPIO.
//
// The same trick as the Warlock deck's locator, native on the ESP32:
//   1. WiFi.scanNetworks() lists APs (BSSID + RSSI + channel) → you PICK one.
//   2. Lock that MAC: promiscuous mode on its channel; the rx callback fires on
//      every frame and gives rx_ctrl.rssi. We keep only frames TRANSMITTED BY the
//      target (802.11 addr2 == target MAC) → that frame's RSSI is the target's
//      signal here, attributed to the right radio (just like wlan.ta on the deck).
//   3. GEIGER: a buzzer tick whose rate scales with signal — fast chatter when
//      hot, slow blips when cold — + a dBm/bar/peak/warmer-colder readout.
//
// Controls (2 buttons): BTN1 (GPIO0/BOOT) = next AP / mute while locked.
//                       BTN2 (GPIO14)     = lock onto selection / back to list.
//                       BTN1 long-press   = rescan.
// ============================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <TFT_eSPI.h>

// ---- pins (T-Display-S3) ---------------------------------------------------
static const int PIN_POWER_ON = 15; // MUST be HIGH to power the LCD + battery rail
static const int PIN_BTN1     = 0;  // BOOT button (active LOW)
static const int PIN_BTN2     = 14; // second button (active LOW)
static const int PIN_BUZZER   = 16; // passive buzzer / piezo (change to a free GPIO if needed)
static const int PIN_BAT_ADC  = 4;  // battery voltage divider

// ---- display ---------------------------------------------------------------
TFT_eSPI tft;
static const int SCRW = 320, SCRH = 170; // landscape

// ---- colors ----------------------------------------------------------------
#define C_BG      0x0000
#define C_DIM     0x52AA
#define C_TEXT    0xFFFF
#define C_MINT    0x5FEB
#define C_AMBER   0xFD20
#define C_PINK    0xF9A6
#define C_VIOLET  0x9C1F
#define C_CYAN    0x07FF

// ---- modes -----------------------------------------------------------------
enum Mode { SCANNING, PICKING, LOCKED };
static Mode mode = SCANNING;

// ---- scan results ----------------------------------------------------------
struct AP { uint8_t bssid[6]; char ssid[33]; int8_t rssi; uint8_t channel; };
static const int MAX_AP = 40;
static AP aps[MAX_AP];
static int apCount = 0;
static int sel = 0;

// ---- lock / sniffer state --------------------------------------------------
static uint8_t targetMac[6];
static uint8_t targetCh = 1;
static char    targetName[33] = "";
static volatile int      g_rssi   = -127;
static volatile uint32_t g_rssiMs = 0;
static volatile uint32_t g_frames = 0;
static float   smRssi = -90;      // smoothed
static int8_t  peakRssi = -127;
static uint32_t peakMs = 0;
static float   trendRef = -90;    // for warmer/colder
static uint32_t trendMs = 0;
static bool    muted = false;
static uint32_t lastTickMs = 0;

// ---- buzzer ----------------------------------------------------------------
static void tick(int freq, int durMs) {
  if (muted) return;
  tone(PIN_BUZZER, freq, durMs);
}

// Geiger cadence: clamp[-90,-35] → -90 = 1300ms (slow) … -35 = 110ms (fast).
static uint32_t geigerInterval(float rssi) {
  int r = (int)constrain(rssi, -90.0f, -35.0f);
  return (uint32_t)map(r, -90, -35, 1300, 110);
}

// ---- promiscuous rx callback: keep frames transmitted BY the target --------
static void snifferCb(void *buf, wifi_promiscuous_pkt_type_t type) {
  const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
  const uint8_t *pl = p->payload;
  // 802.11 addr2 (transmitter address) = bytes 10..15 of the MAC header.
  if (memcmp(pl + 10, targetMac, 6) == 0) {
    g_rssi   = p->rx_ctrl.rssi;
    g_rssiMs = millis();
    g_frames++;
  }
}

// ---- battery ---------------------------------------------------------------
static float batteryVolts() {
  // T-Display-S3 divides VBAT by 2 into GPIO4.
  uint32_t mv = analogReadMilliVolts(PIN_BAT_ADC);
  return (mv * 2.0f) / 1000.0f;
}

// ---- wifi mode helpers -----------------------------------------------------
static void startSta() {
  esp_wifi_set_promiscuous(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(50);
}

static void startSniffer() {
  WiFi.mode(WIFI_STA);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(snifferCb);
  esp_wifi_set_channel(targetCh, WIFI_SECOND_CHAN_NONE);
  g_rssi = -127; g_rssiMs = 0; g_frames = 0;
  smRssi = -90; peakRssi = -127; trendRef = -90; trendMs = millis();
}

// ---- scanning --------------------------------------------------------------
static void doScan() {
  mode = SCANNING;
  tft.fillScreen(C_BG);
  tft.setTextColor(C_AMBER, C_BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("scanning...", SCRW / 2, SCRH / 2, 4);
  startSta();
  int n = WiFi.scanNetworks(false, true); // blocking, include hidden
  apCount = 0;
  for (int i = 0; i < n && apCount < MAX_AP; i++) {
    AP &a = aps[apCount];
    memcpy(a.bssid, WiFi.BSSID(i), 6);
    String s = WiFi.SSID(i);
    s.toCharArray(a.ssid, sizeof(a.ssid));
    if (s.length() == 0) strcpy(a.ssid, "(hidden)");
    a.rssi = WiFi.RSSI(i);
    a.channel = WiFi.channel(i);
    apCount++;
  }
  WiFi.scanDelete();
  // sort by RSSI desc (strongest first)
  for (int i = 0; i < apCount; i++)
    for (int j = i + 1; j < apCount; j++)
      if (aps[j].rssi > aps[i].rssi) { AP t = aps[i]; aps[i] = aps[j]; aps[j] = t; }
  sel = 0;
  mode = PICKING;
}

// ---- rendering -------------------------------------------------------------
static const char *macStr(const uint8_t *m) {
  static char b[18];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return b;
}

static void drawPicker() {
  tft.fillScreen(C_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_VIOLET, C_BG);
  tft.drawString("APHound  pick a target", 6, 4, 2);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("BTN1 next  BTN2 lock  hold BTN1 rescan", 6, 154, 2);
  if (apCount == 0) {
    tft.setTextColor(C_AMBER, C_BG);
    tft.drawString("no APs - hold BTN1 to rescan", 6, 70, 2);
    return;
  }
  const int rows = 8, top = 26, rh = 16;
  int start = constrain(sel - rows / 2, 0, max(0, apCount - rows));
  for (int i = 0; i < rows && start + i < apCount; i++) {
    int idx = start + i, y = top + i * rh;
    bool on = (idx == sel);
    AP &a = aps[idx];
    uint16_t col = a.rssi >= -60 ? C_MINT : a.rssi >= -75 ? C_AMBER : C_PINK;
    if (on) { tft.fillRect(0, y - 1, SCRW, rh, 0x18E3); tft.setTextColor(C_TEXT, 0x18E3); }
    else tft.setTextColor(C_DIM, C_BG);
    tft.setTextDatum(TL_DATUM);
    char line[40];
    snprintf(line, sizeof(line), "%-16.16s c%-3d", a.ssid, a.channel);
    tft.drawString(on ? ">" : " ", 4, y, 2);
    tft.drawString(line, 16, y, 2);
    tft.setTextColor(col, on ? 0x18E3 : C_BG);
    tft.setTextDatum(TR_DATUM);
    char r[8]; snprintf(r, sizeof(r), "%d", a.rssi);
    tft.drawString(r, SCRW - 6, y, 2);
  }
}

static void lockSelection() {
  AP &a = aps[sel];
  memcpy(targetMac, a.bssid, 6);
  targetCh = a.channel ? a.channel : 1;
  strncpy(targetName, a.ssid, sizeof(targetName));
  startSniffer();
  mode = LOCKED;
  lastTickMs = 0;
}

static void drawMeter(int rssi, bool live) {
  // header
  tft.fillRect(0, 0, SCRW, 22, C_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_VIOLET, C_BG);
  tft.drawString(targetName[0] ? targetName : macStr(targetMac), 6, 3, 2);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(C_DIM, C_BG);
  char ch[24]; snprintf(ch, sizeof(ch), "ch%d  %.2fV", targetCh, batteryVolts());
  tft.drawString(ch, SCRW - 6, 3, 2);

  // big dBm
  tft.fillRect(0, 26, SCRW, 60, C_BG);
  uint16_t rc = !live ? C_DIM : rssi >= -60 ? C_MINT : rssi >= -75 ? C_AMBER : C_PINK;
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(rc, C_BG);
  char db[8]; snprintf(db, sizeof(db), "%d", live ? rssi : 0);
  tft.drawString(live ? db : "--", 6, 30, 7);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("dBm", 150, 60, 4);

  // signal bar (-90..-30)
  int bw = SCRW - 12, x = 6, y = 92, h = 18;
  tft.drawRect(x, y, bw, h, C_DIM);
  int fill = live ? (int)(((constrain(rssi, -90, -30) + 90) / 60.0f) * (bw - 2)) : 0;
  tft.fillRect(x + 1, y + 1, bw - 2, h - 2, C_BG);
  if (fill > 0) tft.fillRect(x + 1, y + 1, fill, h - 2, rc);

  // trend + proximity + peak
  tft.fillRect(0, 116, SCRW, 36, C_BG);
  tft.setTextDatum(TL_DATUM);
  const char *trend; uint16_t tcol;
  float d = smRssi - trendRef;
  if (!live) { trend = "ACQUIRING"; tcol = C_DIM; }
  else if (d >= 2) { trend = "WARMER >>"; tcol = C_MINT; }
  else if (d <= -2) { trend = "COLDER <<"; tcol = C_PINK; }
  else { trend = "STEADY"; tcol = C_AMBER; }
  tft.setTextColor(tcol, C_BG);
  tft.drawString(trend, 6, 118, 4);
  tft.setTextColor(C_DIM, C_BG);
  char pk[40];
  snprintf(pk, sizeof(pk), "peak %d   %s", peakRssi, muted ? "[muted]" : "<3 geiger");
  tft.drawString(pk, 6, 150, 2);
  tft.setTextDatum(TR_DATUM);
  const char *prox = !live ? "" : rssi >= -45 ? "VERY CLOSE" : rssi >= -60 ? "CLOSE" : rssi >= -72 ? "NEAR" : "FAR";
  tft.setTextColor(rc, C_BG);
  tft.drawString(prox, SCRW - 6, 118, 4);
}

// ---- buttons ---------------------------------------------------------------
struct Btn { int pin; bool last; uint32_t downAt; bool longFired; };
static Btn b1 = {PIN_BTN1, true, 0, false};
static Btn b2 = {PIN_BTN2, true, 0, false};

// returns: 0 none, 1 short, 2 long (on release / threshold)
static int poll(Btn &b) {
  bool s = digitalRead(b.pin); // active LOW
  int ev = 0;
  if (b.last && !s) { b.downAt = millis(); b.longFired = false; }       // press
  if (!s && !b.longFired && millis() - b.downAt > 600) { ev = 2; b.longFired = true; } // long
  if (!b.last && s && !b.longFired) ev = 1;                              // release = short
  b.last = s;
  return ev;
}

// ============================================================================
void setup() {
  pinMode(PIN_POWER_ON, OUTPUT);
  digitalWrite(PIN_POWER_ON, HIGH); // power LCD + battery rail
  pinMode(PIN_BTN1, INPUT_PULLUP);
  pinMode(PIN_BTN2, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  Serial.begin(115200);

  tft.init();
  tft.setRotation(1); // landscape 320x170
  tft.fillScreen(C_BG);

  // boot chirp
  tick(2200, 60); delay(80); tick(3000, 60);

  doScan();
  drawPicker();
}

void loop() {
  int e1 = poll(b1), e2 = poll(b2);

  if (mode == PICKING) {
    if (e1 == 1) { sel = (sel + 1) % max(1, apCount); drawPicker(); }
    if (e1 == 2) { doScan(); drawPicker(); }
    if (e2 == 1 && apCount > 0) { lockSelection(); drawMeter(-127, false); }
    delay(15);
    return;
  }

  if (mode == LOCKED) {
    if (e2 == 1) { startSta(); doScan(); drawPicker(); return; } // back to list
    if (e1 == 1) { muted = !muted; }                            // mute toggle

    uint32_t now = millis();
    bool live = (now - g_rssiMs) < 2000 && g_rssiMs != 0;
    int rssi = g_rssi;
    if (live) {
      smRssi = smRssi * 0.7f + rssi * 0.3f;
      if (rssi > peakRssi) { peakRssi = rssi; peakMs = now; tick(2600, 35); } // peak ping
    }
    if (now - trendMs > 1200) { trendRef = smRssi; trendMs = now; }

    // geiger cadence
    if (live) {
      uint32_t iv = geigerInterval(smRssi);
      if (now - lastTickMs >= iv) { tick(1000, 18); lastTickMs = now; }
    }

    static uint32_t lastDraw = 0;
    if (now - lastDraw > 120) { drawMeter(rssi, live); lastDraw = now; }
    delay(5);
    return;
  }

  delay(20);
}
