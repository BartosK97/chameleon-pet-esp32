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
static_assert(S == 4, "present() writes 4-pixel rows as two 32-bit words");
constexpr uint32_t SAVE_MS = 5 * 60 * 1000;
static const char *TIMEZONE = "CET-1CEST,M3.5.0,M10.5.0/3";  // lamp schedule follows local time (Vienna/Berlin)
// QMI8658 chip axes to screen axes (+x right, +y down); flip signs/swap here if the water tilts the wrong way.
#define SCREEN_GX(ax, ay, az) (-(ay))
#define SCREEN_GY(ax, ay, az) (-(ax))
alignas(4) static uint16_t canvas[CW * CH];
static uint32_t beep_until = 0, next_sample = 0, next_log = 0, next_save = SAVE_MS, saved_ts = 0;
static bool was_down = false, net_up = false, clock_synced = false;
static uint32_t frames = 0, draw_us = 0, wait_us = 0, push_us = 0, pre_us = 0, paint_us = 0;  // per-second frame timing for the log
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
    configTzTime(TIMEZONE, "pool.ntp.org", "time.google.com");
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
static uint16_t *shown;  // last pushed canvas; in PSRAM, internal RAM is nearly gone with WiFi+BLE
static bool shown_valid = false;

static void present() {
  uint16_t *fb = board_fb();
  const uint32_t *now2 = (const uint32_t *)canvas, *old2 = (const uint32_t *)shown;
  for (int i = 0; i < CW * CH; i++) {
    if (shown_valid && !(i & 1) && now2[i / 2] == old2[i / 2]) {  // two unchanged pixels at once
      i++;
      continue;
    }
    uint16_t c = canvas[i];
    if (shown_valid && shown[i] == c) continue;
    shown[i] = c;
    uint32_t cc = c | (uint32_t)c << 16, *p = (uint32_t *)(fb + (i / CW) * S * LCD_W + (i % CW) * S);  // S == 4: two words per row
    for (int k = 0; k < S; k++, p += LCD_W / 2) p[0] = p[1] = cc;
  }
  shown_valid = true;
}

void setup() {
  Serial.begin(115200);
  for (uint32_t t = millis(); !Serial && millis() - t < 3000;) delay(10);
  shown = (uint16_t *)heap_caps_malloc(CW * CH * 2, MALLOC_CAP_SPIRAM);
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

  pet_init(millis(), (uint16_t *)heap_caps_malloc(CW * CH * 2, MALLOC_CAP_SPIRAM));
  prefs.begin("mini-pet");
  PetSave s;
  if (prefs.getBytes("pet", &s, sizeof s) == sizeof s) pet_load(s);
  saved_ts = prefs.getULong("ts", 0);
  logf("mini-pet ready\n");
}

// Paced by the panel (~42 Hz): push the frame drawn last time right after vsync, then draw the next one
// while the panel scans out; waiting and drawing no longer add up.
void loop() {
  static bool drawn = false;
  uint32_t tw = micros();
  board_wait_vsync();
  uint32_t tp = micros();
  if (drawn) present();
  wait_us += tp - tw, push_us += micros() - tp;

  uint32_t now = millis();
  net_loop(now);

  BLEScan scan = BLE.getScan();
  if (!scan.isScanning()) {  // short sessions so the library's result list of rotating addresses stays small
    scan.clearResults();
    scan.start(5000);
  }

  int tx = 0, ty = 0;
  bool down = board_touch(tx, ty);
  PetInput in = {};
  in.now = now, in.tap = down && !was_down, in.tx = tx / S, in.ty = ty / S;
  was_down = down;

  static float gx = 0, gy = 1, ax = 0, ay = 0, az = 1;
  static uint32_t shake_until = 0;
  if (board_accel(ax, ay, az)) {
    gx += (SCREEN_GX(ax, ay, az) - gx) * 0.15f, gy += (SCREEN_GY(ax, ay, az) - gy) * 0.15f;
    float jolt = fabsf(sqrtf(ax * ax + ay * ay + az * az) - 1);  // 0 at rest, whatever the orientation
    if (jolt > 0.9f && now > shake_until) in.shake = true, shake_until = now + 1500;
  }
  in.gx = gx, in.gy = gy;

  uint32_t tq = micros();
  static int minute = -1, day = -1;
  static uint32_t next_clock = 0;
  if (now >= next_clock) {  // localtime_r parses TZ on every call, so once a second is plenty
    next_clock = now + 1000;
    if (time_t wall = wall_clock()) {
      struct tm lt;
      localtime_r(&wall, &lt);
      minute = lt.tm_hour * 60 + lt.tm_min, day = lt.tm_year * 400 + lt.tm_yday;
    }
  }
  in.minute = minute, in.day = day;
  pre_us += micros() - tq;
  static int sig = -100, base = -100;
  if (now >= next_sample) {
    in.approach = sample_devices(now, sig, base);
    in.sample = true;
    next_sample = now + 250;
  }
  in.sig = sig;
  in.base = base;
  if (now >= next_log) {
    uint32_t f = frames ? frames : 1;
    logf("sig %d base %d | %u fps: clock %u, sim %u, paint %u, vsync wait %u, push %u us | acc %.2f %.2f %.2f | ram %u\n", sig,
         base, frames, pre_us / f, (draw_us - paint_us) / f, paint_us / f, wait_us / f, push_us / f, ax, ay, az,
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    frames = draw_us = wait_us = push_us = pre_us = paint_us = 0;
    next_log = now + 1000;
  }

  uint32_t t0 = micros();
  PetOutput out;
  pet_update(in, out);
  if (out.events & EV_FED) logf("food!\n");
  if (out.events & EV_DIED) logf("a fish died\n");
  if (out.events & EV_BABY) logf("a baby fish!\n");
  if (out.events) next_save = min(next_save, now + 5000);
  static int backlight = -1;
  if (abs(out.backlight - backlight) >= 2) board_backlight(backlight = out.backlight);

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

  uint32_t tpaint = micros();
  pet_draw(canvas, now);
  paint_us += micros() - tpaint;
  drawn = true;
  frames++, draw_us += micros() - t0;
}
