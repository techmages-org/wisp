// ============================================================================
// Wisp — M5 build (M5Unified). Targets the M5StickC Plus 1.1 and also runs on
// the M5 Cardputer / other M5 boards (M5Unified auto-detects display, buttons,
// and the built-in buzzer/speaker — so NO external wiring).
//
// Adds the "getting closer" RADAR animation: an expanding ping ring whose rate is
// the Geiger cadence (faster as you close in), a center blip that grows hotter,
// + dBm / signal-bar / WARMER-COLDER / peak readouts. Buzzer ticks faster as the
// target gets stronger; a sharper double-beep on each new peak.
//
// Buttons (StickC Plus): A (big front "M5") = next AP / mute · B (side) = lock /
// back to list · hold A = rescan.
// ============================================================================
#include <M5Unified.h>
#include "foxcore.h"
#include "wisp_logo.h"
using namespace fox;

// RGB565 palette
#define C_BG 0x0000
#define C_DIM 0x52AA
#define C_TEXT 0xFFFF
#define C_MINT 0x5FEB
#define C_AMBER 0xFD20
#define C_PINK 0xF9A6
#define C_VIOLET 0x9C1F
#define C_RING 0x2104
#define C_SEL 0x18E3

// StickC Plus buzzer = a PASSIVE piezo on GPIO2. M5.Speaker is finicky with it,
// so we disable M5's speaker (cfg.internal_spk=false) and drive GPIO2 directly
// with Arduino tone() — a square wave the piezo turns into a real "tick".
#define BUZZER_PIN 2

static bool muted = false;
static uint32_t lastTickMs = 0;
static int SCRW = 240, SCRH = 135;

// Top-level tool: the AP/device fox-hunt, the probe-request sniffer, or the
// associated-client list for a chosen AP. PWR cycles HUNT<->PROBES; hold-B on
// an AP in the picker drills into that AP's CLIENTS. Both are orthogonal to the
// HUNT mode machine and return early before the PICKING/LOCKED handlers.
enum Tool { T_HUNT, T_PROBES, T_CLIENTS };
static Tool tool = T_HUNT;
static int  probeSel = 0;
static int  clientSel = 0;
static char clientApName[33] = "";
static uint32_t lastHop = 0;

static void tick(int freq, int dur) { if (!muted) tone(BUZZER_PIN, freq, dur); }

static uint16_t heat(int rssi, bool live) {
  if (!live) return C_DIM;
  return rssi >= -60 ? C_MINT : rssi >= -75 ? C_AMBER : C_PINK;
}

static void drawPicker() {
  auto &d = M5.Display;
  d.fillScreen(C_BG);
  d.setTextSize(1);
  d.setTextColor(C_VIOLET, C_BG);
  d.setCursor(4, 2); d.print("Wisp  pick a target");
  d.setTextColor(C_DIM, C_BG);
  d.setCursor(4, SCRH - 9); d.print("A next B hunt Bhold clients PWR probe");
  if (apCount == 0) {
    d.setTextColor(C_AMBER, C_BG);
    d.setCursor(4, 60); d.print("no APs - hold A to rescan");
    return;
  }
  const int rows = 9, top = 15, rh = 12;
  int start = constrain(sel - rows / 2, 0, max(0, apCount - rows));
  for (int i = 0; i < rows && start + i < apCount; i++) {
    int idx = start + i, y = top + i * rh;
    bool on = (idx == sel);
    AP &a = aps[idx];
    uint16_t col = a.rssi >= -60 ? C_MINT : a.rssi >= -75 ? C_AMBER : C_PINK;
    if (on) { d.fillRect(0, y - 1, SCRW, rh, C_SEL); d.setTextColor(C_TEXT, C_SEL); }
    else d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, y);
    d.printf("%c%-14.14s c%-2d", on ? '>' : ' ', a.ssid, a.channel);
    d.setTextColor(col, on ? C_SEL : C_BG);
    char r[6]; snprintf(r, sizeof(r), "%d", a.rssi);
    int w = d.textWidth(r);
    d.setCursor(SCRW - w - 4, y); d.print(r);
  }
}

