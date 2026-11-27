// Aquarium: a phone brought close drops food (per-device BLE RSSI rise), unfed fish die after 24 h.
// WiFi gives OTA, a TCP log and NTP time so hunger keeps counting while powered off.
#include <stdarg.h>
#include <time.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <BLE.h>
#include <WiFi.h>
#include "board.h"
#include "pet.h"
#if __has_include("secrets.h")
#include "secrets.h"  // WIFI_SSID, WIFI_PASS; gitignored, see secrets.example.h
#endif

constexpr int S = LCD_W / CW;  // canvas scale, 4x
constexpr uint32_t FRAME_MS = 1000 / 24, SAVE_MS = 5 * 60 * 1000;
static uint16_t canvas[CW * CH];
static uint32_t beep_until = 0, next_sample = 0, next_log = 0, next_frame = 0, next_save = SAVE_MS, saved_ts = 0;
static bool was_down = false, net_up = false, clock_synced = false;
static Preferences prefs;

static uint32_t wall_clock() {
  time_t t = time(nullptr);
  return t > 1700000000 ? (uint32_t)t : 0;
}

// Flash writes stall the panel for a moment, so only every few minutes or right after something changed.
static void save_state() {
  PetSave s;
  pet_get_save(s);
  prefs.putBytes("pet", &s, sizeof s);
  prefs.putULong("ts", wall_clock());
}

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
    configTime(0, 0, "pool.ntp.org", "time.google.com");
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
  WiFi.persistent(false);  // no NVS writes from the WiFi stack
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
  prefs.begin("mini-pet");
  PetSave s;
  if (prefs.getBytes("pet", &s, sizeof s) == sizeof s) pet_load(s);
  saved_ts = prefs.getULong("ts", 0);
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
  if (out.fed) logf("food!\n");
  if (out.fed || out.beep_ms == 300) next_save = min(next_save, now + 5000);

  if (!clock_synced && wall_clock()) {  // catch up on the time spent powered off
    clock_synced = true;
    uint32_t wall = wall_clock();
    if (saved_ts && wall > saved_ts) pet_advance(wall - saved_ts);
    logf("clock synced, %lu s since last save\n", saved_ts ? (unsigned long)(wall - saved_ts) : 0UL);
    save_state();
  }
#ifdef WIFI_SSID
  bool can_save = clock_synced;
#else
  bool can_save = true;
#endif
  if (can_save && now >= next_save) {
    save_state();
    next_save = now + SAVE_MS;
  }
  if (out.beep_ms) {
    board_buzzer(true);
    beep_until = now + out.beep_ms;
  }
  if (beep_until && now > beep_until) {
    board_buzzer(false);
    beep_until = 0;
  }

  pet_draw(canvas, now);
  board_wait_vsync();
  present();
  next_frame += FRAME_MS;
  int32_t wait = (int32_t)(next_frame - millis());
  if (wait > 0) delay(wait);
  else next_frame = millis();
}
