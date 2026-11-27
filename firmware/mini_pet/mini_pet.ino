// Chameleon pet: touch pets it, a phone brought close feeds it (per-device BLE RSSI rise). WiFi gives OTA + a TCP log.
#include <stdarg.h>
#include <ArduinoOTA.h>
#include <BLE.h>
#include <WiFi.h>
#include "board.h"
#include "pet.h"
#if __has_include("secrets.h")
#include "secrets.h"  // WIFI_SSID, WIFI_PASS; gitignored, see secrets.example.h
#endif

constexpr int S = LCD_W / CW;  // canvas scale, 8x
static uint16_t canvas[CW * CH];
static uint32_t beep_until = 0, next_sample = 0, next_log = 0;
static bool was_down = false, net_up = false;

static NetworkServer log_server(23);
static NetworkClient log_client;

static void logf(const char *fmt, ...) {
  char b[128];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  Serial.print(b);
  if (log_client.connected()) log_client.print(b);
}

// Per-device RSSI: fast follows each advert, slow is a ~4 s baseline. A device rising well above its own
// baseline is someone bringing a phone closer; devices that just sit nearby never trigger.
struct Dev { uint32_t key; float fast, slow; uint32_t seen; };
static Dev devs[24];
static portMUX_TYPE devs_mux = portMUX_INITIALIZER_UNLOCKED;

static void on_advert(const BLEAdvertisedDevice &d) {
  const uint8_t *a = d.getAddress().data();
  uint32_t key = ((uint32_t)a[0] | a[1] << 8 | a[2] << 16 | (uint32_t)a[3] << 24) ^ ((uint32_t)a[4] << 5 | (uint32_t)a[5] << 13);
  if (!key) key = 1;
  float r = d.getRSSI();
  uint32_t now = millis();
  taskENTER_CRITICAL(&devs_mux);
  Dev *slot = nullptr, *oldest = &devs[0];
  for (auto &v : devs) {
    if (v.key == key) { slot = &v; break; }
    if (v.seen < oldest->seen) oldest = &v;
  }
  if (slot) slot->fast = slot->fast * 0.6f + r * 0.4f;
  else *(slot = oldest) = {key, r, r, now};
  slot->seen = now;
  taskEXIT_CRITICAL(&devs_mux);
}

// Called every 250 ms: ages baselines, reports the strongest device and whether any device just approached.
static bool sample_devices(uint32_t now, int &sig, int &base) {
  bool approach = false;
  float best = -200, rise = 0;
  sig = base = -100;
  taskENTER_CRITICAL(&devs_mux);
  for (auto &v : devs) {
    if (!v.key) continue;
    if (now - v.seen > 10000) { v.key = 0; continue; }
    if (now - v.seen > 2000) continue;
    if (!approach && v.fast >= APPROACH_MIN && v.fast - v.slow >= APPROACH_RISE) {
      approach = true;
      rise = v.fast - v.slow;
      v.slow = v.fast;  // consumed: it has to move away and come back to feed again
    }
    v.slow += (v.fast - v.slow) * 0.06f;
    if (v.fast > best) best = v.fast, sig = (int)v.fast, base = (int)v.slow;
  }
  taskEXIT_CRITICAL(&devs_mux);
  if (approach) logf("approach +%.0f dB\n", rise);
  return approach;
}

static void net_loop(uint32_t now) {
#ifdef WIFI_SSID
  if (!net_up && WiFi.status() == WL_CONNECTED) {
    net_up = true;
    ArduinoOTA.setHostname("mini-pet");
    ArduinoOTA.begin();
    log_server.begin();
    logf("wifi %s  ota: mini-pet.local  log: nc mini-pet.local 23\n", WiFi.localIP().toString().c_str());
  }
  if (!net_up) return;
  ArduinoOTA.handle();
  if (log_server.hasClient()) {
    log_client = log_server.accept();
    logf("log attached\n");
  }
#endif
}

// Only changed blocks touch PSRAM: full 600 KB redraws starve the panel's scanout once BLE runs.
static uint16_t shown[CW * CH];
static bool shown_valid = false;

static void present() {
  uint16_t *fb = board_fb();
  for (int i = 0; i < CW * CH; i++) {
    uint16_t c = canvas[i];
    if (shown_valid && shown[i] == c) continue;
    shown[i] = c;
    uint16_t *p = fb + (i / CW) * S * LCD_W + (i % CW) * S;
    for (int k = 0; k < S; k++, p += LCD_W)
      for (int j = 0; j < S; j++) p[j] = c;
  }
  shown_valid = true;
}

void setup() {
  Serial.begin(115200);
  for (uint32_t t = millis(); !Serial && millis() - t < 3000;) delay(10);
  if (!board_init()) logf("display init failed\n");

#ifdef WIFI_SSID
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("mini-pet");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
#endif

  if (!BLE.begin("mini-pet")) logf("BLE init failed\n");
  BLEScan scan = BLE.getScan();
  scan.setActiveScan(false);
  scan.setInterval(100);
  scan.setWindow(99);
  scan.setFilterDuplicates(false);
  scan.onResult(on_advert);

  pet_init(millis());
  logf("mini-pet ready\n");
}

void loop() {
  uint32_t now = millis();
  net_loop(now);

  BLEScan scan = BLE.getScan();
  if (!scan.isScanning()) {  // short sessions so the library's result list of rotating addresses stays small
    scan.clearResults();
    scan.start(5000);
  }

  int tx = 0, ty = 0;
  bool down = board_touch(tx, ty);
  PetInput in = {now, down && !was_down, tx / S, ty / S, false, -100, -100, false};
  was_down = down;
  static int sig = -100, base = -100;
  if (now >= next_sample) {
    in.approach = sample_devices(now, sig, base);
    in.sample = true;
    next_sample = now + 250;
  }
  in.sig = sig;
  in.base = base;
  if (now >= next_log) {
    logf("sig %d base %d\n", sig, base);
    next_log = now + 1000;
  }

  PetOutput out;
  pet_update(in, out);
  if (out.beep_ms == 15) logf("fly!\n");
  if (out.beep_ms) {
    board_buzzer(true);
    beep_until = now + out.beep_ms;
  }
  if (beep_until && now > beep_until) {
    board_buzzer(false);
    beep_until = 0;
  }

  pet_draw(canvas, now);
  present();
  delay(30);
}
