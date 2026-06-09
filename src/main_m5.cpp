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

// Audio differs by board. The StickC Plus 1.1 has a PASSIVE piezo on GPIO2 that
// M5.Speaker won't drive well, so we disable M5's speaker and hit GPIO2 directly
// with Arduino tone(). The ESP32-S3 stick has a speaker M5Unified DOES drive, so
// there we use M5.Speaker.tone() and let the lib own whatever pin/codec it is.
#ifndef WISP_S3
#define BUZZER_PIN 2
#endif

static bool muted = false;
static uint32_t lastTickMs = 0;
static int SCRW = 240, SCRH = 135;

// Power saving (small on-board battery): a moderate default backlight + auto-dim
// of the browse screens when idle (never the hunt meter — that's when you're
// actively reading it). CPU also drops to 160 MHz in setup: WiFi-safe, and about
// half the core draw of 240 MHz. Any button press wakes the screen.
static const uint8_t BRI_FULL = 170, BRI_DIM = 24;
static uint32_t lastInput = 0;
static bool     screenDim = false;

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

static void tick(int freq, int dur) {
  if (muted) return;
#ifdef WISP_S3
  M5.Speaker.tone(freq, dur);   // S3 stick: real speaker, driven by M5Unified
#else
  tone(BUZZER_PIN, freq, dur);  // Plus 1.1: passive piezo on GPIO2
#endif
}

static uint16_t heat(int rssi, bool live) {
  if (!live) return C_DIM;
  return rssi >= -60 ? C_MINT : rssi >= -75 ? C_AMBER : C_PINK;
}

// Battery % tag, top-right of a header. Skipped if the board has no gauge.
// Turns pink under 15% so you see the runway on every screen.
static void battTag() {
  int lvl = M5.Power.getBatteryLevel();
  if (lvl < 0) return;
  auto &d = M5.Display;
  char b[8]; snprintf(b, sizeof(b), "%d%%", lvl);
  d.setTextSize(2);
  d.setTextColor(lvl <= 15 ? C_PINK : C_DIM, C_BG);
  int w = d.textWidth(b);
  d.setCursor(SCRW - w - 3, 2); d.print(b);
}

// Tiny 4-bar signal glyph (phone-style) — a persistent S-meter on every list row.
static void drawBars(int x, int yBottom, int rssi, uint16_t lit, uint16_t off) {
  auto &d = M5.Display;
  int n = rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -78 ? 2 : rssi >= -88 ? 1 : 0;
  for (int b = 0; b < 4; b++) {
    int h = 2 + b * 2;                        // 2,4,6,8 px
    d.fillRect(x + b * 3, yBottom - h, 2, h, b < n ? lit : off);
  }
}

// Any button press wakes the screen back to full brightness.
static void wakeScreen() {
  lastInput = millis();
  if (screenDim) { M5.Display.setBrightness(BRI_FULL); screenDim = false; }
}

// Marquee: the selected row's name ping-pong scrolls when it's wider than its
// cell, so a long Wi-Fi name can be read end to end. marqueeOff advances on a
// timer in loop(); it resets to 0 whenever the selection changes.
static int marqueeOff = 0;
static void drawScrollName(int x, int y, int cw, const char* name, uint16_t fg, uint16_t bg) {
  auto &d = M5.Display;
  d.setClipRect(x, y - 2, cw, 19);
  d.fillRect(x, y - 2, cw, 19, bg);
  d.setTextColor(fg, bg);
  int tw = d.textWidth(name);
  if (tw <= cw) { d.setCursor(x, y); d.print(name); }
  else {
    int span = tw - cw + 14;
    int off = marqueeOff % (span * 2);
    if (off > span) off = span * 2 - off;   // ping-pong: out, then back
    d.setCursor(x - off, y); d.print(name);
  }
  d.clearClipRect();
}

// One size-2 list row: highlight bar, rssi (right) + S-meter bars, marker, and a
// name cell (scrolls when selected & too long, otherwise clipped). Assumes the
// caller has already set textSize(2).
static int selY = -1;   // screen-y of the selected row, for the marquee repaint
static void drawRow(int y, bool on, const char* name, int rssi) {
  auto &d = M5.Display;
  uint16_t col = rssi >= -60 ? C_MINT : rssi >= -75 ? C_AMBER : C_PINK;
  uint16_t bg = on ? C_SEL : C_BG;
  if (on) d.fillRect(0, y - 2, SCRW, 19, C_SEL);
  char r[6]; snprintf(r, sizeof(r), "%d", rssi);
  int w = d.textWidth(r);
  int barsX = SCRW - w - 18;
  d.setTextColor(col, bg);
  d.setCursor(SCRW - w - 4, y); d.print(r);
  drawBars(barsX, y + 13, rssi, col, on ? C_DIM : C_RING);
  d.setTextColor(on ? C_TEXT : C_DIM, bg);
  d.setCursor(2, y); d.print(on ? ">" : " ");
  int nx = 16, nw = barsX - nx - 4; if (nw < 12) nw = 12;
  if (on) drawScrollName(nx, y, nw, name, C_TEXT, C_SEL);
  else {
    d.setClipRect(nx, y - 2, nw, 19);
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(nx, y); d.print(name);
    d.clearClipRect();
  }
}

