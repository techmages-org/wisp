// foxcore.h — board-agnostic Wi-Fi AP fox-hunt core (shared by every board build).
// Only ONE main_*.cpp is compiled per env (see build_src_filter), so the `static`
// state here lives in exactly one translation unit — no multiple-definition issue.
//
// The hardware-specific bits (display, buzzer/speaker, buttons) live in each
// board's main_*.cpp; this header owns the radio + signal logic:
//   scan APs → lock a MAC → promiscuous per-frame RSSI (filtered to the target's
//   transmitter addr2) → smoothing, peak-hold, and the Geiger cadence.
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <string.h>

namespace fox {

enum Mode { SCANNING, PICKING, LOCKED };

struct AP { uint8_t bssid[6]; char ssid[33]; int8_t rssi; uint8_t channel; };
static const int MAX_AP = 40;

// --- shared state (single TU per build) ---
static Mode     mode = SCANNING;
static AP       aps[MAX_AP];
static int      apCount = 0;
static int      sel = 0;
static uint8_t  targetMac[6];
static uint8_t  targetCh = 1;
static char     targetName[33] = "";
static volatile int      g_rssi   = -127;
static volatile uint32_t g_rssiMs = 0;
static volatile uint32_t g_frames = 0;
static float    smRssi = -90;     // smoothed RSSI
static int8_t   peakRssi = -127;  // closest approach
static uint32_t peakMs = 0;
static float    trendRef = -90;   // baseline for warmer/colder
static uint32_t trendMs = 0;

static const char* macStr(const uint8_t* m) {
  static char b[18];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return b;
}

// Geiger cadence: clamp[-90,-35] → -90 dBm = 1300 ms (slow blip) … -35 = 110 ms (fast chatter).
static inline uint32_t geigerInterval(float rssi) {
  int r = (int)constrain(rssi, -90.0f, -35.0f);
  return (uint32_t)map(r, -90, -35, 1300, 110);
}

// --- PROBES tool: nearby devices + the SSIDs their phones call out for --------
struct Probe { uint8_t mac[6]; char ssid[24]; int8_t rssi; uint8_t ch; uint32_t lastMs; };
static const int MAX_PROBE = 48;
static Probe   probes[MAX_PROBE];
static int     probeCount = 0;
static bool    probeCapturing = false;
static uint8_t hopCh = 1;

static void addProbe(const uint8_t* mac, const char* ssid, int8_t rssi, uint8_t ch) {
  for (int i = 0; i < probeCount; i++)
    if (memcmp(probes[i].mac, mac, 6) == 0 && strncmp(probes[i].ssid, ssid, sizeof(probes[i].ssid)) == 0) {
      probes[i].rssi = rssi; probes[i].ch = ch; probes[i].lastMs = millis();
      return;
    }
  if (probeCount >= MAX_PROBE) return;
  Probe& pr = probes[probeCount++];
  memcpy(pr.mac, mac, 6);
  strncpy(pr.ssid, ssid, sizeof(pr.ssid) - 1); pr.ssid[sizeof(pr.ssid) - 1] = 0;
  pr.rssi = rssi; pr.ch = ch; pr.lastMs = millis();
}

// Promiscuous rx callback. HUNT: keep frames TRANSMITTED BY the target (addr2 ==
// target MAC) for its RSSI. PROBES: parse probe-request frames (mgmt subtype 0x40)
// for the device MAC + the SSID it's searching for.
static void snifferCb(void* buf, wifi_promiscuous_pkt_type_t type) {
  const wifi_promiscuous_pkt_t* p = (const wifi_promiscuous_pkt_t*)buf;
  const uint8_t* pl = p->payload;
  if (probeCapturing) {
    if (pl[0] == 0x40) { // probe request
      char ssid[24] = {0};
      if (pl[24] == 0x00) { // tag 0 = SSID, starts after the 24-byte mgmt header
        int len = pl[25]; if (len > 23) len = 23;
        for (int i = 0; i < len; i++) {
          uint8_t c = pl[26 + i];
          ssid[i] = (c >= 32 && c < 127) ? c : '.';
        }
        ssid[len] = 0;
      }
      addProbe(pl + 10, ssid[0] ? ssid : "(any)", p->rx_ctrl.rssi, hopCh);
    }
    return;
  }
  if (memcmp(pl + 10, targetMac, 6) == 0) {
    g_rssi = p->rx_ctrl.rssi;
    g_rssiMs = millis();
    g_frames++;
  }
}

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

// Blocking AP scan → fills aps[] (sorted strongest-first), enters PICKING.
static void doScan() {
  mode = SCANNING;
  startSta();
  int n = WiFi.scanNetworks(false, true);
  apCount = 0;
  for (int i = 0; i < n && apCount < MAX_AP; i++) {
    AP& a = aps[apCount];
    memcpy(a.bssid, WiFi.BSSID(i), 6);
    String s = WiFi.SSID(i);
    s.toCharArray(a.ssid, sizeof(a.ssid));
    if (s.length() == 0) strcpy(a.ssid, "(hidden)");
    a.rssi = WiFi.RSSI(i);
    a.channel = WiFi.channel(i);
    apCount++;
  }
  WiFi.scanDelete();
  for (int i = 0; i < apCount; i++)
    for (int j = i + 1; j < apCount; j++)
      if (aps[j].rssi > aps[i].rssi) { AP t = aps[i]; aps[i] = aps[j]; aps[j] = t; }
  sel = 0;
  mode = PICKING;
}

static void lockSelection() {
  AP& a = aps[sel];
  memcpy(targetMac, a.bssid, 6);
  targetCh = a.channel ? a.channel : 1;
  strncpy(targetName, a.ssid, sizeof(targetName));
  startSniffer();
  mode = LOCKED;
}

// Call each loop while LOCKED. Updates smoothing + peak; sets live/rssi out-params;
// returns true on a NEW peak (closer than ever — fire the peak ping).
static bool updateSignal(bool& live, int& rssi) {
  uint32_t now = millis();
  live = (now - g_rssiMs) < 2000 && g_rssiMs != 0;
  rssi = g_rssi;
  bool newPeak = false;
  if (live) {
    smRssi = smRssi * 0.7f + rssi * 0.3f;
    if (rssi > peakRssi) { peakRssi = rssi; peakMs = now; newPeak = true; }
  }
  if (now - trendMs > 1200) { trendRef = smRssi; trendMs = now; }
  return newPeak;
}

// warmer/colder from the smoothed trend: +1 warmer, -1 colder, 0 steady.
static inline int trendDir() {
  float d = smRssi - trendRef;
  return d >= 2 ? 1 : d <= -2 ? -1 : 0;
}

// --- PROBES lifecycle -------------------------------------------------------
// NOTE: mirror startSniffer's order exactly — set WIFI_STA mode immediately
// before enabling promiscuous. Do NOT call WiFi.disconnect(true) here: the
// `true` (wifioff) flag runs esp_wifi_stop(), and a stopped radio makes the
// following esp_wifi_set_promiscuous(true) fail silently — the rx callback then
// never fires and the probe list stays empty no matter how long you hop.
static void startProbes() {
  esp_wifi_set_promiscuous(false);
  WiFi.mode(WIFI_STA);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(snifferCb);
  probeCapturing = true;
  probeCount = 0;
  hopCh = 1;
  esp_wifi_set_channel(hopCh, WIFI_SECOND_CHAN_NONE);
}

static void stopProbes() {
  esp_wifi_set_promiscuous(false);
  probeCapturing = false;
}

static void hopChannel() {
  hopCh = (hopCh % 13) + 1;
  esp_wifi_set_channel(hopCh, WIFI_SECOND_CHAN_NONE);
}

// Lock onto a captured device (the probing client) → switch to the HUNT meter on
// it. Same fox-hunt, but the target is a phone/laptop instead of an AP.
static void lockProbe(int idx) {
  if (idx < 0 || idx >= probeCount) return;
  memcpy(targetMac, probes[idx].mac, 6);
  targetCh = probes[idx].ch ? probes[idx].ch : 1;
  strncpy(targetName, macStr(probes[idx].mac), sizeof(targetName));
  stopProbes();
  startSniffer();
  mode = LOCKED;
}

} // namespace fox
