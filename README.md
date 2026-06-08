# APHound — ESP32-S3 Wi-Fi AP fox-hunter (Geiger locator)

A pocket "where's that AP?" tool. Lock onto an access point's MAC and walk it down
by ear: a buzzer **ticks faster as you get closer** (Geiger style), with a dBm /
signal-bar / warmer-colder / peak-hold readout on the screen. It's the Warlock
deck's AP locator, shrunk to a standalone battery gadget.

> Board: **LilyGo T-Display-S3** (ESP32-S3, 1.9" 170×320 LCD, 2 buttons, LiPo).
> Built with PlatformIO. Side project — rename `aphound` to whatever you like.

## How it works
ESP32-S3 **promiscuous mode**: the rx callback fires on every 802.11 frame and
hands us `rx_ctrl.rssi`. We keep only frames whose **transmitter address (addr2)
== the target MAC** — so the RSSI is the target's signal, attributed to the right
radio (the on-chip version of the deck's `wlan.ta` tshark filter). Cadence:
`-90 dBm → ~1300 ms (slow blips) … -35 dBm → ~110 ms (fast chatter)`.

## One part to add: a buzzer
No onboard speaker, so wire a **passive buzzer or piezo**:

```
 buzzer (+)  →  GPIO16   (PIN_BUZZER in src/main.cpp — change to any free GPIO)
 buzzer (–)  →  GND
```
A 3.3 V active buzzer also works but ignores the pitch (still ticks). For a real
"tick", a **passive** piezo + the firmware's `tone()` is best. The T-Display-S3
breaks out 16/17/18/21/etc. on the side header — any free one works.

## Flash it
```bash
# from this folder, board plugged in over USB-C:
pio run -t upload          # build + flash
pio device monitor         # 115200, optional serial log
```
First build downloads the ESP32-S3 toolchain (a few minutes, once).

## Use it (2 buttons)
- **BTN2 (GPIO14)** — lock onto the highlighted AP → homing meter starts.
- **BTN1 (GPIO0 / BOOT)** — next AP in the list · while locked: **mute** the buzzer.
- **hold BTN1** — rescan.
- **BTN2 while locked** — back to the list to pick another target.

Boot → it scans → pick the AP → lock → walk toward it. The tick speeds up as you
close in; a sharper double-beep fires each time you hit a **new peak** (you just
got closer than ever — the "you passed it / you're on it" cue).

## Gotchas (T-Display-S3)
- **Black screen?** `GPIO15` (PWR_ON) must be HIGH — the firmware does this in
  `setup()`. If you fork the display code, keep it.
- **Wrong colors / inverted?** Flip `-DTFT_RGB_ORDER` (TFT_RGB↔TFT_BGR) or toggle
  `-DTFT_INVERSION_ON` in `platformio.ini`.
- **No sound?** Confirm the buzzer is **passive** and on `PIN_BUZZER`; active
  buzzers self-oscillate and won't change pitch. Check `muted` (BTN1 toggles it).
- **Battery volts off?** The divider/ADC cal varies a little per board; tweak
  `batteryVolts()`.

## Roadmap (matches the deck)
- Pre-load a known **rogue MAC** (skip the picker) for targeted sweeps.
- A second "search mode": steady tone whose **pitch** tracks signal (sonar).
- Lock onto a **client** MAC, not just an AP (already MAC-generic in the callback —
  just needs the channel).
