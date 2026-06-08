<div align="center">

<img src="assets/wisp.png" alt="Wisp" width="180" />

# Wisp

### A pocket Wi-Fi will-o'-the-wisp 🔵

**Lock an access point — or a single device — by MAC, then walk it down by ear.**
A buzzer that ticks faster as you close in, a radar "getting warmer" screen, and
peak-hold — all on a battery-powered ESP32 that fits in your palm.

<br>

![platform ESP32](https://img.shields.io/badge/platform-ESP32-3C3C3C?style=for-the-badge&logo=espressif&logoColor=white&labelColor=0A0B10)
![PlatformIO](https://img.shields.io/badge/build-PlatformIO-FF7F00?style=for-the-badge&logo=platformio&logoColor=white&labelColor=0A0B10)
![Arduino](https://img.shields.io/badge/framework-Arduino-00979D?style=for-the-badge&logo=arduino&logoColor=white&labelColor=0A0B10)
![white-hat](https://img.shields.io/badge/security-white--hat-5FEFA0?style=for-the-badge&labelColor=0A0B10)
![license MIT](https://img.shields.io/badge/license-MIT-9B8CF0?style=for-the-badge&labelColor=0A0B10)

*The handheld sibling to the [**Warlock**](https://github.com/techmages-org/warlock) cyberdeck — rogue-AP hunting, in your hand.*
**An open project of [🧙‍♂️ TechMages](https://github.com/techmages-org).**

</div>

---

## What is it?

Ever needed to find *where* a Wi-Fi signal is actually coming from? A rogue AP
plugged into a closet. A pineapple in the ceiling tiles. A device that keeps
beaconing and you don't know which desk it's on.

**Wisp turns radio signal strength into sound and motion.** Pick a target, and the
closer you walk, the faster it ticks — like a Geiger counter for Wi-Fi. Watch the
radar ring pulse quicker, the dBm climb, the **WARMER / COLDER** readout flip. When
you hit a new closest-approach, it fires a sharp double-beep: *you're on it.*

It's the [Warlock deck's](https://github.com/techmages-org/warlock) AP locator,
shrunk down to a thing you can clip to a lanyard and hand to a teammate.

> 🛡 **White-hat by design.** Wisp is a *receiver* — it listens to signal strength
> on networks you're authorized to survey. It doesn't connect, deauth, inject, or
> capture traffic. Hunt your own APs, or ones you have written permission to find.

## ✨ Three tools in one

| Mode | What it does |
|------|--------------|
| 🎯 **HUNT** | Scan the air, **lock an AP by MAC**, and home in. Geiger ticks + radar + dBm + warmer/colder + peak-hold. The core fox-hunt. |
| 📡 **PROBES** | Sniff **probe requests** — every nearby phone/laptop *searching* for networks, with the SSID it's hunting and its signal. **Lock one** and fox-hunt *that device* instead of an AP. |
| 👥 **CLIENTS** | Pick an AP and see the **stations actually associated to it** (read live from its data frames, parked on its channel). **Lock one** and walk down that specific connected device. |

**PWR** flips HUNT ⇄ PROBES. **Hold B** on an AP in the picker drills into its **CLIENTS**. Any list → lock a target (**B**) and the Geiger meter takes over.

## 🔧 Boards

Wisp builds for two boards from one codebase — the radio + signal logic lives in a
shared `foxcore.h`; each board has its own thin `main`.

<table>
<tr>
<td width="50%" valign="top">

### ⭐ M5StickC Plus 1.1 *(recommended)*
**`m5stickc-plus`** · zero wiring

ESP32-PICO, built-in color LCD, buttons, **buzzer**, and **LiPo battery** — it's a
finished gadget out of the box. Also runs on the **M5 Cardputer** and other M5
boards (M5Unified auto-detects). This is the one to start with.

</td>
<td width="50%" valign="top">

### LilyGo T-Display-S3
**`tdisplay-s3`** · add one buzzer

ESP32-S3 with a bright 1.9″ 170×320 LCD and two buttons. No onboard speaker — wire
a **passive piezo** to a free GPIO (default `GPIO16`) for the ticks. Bigger screen,
USB-C, internal LiPo connector.

</td>
</tr>
</table>

## 🚀 Build & flash

Install [PlatformIO](https://platformio.org/install) (the CLI or the VS Code
extension), plug the board in over USB, and:

```bash
# M5StickC Plus 1.1  (recommended — nothing to wire)
pio run -e m5stickc-plus -t upload

# LilyGo T-Display-S3  (add a passive buzzer on GPIO16)
pio run -e tdisplay-s3 -t upload

# optional serial log (115200)
pio device monitor
```

The first build downloads the ESP32 toolchain (a few minutes, once). That's it —
the device boots straight into a scan.

## 🎮 Controls

**M5StickC Plus** — A (big front button) · B (side) · PWR (power)

| | HUNT — picking | HUNT — locked | PROBES | CLIENTS |
|---|---|---|---|---|
| **A** | next AP | mute buzzer | next device | next station |
| **hold A** | rescan | — | clear list | back to APs |
| **B** | lock & hunt AP | back to list | lock device → hunt it | lock station → hunt it |
| **hold B** | this AP's **clients** | — | — | — |
| **PWR** | → PROBES | → PROBES | → HUNT | → AP picker |

**T-Display-S3** — BTN1 (GPIO0/BOOT) = next / mute · hold = rescan · BTN2 (GPIO14) = lock / back.

Boot → it scans → pick a target → lock → walk toward it. The tick quickens as you
close in; the double-beep means **new peak** — you just got closer than ever.

## 🧠 How it works

ESP32 **promiscuous mode**: the rx callback fires on every 802.11 frame and hands us
`rx_ctrl.rssi`. In HUNT we keep only frames whose **transmitter address (addr2) ==
the target MAC**, so the RSSI is *that radio's* signal, attributed correctly — the
on-chip version of the deck's `wlan.ta` tshark filter. In PROBES we parse management
**probe-request** frames (subtype `0x40`) for the device MAC and the SSID it's
searching for, channel-hopping to sweep the band. In CLIENTS we park on the chosen
AP's channel and read **data frames** — the `ToDS`/`FromDS` bits tell us which
address is the station, so a frame *to* the AP reveals the client as `addr2` and a
frame *from* the AP reveals it as `addr1`. Lock any of these and the hunt is the same
math — only the target MAC changes.

Signal → cadence is a simple map, smoothed and peak-held:

```
-90 dBm  →  ~1300 ms   (slow, distant blips)
-35 dBm  →  ~110 ms    (fast, you're-right-on-it chatter)
```

## 🤝 Contributing

Wisp is open and we'd love your help. It shares the
[**TechMages**](https://github.com/techmages-org) white-hat, authorization-first
ethos — please read the
[**Contributing guide**](https://github.com/techmages-org/techmages/blob/main/CONTRIBUTING.md)
and the
[**Code of Ethics**](https://github.com/techmages-org/techmages/blob/main/CODE_OF_ETHICS.md)
before opening a PR. Both are non-negotiable.

### 🙋 How to help

No matter your level, there's a way in:

- 🔌 **Port it to another board** — got an M5 Stamp, an ESP32-C3, a Heltec? Add an
  env + a thin `main`. `foxcore.h` already does the radio.
- 🎨 **New search modes** — a sonar pitch-sweep, a vibration motor, an RGB "hot/cold"
  LED bar, a screen visual. The cadence math is one function away.
- 🐛 **Field reports** — tried it on a real hunt? Open an issue with what worked,
  what didn't, and your board. Real-world RSSI behavior is gold.
- 📖 **Docs & photos** — wiring diagrams, a build photo, a demo GIF of the radar
  closing in. Show people what it looks like.
- 💡 **Ideas** — pre-loaded rogue-MAC lists, a logging mode, a deck handoff. Open an
  issue and let's talk.

Pick up an [open issue](https://github.com/techmages-org/wisp/issues) or open a new
one — even a question helps.

### Contributors

<a href="https://github.com/techmages-org/wisp/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=techmages-org/wisp" alt="Wisp contributors" />
</a>

## 📜 License

MIT — see [`LICENSE`](LICENSE). Use it, fork it, build on it. Hunt responsibly.

---

<div align="center">

*The deck is the [Warlock](https://github.com/techmages-org/warlock). The org is [TechMages](https://github.com/techmages-org). The wisp lights the way.* 🔵

</div>