// PROBES list: every nearby device that's calling out for a network, with the
// SSID it's hunting for, its RSSI, and the channel we heard it on. Lock one (B)
// to fox-hunt that specific phone/laptop. Channel-hops while open.
static void drawProbes() {
  auto &d = M5.Display;
  d.fillScreen(C_BG);
  d.setTextSize(1);
  d.setTextColor(C_VIOLET, C_BG);
  d.setCursor(4, 2);
  d.printf("Wisp probes  ch%-2d  %d dev", hopCh, probeCount);
  d.setTextColor(C_DIM, C_BG);
  d.setCursor(4, SCRH - 9); d.print("A next  B lock  holdA clr  PWR hunt");
  if (probeCount == 0) {
    d.setTextColor(C_AMBER, C_BG);
    d.setCursor(4, 60); d.print("listening for probe requests...");
    return;
  }
  const int rows = 9, top = 15, rh = 12;
  int start = constrain(probeSel - rows / 2, 0, max(0, probeCount - rows));
  for (int i = 0; i < rows && start + i < probeCount; i++) {
    int idx = start + i, y = top + i * rh;
    bool on = (idx == probeSel);
    Probe &pr = probes[idx];
    uint16_t col = pr.rssi >= -60 ? C_MINT : pr.rssi >= -75 ? C_AMBER : C_PINK;
    if (on) { d.fillRect(0, y - 1, SCRW, rh, C_SEL); d.setTextColor(C_TEXT, C_SEL); }
    else d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, y);
    d.printf("%c%02X:%02X:%02X %-11.11s", on ? '>' : ' ',
             pr.mac[3], pr.mac[4], pr.mac[5], pr.ssid);
    d.setTextColor(col, on ? C_SEL : C_BG);
    char r[6]; snprintf(r, sizeof(r), "%d", pr.rssi);
    int w = d.textWidth(r);
    d.setCursor(SCRW - w - 4, y); d.print(r);
  }
}

// CLIENTS list: stations associated to the AP you picked, by MAC + signal. Lock
// one (B) to fox-hunt that station. Stays parked on the AP's channel (no hop).
static void drawClients() {
  auto &d = M5.Display;
  d.fillScreen(C_BG);
  d.setTextSize(1);
  d.setTextColor(C_VIOLET, C_BG);
  d.setCursor(4, 2);
  d.printf("%.10s ch%-2d  %d sta", clientApName[0] ? clientApName : "AP", clientCh, clientCount);
  d.setTextColor(C_DIM, C_BG);
  d.setCursor(4, SCRH - 9); d.print("A next  B hunt  holdA back");
  if (clientCount == 0) {
    d.setTextColor(C_AMBER, C_BG);
    d.setCursor(4, 60); d.print("waiting for client traffic...");
    return;
  }
  const int rows = 9, top = 15, rh = 12;
  int start = constrain(clientSel - rows / 2, 0, max(0, clientCount - rows));
  for (int i = 0; i < rows && start + i < clientCount; i++) {
    int idx = start + i, y = top + i * rh;
    bool on = (idx == clientSel);
    Sta &c = clients[idx];
    uint16_t col = c.rssi >= -60 ? C_MINT : c.rssi >= -75 ? C_AMBER : C_PINK;
    if (on) { d.fillRect(0, y - 1, SCRW, rh, C_SEL); d.setTextColor(C_TEXT, C_SEL); }
    else d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, y);
    d.printf("%c%02X:%02X:%02X:%02X:%02X:%02X", on ? '>' : ' ',
             c.mac[0], c.mac[1], c.mac[2], c.mac[3], c.mac[4], c.mac[5]);
    d.setTextColor(col, on ? C_SEL : C_BG);
    char r[6]; snprintf(r, sizeof(r), "%d", c.rssi);
    int w = d.textWidth(r);
    d.setCursor(SCRW - w - 4, y); d.print(r);
  }
}

static float pingPhase = 0;
static uint32_t lastFrame = 0;