static void drawPicker() {
  auto &d = M5.Display;
  d.fillScreen(C_BG);
  d.setTextSize(2);
  d.setTextColor(C_VIOLET, C_BG);
  d.setCursor(4, 2); d.print("Pick AP");
  battTag();
  d.setTextSize(1);
  d.setTextColor(C_DIM, C_BG);
  d.setCursor(4, SCRH - 9); d.print("A next  B hunt  Bhold clients  PWR probe");
  if (apCount == 0) {
    d.setTextSize(2); d.setTextColor(C_AMBER, C_BG);
    d.setCursor(6, 48); d.print("no APs");
    d.setCursor(6, 72); d.print("holdA scan");
    return;
  }
  selY = -1;
  const int top = 22, rh = 19;
  int rows = (SCRH - top - 12) / rh; if (rows < 1) rows = 1;
  int start = constrain(sel - rows / 2, 0, max(0, apCount - rows));
  d.setTextSize(2);
  for (int i = 0; i < rows && start + i < apCount; i++) {
    int idx = start + i, y = top + i * rh;
    bool on = (idx == sel);
    if (on) selY = y;
    drawRow(y, on, aps[idx].ssid, aps[idx].rssi);
  }
}

// PROBES list: every nearby device that's calling out for a network, with the
// SSID it's hunting for, its RSSI, and the channel we heard it on. Lock one (B)
// to fox-hunt that specific phone/laptop. Channel-hops while open.
static void drawProbes() {
  auto &d = M5.Display;
  d.fillScreen(C_BG);
  d.setTextSize(2);
  d.setTextColor(C_VIOLET, C_BG);
  d.setCursor(4, 2); d.printf("Probes %d", probeCount);
  battTag();
  d.setTextSize(1);
  d.setTextColor(C_DIM, C_BG);
  d.setCursor(4, SCRH - 9); d.print("A next  B lock  holdA clr  PWR hunt");
  if (probeCount == 0) {
    d.setTextSize(2); d.setTextColor(C_AMBER, C_BG);
    d.setCursor(6, 48); d.print("listening");
    d.setCursor(6, 72); d.printf("ch %d ...", hopCh);
    selY = -1;
    return;
  }
  selY = -1;
  const int top = 22, rh = 19;
  int rows = (SCRH - top - 12) / rh; if (rows < 1) rows = 1;
  int start = constrain(probeSel - rows / 2, 0, max(0, probeCount - rows));
  d.setTextSize(2);
  for (int i = 0; i < rows && start + i < probeCount; i++) {
    int idx = start + i, y = top + i * rh;
    bool on = (idx == probeSel);
    if (on) selY = y;
    Probe &pr = probes[idx];
    char nm[40]; snprintf(nm, sizeof(nm), "%02X%02X %s", pr.mac[4], pr.mac[5], pr.ssid);
    drawRow(y, on, nm, pr.rssi);
  }
}

// CLIENTS list: stations associated to the AP you picked, by MAC + signal. Lock
// one (B) to fox-hunt that station. Stays parked on the AP's channel (no hop).
static void drawClients() {
  auto &d = M5.Display;
  d.fillScreen(C_BG);
  d.setTextSize(2);
  d.setTextColor(C_VIOLET, C_BG);
  d.setCursor(4, 2); d.printf("%.6s %d", clientApName[0] ? clientApName : "AP", clientCount);
  battTag();
  d.setTextSize(1);
  d.setTextColor(C_DIM, C_BG);
  d.setCursor(4, SCRH - 9); d.print("A next  B hunt  holdA back");
  if (clientCount == 0) {
    d.setTextSize(2); d.setTextColor(C_AMBER, C_BG);
    d.setCursor(6, 48); d.print("waiting");
    d.setCursor(6, 72); d.print("traffic");
    selY = -1;
    return;
  }
  selY = -1;
  const int top = 22, rh = 19;
  int rows = (SCRH - top - 12) / rh; if (rows < 1) rows = 1;
  int start = constrain(clientSel - rows / 2, 0, max(0, clientCount - rows));
  d.setTextSize(2);
  for (int i = 0; i < rows && start + i < clientCount; i++) {
    int idx = start + i, y = top + i * rh;
    bool on = (idx == clientSel);
    if (on) selY = y;
    Sta &c = clients[idx];
    char nm[20]; snprintf(nm, sizeof(nm), "%02X:%02X:%02X:%02X", c.mac[2], c.mac[3], c.mac[4], c.mac[5]);
    drawRow(y, on, nm, c.rssi);
  }
}

