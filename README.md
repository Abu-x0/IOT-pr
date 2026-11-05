# Smart Irrigation Node — Wokwi Simulation

A low-cost ESP32 soil-moisture node that follows the irrigation rule from
**Abdelmoneim et al., _Sensors_ 2025, 25, 1568** — keeping the 15 cm soil layer
between **0.25 and 0.30 m³/m³** — and uploads four depth readings to ThingSpeak.
This project reproduces that experiment in the **Wokwi** simulator, compressing a
7-day trial into roughly 10 minutes of real time.

## What the simulation shows

- Four virtual capacitive probes at **15, 30, 45 and 60 cm** (GPIO 32–35).
- Soil-moisture dynamics: **drying**, **rain** on two days, and **drip irrigation**
  triggered by the 0.25 / 0.30 thresholds.
- A **blue LED** on GPIO 26 lights whenever the rule advises irrigation.
- Every slot connects to Wi-Fi (30 s timeout) and logs 5 fields to ThingSpeak.

## How it works

One loop = one **6-hour slot** (20 s in the simulation). A 7-day trial is
**28 slots**. Each slot the node:

1. Advances the assumed soil model for all four depths.
2. Applies the plot-B rule on the **15 cm** sensor (start < 0.25, stop at 0.30).
3. Drives the LED.
4. Connects to Wi-Fi within 30 s and uploads the readings.

Rain is applied at the first slot of **3 May (14.75 mm)** and **7 May (11.33 mm)**.

## Parameters and assumptions

| Item | Value |
|---|---|
| Slot length | 6 h (20 s simulated) |
| Trial length | 7 days / 28 slots |
| Thresholds (15 cm) | 0.25 – 0.30 m³/m³ |
| Initial VWC 15/30/45/60 | 0.27 / 0.27 / 0.25 / 0.24 |
| Drying per slot | 0.0085 / 0.004 / 0.002 / 0.001 |
| Irrigation gain per slot | 0.03 / 0.013 / 0.0065 / 0.003 |
| Rain response (per mm) | 0.004 / 0.003 / 0.0015 / 0.001 |
| Wi-Fi timeout | 30 s |

The drying and irrigation gains are **assumptions** (the paper reports rain and
the control rule, not these rates). They are calibrated so the model reproduces
the paper's cadence of about **0.5 irrigation events per day** (24 events over
its 46-day plot-B trial).

## ThingSpeak fields

| Field | Meaning |
|---|---|
| field1 | VWC at 15 cm (m³/m³) |
| field2 | VWC at 30 cm |
| field3 | VWC at 45 cm |
| field4 | VWC at 60 cm |
| field5 | Irrigation advised: 1 = yes, 0 = no |

## Files

- `sketch.ino` — firmware for the ESP32 (Arduino core).
- `diagram.json` — Wokwi circuit (ESP32 + 4 potentiometers + LED).
- `README.md` — this file.

## How to run

1. Open the project in Wokwi (or import `sketch.ino` + `diagram.json`).
2. Press **▶ Start the simulation**.
3. Watch the Serial monitor; the trial finishes at slot 28.

- Live project: `https://wokwi.com/projects/477678407076745217`
- Live data (public ThingSpeak channel): `https://thingspeak.mathworks.com/channels/3532920`

## Expected result

```
############ TRIAL SUMMARY (7 days) ############
Irrigation events: 3 (paper rate ~0.5/day)
Slots with irrigation on: 7
Uploads OK / failed: 28 / 0
```

## Notes and limitations

- **Deep sleep is disabled.** Wokwi resets the chip and loses `RTC_DATA_ATTR` on
  deep sleep, which would restart the trial at slot 1. Instead the Wi-Fi link is
  dropped after each slot, so every cycle reconnects and the 30 s timeout path is
  exercised.
- **RSSI is not reported.** It is simulated (not physical) in Wokwi, so it was
  removed; only fields 1–5 are uploaded.
- **API key:** replace the key in `sketch.ino` with your own ThingSpeak write key.
  Never commit a real key to a public repository.