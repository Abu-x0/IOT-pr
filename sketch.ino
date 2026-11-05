#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// ---------------- Simulation control ----------------
#define AUTO_SIMULATION true   // true: model generates data, false: Wokwi sliders
#define USE_DEEP_SLEEP  false  // Wokwi loses RTC state on deep sleep; drop the Wi-Fi link instead

const uint32_t SLEEP_SECONDS = 20;     // stands for 6 h in the model (ThingSpeak needs >= 15 s)
const int      TRIAL_SLOTS   = 28;     // 7 days x 4 slots/day (2 - 8 May 2024)
const int      SLOTS_PER_DAY = 4;

// ---------------- Pins ----------------
const int pot15Pin = 32;
const int pot30Pin = 33;
const int pot45Pin = 34;
const int pot60Pin = 35;
const int ledPin   = 26;   // ON = irrigation advised (paper: manual valve)

// ---------------- Network / ThingSpeak ----------------
const char* ssid     = "Wokwi-GUEST";
const char* password = "";
String apiKey = "YT4GCL1TAHOJOQ22";   // put your own key here, never publish it

// ThingSpeak channel field map (all VWC values are in m3/m3):
//   field1 = 15 cm   field2 = 30 cm   field3 = 45 cm   field4 = 60 cm
//   field5 = irrigation advised: 1 = YES, 0 = NO

// ---------------- Paper thresholds (Section 2.4) ----------------
const float VWC_LOW  = 0.25;   // start irrigation
const float VWC_HIGH = 0.30;   // stop irrigation

// ---------------- Persistent state (kept across a wake) ----------------
RTC_DATA_ATTR int   slotNo      = 0;
RTC_DATA_ATTR float vwc15s      = 0.27;
RTC_DATA_ATTR float vwc30s      = 0.27;
RTC_DATA_ATTR float vwc45s      = 0.25;
RTC_DATA_ATTR float vwc60s      = 0.24;
RTC_DATA_ATTR bool  irrigating  = false;
RTC_DATA_ATTR int   events      = 0;     // irrigation events (paper rate: ~0.5/day)
RTC_DATA_ATTR int   pumpSlots   = 0;     // slots with irrigation on
RTC_DATA_ATTR int   uploadOK    = 0;
RTC_DATA_ATTR int   uploadFail  = 0;

bool pumpOn = false;   // irrigation active during the current slot

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Wokwi potentiometer (ADC 0..4095) -> volumetric water content (m3/m3).
// The mapping is assumed linear and inverted: a higher wiper voltage means drier
// soil, so ADC 0 -> 0.460 (wet) and ADC 4095 -> 0.050 (dry).
float getVWCFromPot(int rawADC) {
  return map(rawADC, 0, 4095, 460, 50) / 1000.0;
}

// ---------------- Soil model (AUTO mode) ----------------
void updateSoilModel(float &v15, float &v30, float &v45, float &v60) {
  // 1) drying during the last 6 h (ET uptake and drainage)
  //    15 cm rate calibrated to reproduce the paper's 24 events (plot B)
  v15 -= 0.0085; v30 -= 0.004;  v45 -= 0.002;  v60 -= 0.001;

  // 2) rain (2 days kept from the paper): 3 May = day 1, 7 May = day 5 (2 May = day 0)
  int day = slotNo / SLOTS_PER_DAY;
  float rainMm = 0;
  if (slotNo % SLOTS_PER_DAY == 0) {
    if (day == 1) rainMm = 14.75;
    if (day == 5) rainMm = 11.33;
  }
  if (rainMm > 0) {
    // Rain -> VWC response (assumed): the surface layer gains the most, deeper
    // layers less, using a per-mm gain that decays with depth.
    v15 += rainMm * 0.004;  v30 += rainMm * 0.003;
    v45 += rainMm * 0.0015; v60 += rainMm * 0.001;
    Serial.printf("[Rain] %.2f mm\n", rainMm);
  }

  // 3) irrigation rule on the 15 cm sensor: start below 0.25, stop at 0.30
  if (v15 < VWC_LOW && !irrigating) {
    events++;
    irrigating = true;
  }
  pumpOn = irrigating;

  if (irrigating) {
    pumpSlots++;
    // Water applied by drip while the valve is open: each active 6 h slot raises
    // the 15 cm layer by 0.03 m3/m3, with smaller (assumed) gains deeper down.
    v15 += 0.03;  v30 += 0.013;  v45 += 0.0065; v60 += 0.003;
    if (v15 >= VWC_HIGH) {      // irrigation is cut off at the upper threshold
      v15 = VWC_HIGH;
      irrigating = false;
    }
  }

  v15 = clampf(v15, 0.05, 0.46);  v30 = clampf(v30, 0.05, 0.46);
  v45 = clampf(v45, 0.05, 0.46);  v60 = clampf(v60, 0.05, 0.46);
}