// The selected entry's display name + rssi for the active list — used by the
// marquee to repaint just that one row between full redraws.
static void selName(char* out, int n) {
  if (tool == T_PROBES && probeCount) {
    Probe &p = probes[probeSel];
    snprintf(out, n, "%02X%02X %s", p.mac[4], p.mac[5], p.ssid);
  } else if (tool == T_CLIENTS && clientCount) {
    Sta &c = clients[clientSel];
    snprintf(out, n, "%02X:%02X:%02X:%02X", c.mac[2], c.mac[3], c.mac[4], c.mac[5]);
  } else if (tool == T_HUNT && apCount) {
    snprintf(out, n, "%s", aps[sel].ssid);
  } else { out[0] = 0; }
}
static int selRssi() {
  if (tool == T_PROBES && probeCount) return probes[probeSel].rssi;
  if (tool == T_CLIENTS && clientCount) return clients[clientSel].rssi;
  if (tool == T_HUNT && apCount) return aps[sel].rssi;
  return -127;
}
static void tickMarquee() {
  if (selY < 0) return;
  char nm[40]; selName(nm, sizeof(nm));
  if (!nm[0]) return;
  M5.Display.setTextSize(2);
  drawRow(selY, true, nm, selRssi());
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
#ifndef WISP_S3
  cfg.internal_spk = false; // Plus 1.1: don't let M5 grab the GPIO2 buzzer — we drive it
#endif
  M5.begin(cfg);
  M5.Display.setRotation(1);
  SCRW = M5.Display.width();
  SCRH = M5.Display.height();
  setCpuFrequencyMhz(160);          // WiFi-safe, ~half the core draw of 240
  M5.Display.setBrightness(BRI_FULL);
  lastInput = millis();
#ifdef WISP_S3
  M5.Speaker.setVolume(200);  // S3 stick: crank the real speaker
#else
  pinMode(BUZZER_PIN, OUTPUT);
#endif
  splash();
  doScan();
  drawPicker();
}

void loop() {
  M5.update();

  // Screen power: wake on any button; auto-dim the browse screens after 30 s idle
  // (never the hunt meter — full brightness while you're actively walking it down).
  if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnPWR.wasPressed()) wakeScreen();
  if (mode == LOCKED) {
    if (screenDim) { M5.Display.setBrightness(BRI_FULL); screenDim = false; }
  } else if (!screenDim && millis() - lastInput > 30000) {
    M5.Display.setBrightness(BRI_DIM); screenDim = true;
  }

  // Marquee: scroll the selected long name on the list screens (not the meter).
  if (mode != LOCKED) {
    static uint32_t lastMarq = 0;
    uint32_t nowm = millis();
    if (nowm - lastMarq > 90) { marqueeOff += 5; lastMarq = nowm; tickMarquee(); }
  }

  // PWR cycles the top-level tool: HUNT <-> PROBES. From CLIENTS it backs out to
  // the HUNT picker. Tool-aware so it stops the right capture before switching.
  if (M5.BtnPWR.wasClicked()) {
    marqueeOff = 0;
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
    if (M5.BtnA.wasClicked() && probeCount) { probeSel = (probeSel + 1) % probeCount; marqueeOff = 0; }
    if (M5.BtnA.wasHold())    { probeCount = 0; probeSel = 0; marqueeOff = 0; }
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
    if (M5.BtnA.wasClicked() && clientCount) { clientSel = (clientSel + 1) % clientCount; marqueeOff = 0; }
    if (M5.BtnA.wasHold())    { stopClients(); tool = T_HUNT; marqueeOff = 0; drawPicker(); return; }
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
    if (M5.BtnA.wasClicked()) { sel = (sel + 1) % max(1, apCount); marqueeOff = 0; drawPicker(); }
    if (M5.BtnA.wasHold())    { doScan(); marqueeOff = 0; drawPicker(); }
    if (M5.BtnB.wasHold() && apCount > 0) {   // drill into this AP's clients
      strncpy(clientApName, aps[sel].ssid, sizeof(clientApName) - 1);
      clientApName[sizeof(clientApName) - 1] = 0;
      startClients(aps[sel].bssid, aps[sel].channel);
      tool = T_CLIENTS; clientSel = 0; marqueeOff = 0; drawClients();
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