static void drawMeter(int rssi, bool live) {
  auto &d = M5.Display;
  uint32_t now = millis();
  uint32_t dt = lastFrame ? now - lastFrame : 33;
  lastFrame = now;
  uint16_t rc = heat(rssi, live);
  int dir = trendDir();

  // header
  d.fillRect(0, 0, SCRW, 13, C_BG);
  d.setTextSize(1);
  d.setTextColor(C_VIOLET, C_BG);
  d.setCursor(2, 2);
  d.printf("%.12s c%d", targetName[0] ? targetName : macStr(targetMac), targetCh);
  char bat[8]; snprintf(bat, sizeof(bat), "%d%%", M5.Power.getBatteryLevel());
  int bw = d.textWidth(bat);
  d.setTextColor(C_DIM, C_BG);
  d.setCursor(SCRW - bw - 2, 2); d.print(bat);

  // --- RADAR (left): faint rings + an expanding ping at the Geiger rate ---
  const int cx = 60, cy = 74, maxR = 56;
  d.fillRect(0, 14, 122, SCRH - 14, C_BG);
  for (int rr = maxR; rr > 6; rr -= 14) d.drawCircle(cx, cy, rr, C_RING);
  d.drawFastHLine(cx - maxR, cy, maxR * 2, C_RING);
  d.drawFastVLine(cx, cy - maxR, maxR * 2, C_RING);
  if (live) {
    uint32_t iv = geigerInterval(smRssi);
    pingPhase += (float)dt / (float)iv;
    while (pingPhase >= 1) pingPhase -= 1;
    int pr = (int)(pingPhase * maxR);
    d.drawCircle(cx, cy, pr, rc);
    if (pr > 1) d.drawCircle(cx, cy, pr - 1, rc);
    int dot = rssi >= -50 ? 7 : rssi >= -65 ? 5 : 3;
    d.fillCircle(cx, cy, dot, rc);
  } else {
    d.fillCircle(cx, cy, 3, C_DIM);
  }

  // --- readouts (right) ---
  d.fillRect(124, 14, SCRW - 124, SCRH - 14, C_BG);
  d.setTextColor(rc, C_BG);
  d.setTextSize(3);
  d.setCursor(130, 22);
  if (live) d.printf("%d", rssi); else d.print("--");
  d.setTextColor(C_DIM, C_BG);
  d.setTextSize(1);
  d.setCursor(130, 48); d.print("dBm");

  int x = 130, y = 64, w = SCRW - x - 6, h = 12;
  d.drawRect(x, y, w, h, C_DIM);
  int fill = live ? (int)(((constrain(rssi, -90, -30) + 90) / 60.0f) * (w - 2)) : 0;
  if (fill > 0) d.fillRect(x + 1, y + 1, fill, h - 2, rc);

  const char *tw = !live ? "ACQUIRING" : dir > 0 ? "WARMER >>" : dir < 0 ? "COLDER <<" : "STEADY";
  uint16_t tc = !live ? C_DIM : dir > 0 ? C_MINT : dir < 0 ? C_PINK : C_AMBER;
  d.setTextColor(tc, C_BG);
  d.setCursor(130, 84); d.print(tw);
  d.setTextColor(C_DIM, C_BG);
  d.setCursor(130, 100); d.printf("pk %d %s", peakRssi, muted ? "[mute]" : "");
  d.setCursor(130, 116); d.print("A mute  B back");
}

// Boot splash: the real Wisp logo (img/wisp.svg → PNG) + wordmark + boot chirp.
static void splash() {
  auto &d = M5.Display;
  d.fillScreen(C_BG);
  int ly = (SCRH - 116) / 2; if (ly < 0) ly = 0;
  d.drawPng(wisp_logo_png, wisp_logo_png_len, 6, ly); // 116x116 emblem, left
  d.setTextColor(C_MINT, C_BG);   d.setTextSize(3); d.setCursor(132, 30); d.print("Wisp");
  d.setTextColor(C_VIOLET, C_BG); d.setTextSize(1); d.setCursor(132, 60); d.print("Wi-Fi");
  d.setCursor(132, 72); d.print("will-o'-wisp");
  d.setTextColor(C_AMBER, C_BG);  d.setCursor(132, SCRH - 12); d.print("v0.1  TechMages");
  tick(2200, 80); delay(170); tick(3100, 80); delay(950); // chirp + hold ~1.3s
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_spk = false; // don't let M5 grab the GPIO2 buzzer — we drive it
  M5.begin(cfg);
  M5.Display.setRotation(1);
  SCRW = M5.Display.width();
  SCRH = M5.Display.height();
  pinMode(BUZZER_PIN, OUTPUT);
  splash();
  doScan();
  drawPicker();
}