// ---------------- Wi-Fi with the paper's 30 s timeout ----------------
bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.begin(ssid, password);
  for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);                       // 60 x 500 ms = 30 s
  }
  return WiFi.status() == WL_CONNECTED;
}

// ---------------- One 6-hour measurement slot ----------------
// Sequence: read/generate the four VWC values -> apply the plot B rule and drive
// the LED -> connect Wi-Fi (30 s timeout) -> upload the five fields to ThingSpeak.
void runCycle() {
  float v15, v30, v45, v60;

  if (AUTO_SIMULATION) {
    // Advance the assumed soil model and persist the new moisture values
    v15 = vwc15s; v30 = vwc30s; v45 = vwc45s; v60 = vwc60s;
    updateSoilModel(v15, v30, v45, v60);
    vwc15s = v15; vwc30s = v30; vwc45s = v45; vwc60s = v60;
  } else {
    // Manual mode: read the four Wokwi potentiometers and apply the rule directly
    v15 = getVWCFromPot(analogRead(pot15Pin));
    v30 = getVWCFromPot(analogRead(pot30Pin));
    v45 = getVWCFromPot(analogRead(pot45Pin));
    v60 = getVWCFromPot(analogRead(pot60Pin));
    if (v15 < VWC_LOW && !irrigating) { events++; irrigating = true; }
    if (v15 >= VWC_HIGH) irrigating = false;
    pumpOn = irrigating;
    if (pumpOn) pumpSlots++;
  }
  digitalWrite(ledPin, pumpOn ? HIGH : LOW);

  Serial.printf("\n=== Slot %d / %d (day %d) ===\n", slotNo + 1, TRIAL_SLOTS, slotNo / SLOTS_PER_DAY);
  Serial.printf("VWC 15/30/45/60 cm: %.3f / %.3f / %.3f / %.3f m3/m3\n", v15, v30, v45, v60);
  Serial.printf("Irrigation advised: %s\n", pumpOn ? "YES" : "NO");

  // ---- Wi-Fi + upload ----
  uint32_t tConn = millis();
  bool online = connectWiFi();
  uint32_t connMs = millis() - tConn;

  if (!online) {
    // 30 s timeout expired: skip this slot's upload, exactly as the paper does
    Serial.println("Wi-Fi timeout (30 s) -> skip upload, back to sleep (as in the paper)");
    uploadFail++;
  } else {
    Serial.printf("Wi-Fi connected in %lu ms\n", (unsigned long)connMs);
    // HTTPS GET (ThingSpeak's documented form); setInsecure() skips certificate checks in the simulator
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    String url = "https://api.thingspeak.com/update?api_key=" + apiKey
               + "&field1=" + String(v15, 3)
               + "&field2=" + String(v30, 3)
               + "&field3=" + String(v45, 3)
               + "&field4=" + String(v60, 3)
               + "&field5=" + String(pumpOn ? 1 : 0);
    if (http.begin(client, url)) {
      int code = http.GET();
      String resp = http.getString();   // ThingSpeak returns the entry id; "0" = rejected
      Serial.printf("HTTP %d, ThingSpeak reply: %s\n", code, resp.c_str());
      if (code == 200 && resp != "0") uploadOK++; else uploadFail++;
      if (code < 0) Serial.printf("HTTP error: %s\n", http.errorToString(code).c_str());
      http.end();
    } else {
      Serial.println("http.begin() failed");
      uploadFail++;
    }
  }

  slotNo++;
  // Emulate the paper's wake cycle without deep sleep: drop the link so the
  // next runCycle() reconnects and the 30 s Wi-Fi timeout is exercised.
  WiFi.disconnect(true);
}

void printSummary() {
  Serial.println("\n############ TRIAL SUMMARY (7 days) ############");
  Serial.printf("Irrigation events: %d (paper rate ~0.5/day)\n", events);
  Serial.printf("Slots with irrigation on: %d\n", pumpSlots);
  Serial.printf("Uploads OK / failed: %d / %d\n", uploadOK, uploadFail);
}

void setup() {
  Serial.begin(115200);
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);

  // If the whole 46-day trial is already done, print the summary and stay asleep
  if (slotNo >= TRIAL_SLOTS) {
    printSummary();
    esp_deep_sleep_start();           // no wake source: stay asleep
  }

  runCycle();

  if (USE_DEEP_SLEEP) {
    Serial.printf("Deep sleep for %lu s (stands for 6 h)\n", (unsigned long)SLEEP_SECONDS);
    Serial.flush();
    esp_sleep_enable_timer_wakeup(SLEEP_SECONDS * 1000000ULL);
    esp_deep_sleep_start();
  }
}

void loop() {
  // only reached when USE_DEEP_SLEEP is false
  if (!USE_DEEP_SLEEP) {
    delay(SLEEP_SECONDS * 1000UL);           // stand-in for the 6 h sleep
    if (slotNo >= TRIAL_SLOTS) { printSummary(); while (true) delay(1000); }
    runCycle();
  }
}