void loop() {
  M5.update();

  // PWR cycles the top-level tool: HUNT <-> PROBES. From CLIENTS it backs out to
  // the HUNT picker. Tool-aware so it stops the right capture before switching.
  if (M5.BtnPWR.wasClicked()) {
    if (tool == T_HUNT) {
      tool = T_PROBES; startProbes(); probeSel = 0; lastHop = millis(); drawProbes();
    } else if (tool == T_PROBES) {
      stopProbes(); tool = T_HUNT; startSta(); doScan(); drawPicker();
    } else { // T_CLIENTS
      stopClients(); tool = T_HUNT; drawPicker();   // aps[] still populated — no rescan
    }
    return;
  }

  if (tool == T_PROBES) {
    uint32_t now = millis();
    if (now - lastHop > 250) { hopChannel(); lastHop = now; }
    if (M5.BtnA.wasClicked() && probeCount) probeSel = (probeSel + 1) % probeCount;
    if (M5.BtnA.wasHold())    { probeCount = 0; probeSel = 0; }
    if (M5.BtnB.wasClicked() && probeCount > 0) {
      lockProbe(probeSel);      // -> mode == LOCKED on the chosen device
      tool = T_HUNT;
      lastFrame = 0;
      drawMeter(-127, false);
      return;
    }
    static uint32_t lastDraw = 0;
    if (now - lastDraw > 200) { drawProbes(); lastDraw = now; }
    delay(8);
    return;
  }

  if (tool == T_CLIENTS) {
    uint32_t now = millis();
    if (M5.BtnA.wasClicked() && clientCount) clientSel = (clientSel + 1) % clientCount;
    if (M5.BtnA.wasHold())    { stopClients(); tool = T_HUNT; drawPicker(); return; }
    if (M5.BtnB.wasClicked() && clientCount > 0) {
      lockClient(clientSel);    // -> mode == LOCKED on the chosen station
      tool = T_HUNT;
      lastFrame = 0;
      drawMeter(-127, false);
      return;
    }
    static uint32_t lastDraw = 0;
    if (now - lastDraw > 200) { drawClients(); lastDraw = now; }
    delay(8);
    return;
  }

  if (mode == PICKING) {
    if (M5.BtnA.wasClicked()) { sel = (sel + 1) % max(1, apCount); drawPicker(); }
    if (M5.BtnA.wasHold())    { doScan(); drawPicker(); }
    if (M5.BtnB.wasHold() && apCount > 0) {   // drill into this AP's clients
      strncpy(clientApName, aps[sel].ssid, sizeof(clientApName) - 1);
      clientApName[sizeof(clientApName) - 1] = 0;
      startClients(aps[sel].bssid, aps[sel].channel);
      tool = T_CLIENTS; clientSel = 0; drawClients();
      return;
    }
    if (M5.BtnB.wasClicked() && apCount > 0) { lockSelection(); lastFrame = 0; drawMeter(-127, false); }
    delay(12);
    return;
  }

  if (mode == LOCKED) {
    if (M5.BtnB.wasClicked()) { startSta(); doScan(); drawPicker(); return; }
    if (M5.BtnA.wasClicked()) { muted = !muted; }
    bool live; int rssi;
    bool newPeak = updateSignal(live, rssi);
    uint32_t now = millis();
    if (newPeak) tick(2600, 35);
    if (live) {
      uint32_t iv = geigerInterval(smRssi);
      if (now - lastTickMs >= iv) { tick(1000, 16); lastTickMs = now; }
    }
    static uint32_t lastDraw = 0;
    if (now - lastDraw > 45) { drawMeter(rssi, live); lastDraw = now; }
    delay(3);
    return;
  }
  delay(20);
}
