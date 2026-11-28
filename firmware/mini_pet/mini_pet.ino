// Aquarium: a phone brought close drops food (per-device BLE RSSI rise), unfed fish die after 24 h.
// WiFi gives OTA, a TCP log and NTP time so hunger keeps counting while powered off.
#include <stdarg.h>
#include <time.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <BLE.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <WiFi.h>
#include "ai.h"
#include "board.h"
#include "pet.h"
#include "talk.h"
#include "web_page.h"
#if __has_include("secrets.h")
#include "secrets.h"  // WIFI_SSID, WIFI_PASS; gitignored, see secrets.example.h
#endif

constexpr int S = LCD_W / CW;  // canvas scale, 4x
static_assert(S == 4, "present() writes 4-pixel rows as two 32-bit words");
constexpr uint32_t SAVE_MS = 5 * 60 * 1000;
static const char *TIMEZONE = "CET-1CEST,M3.5.0,M10.5.0/3";  // lamp schedule follows local time (Vienna/Berlin)
// QMI8658 chip axes to screen axes (+x right, +y down); flip signs/swap here if the water tilts the wrong way.
#define SCREEN_GX(ax, ay, az) (-(ay))
#define SCREEN_GY(ax, ay, az) (ax)
alignas(4) static uint16_t canvas[CW * CH];
static uint32_t beep_until = 0, next_sample = 0, next_log = 0, next_alert = 0, next_save = SAVE_MS, saved_ts = 0;
static bool was_down = false, net_up = false, clock_synced = false;
static const uint32_t SLEEP_MS = 5 * 60 * 1000;  // screen goes dark after this long without touch, phone, movement or web use
static volatile uint32_t last_active = 0;
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
static WebServer web(80);
static SemaphoreHandle_t pet_mux;  // the web task (core 0) and the frame loop (core 1) share the pet state and canvas
static uint16_t *snapshot;  // copy of the canvas the web task sends without holding the lock
static uint32_t last_fps = 0;
static NetworkClient log_client;

static SemaphoreHandle_t log_mux;  // the net task logs too

static void logf(const char *fmt, ...) {
  char b[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (log_mux) xSemaphoreTake(log_mux, portMAX_DELAY);
  Serial.print(b);
  if (log_client.connected()) log_client.print(b);
  if (log_mux) xSemaphoreGive(log_mux);
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

// ---- BLE on/off: TLS needs the ~90 KB NimBLE holds, so network jobs switch BLE off for a few seconds ----
static SemaphoreHandle_t ble_mux;
static volatile bool ble_on = false;

static void ble_set(bool on) {
  xSemaphoreTake(ble_mux, portMAX_DELAY);
  if (on && !ble_on) {
    if (BLE.begin("mini-pet")) {
      BLEScan scan = BLE.getScan();
      scan.setActiveScan(false);
      scan.setInterval(100);
      scan.setWindow(99);
      scan.setFilterDuplicates(false);
      scan.onResult(on_advert);
      ble_on = true;
    } else {
      logf("BLE init failed\n");
    }
  } else if (!on && ble_on) {
    ble_on = false;
    BLE.getScan().stop();
    BLE.end();
  }
  xSemaphoreGive(ble_mux);
}

// ---- Fish talk: names, speech bubbles, diary, chat and pushes ----
#ifdef ANTHROPIC_KEY
static const char *api_key = ANTHROPIC_KEY;
#else
static const char *api_key = nullptr;  // no key: fish still talk, from the canned lines below
#endif
static const char *SPECIES_DE[] = {"Clownfisch", "Doktorfisch", "Gelber Segelflossler", "Neonsalmler"};
static const char *FALLBACK_NAME[FISH] = {"Flossi", "Grummel", "Sunny", "Zisch", "Blitz", "Krümel", "Pünktchen", "Wusel"};
static const char *FALLBACK_TRAIT[FISH] = {"frech", "grantig", "fröhlich", "hibbelig", "neugierig", "verfressen", "schüchtern", "verträumt"};
static Names names;  // persisted as "names", separate from the tank save
static bool name_wanted[FISH];

enum { SIT_FED, SIT_SHAKE, SIT_TURN, SIT_TILT, SIT_TAP, SIT_LIGHT_ON, SIT_LIGHT_OFF, SIT_DIED, SIT_BABY, SIT_HUNGRY, SIT_IDLE };
static const struct { const char *prompt, *canned[3]; } SITUATIONS[] = {
  {"Jemand ist mit dem Handy ans Aquarium gekommen und hat Futterflocken ins Wasser gestreut.", {"Mjam, Flocken!", "Endlich Futter!", "Wer zuerst kommt, frisst zuerst!"}},
  {"Jemand hat das Aquarium gerade kräftig geschüttelt, alles wirbelt durcheinander.", {"Hey! Nicht schütteln!", "Mir ist schwindelig...", "Erdbeben!"}},
  {"Das Aquarium wurde gerade gedreht.", {"Huch, alles steht kopf!", "Neue Aussicht!", "Wer hat das Becken gedreht?"}},
  {"Das Aquarium wird schief gehalten, das Wasser schwappt zur Seite.", {"Wir laufen aus!", "Gerade halten, bitte!", "Wellengang!"}},
  {"Jemand tippt von außen gegen die Scheibe direkt vor dir.", {"Klopf nicht so!", "Hallo da draußen!", "Ich seh dich!"}},
  {"Das Licht im Aquarium ist gerade angegangen, es ist Morgen.", {"Guten Morgen!", "Licht an, Hunger an.", "Schon wieder Tag?"}},
  {"Das Licht ist gerade ausgegangen, Schlafenszeit.", {"Gute Nacht!", "Endlich Ruhe.", "Licht aus, Flossen hoch."}},
  {"Ein Mitbewohner ist gerade gestorben, weil niemand gefüttert hat.", {"Ruhe in Frieden...", "Das war zu viel Hunger.", "Füttert uns doch!"}},
  {"Es gibt gerade Nachwuchs im Aquarium!", {"Ein Baby!", "Willkommen, Kleines!", "Wir sind mehr!"}},
  {"Du hast großen Hunger, es war lange niemand da.", {"Ich hab Hunger!", "Hallo? Futter?", "Mein Magen knurrt."}},
  {"Ein ruhiger Moment. Erzähl etwas Lustiges über dein Leben im Becken.", {"Blubb.", "Ich zähle Blasen.", "Die Schnecke ist so langsam."}},
};

static QueueHandle_t jobs;
static volatile int babble_left = 0;  // chirps the frame loop plays while a fish talks
static volatile bool sound_on = true;  // web page toggle, kept in prefs
static void buzz(bool on) { board_buzzer(on && sound_on); }
static volatile bool lamp_is_on = true;
static uint32_t next_line = 60000, next_idle = 600000, line_gap = 0;

static bool queue_job(Job *j) {
  if (jobs && xQueueSend(jobs, &j, 0) == pdTRUE) return true;
  if (j->type != JOB_CHAT) delete j;
  return false;
}

static void show_line(int fish, const char *text) {
  char b[200];
  snprintf(b, sizeof b, "%s: %s", names.name[fish], text);
  xSemaphoreTake(pet_mux, portMAX_DELAY);
  pet_say(fish, b, millis());
  xSemaphoreGive(pet_mux);
  if (lamp_is_on) babble_left = min(5, (int)strlen(text) / 10 + 2);
}

static String tank_context(int fish) {
  String others;
  PetFish f;
  for (int i = 0; i < FISH; i++)
    if (i != fish && pet_fish(i, f) && f.state == 1) others += String(others.length() ? ", " : "") + names.name[i];
  float h = pet_fish(fish, f) ? f.hunger : 0;  // as words: a bare "Hunger 6 %" read as hungry to the model
  const char *hunger = h < 0.3f ? "satt" : h < 0.6f ? "etwas hungrig" : h < 0.85f ? "sehr hungrig" : "am Verhungern";
  time_t wall = wall_clock();
  struct tm lt = {};
  if (wall) localtime_r(&wall, &lt);
  char b[200];
  snprintf(b, sizeof b, "Uhrzeit: %02d:%02d. Du bist %s. Mitbewohner: %s.", lt.tm_hour, lt.tm_min, hunger, others.length() ? others.c_str() : "niemand");
  return b;
}

static String fish_system(int fish, const char *task) {
  PetFish f;
  int kind = pet_fish(fish, f) ? f.kind : 0;
  return String("Du bist ") + names.name[fish] + ", ein " + SPECIES_DE[kind] + (f.size < 1 ? "-Baby" : "") +
         " in einem winzigen Pixel-Aquarium auf einem Display. Dein Charakter: " + names.trait[fish] + ". " + task +
         " Schreibe auf Deutsch, ohne Emojis und ohne Anführungszeichen.";
}

static bool ask(const char *system, const String &user, int max_tokens, String &reply) {  // logs what each call bills
  uint32_t in = ai_in, out = ai_out;
  bool ok = ask(system, user, max_tokens, reply);
  logf("claude %s: %u in, %u out tokens\n", ok ? "ok" : "failed", (unsigned)(ai_in - in), (unsigned)(ai_out - out));
  return ok;
}

static void run_job(Job *j) {
  String reply;
  switch (j->type) {
    case JOB_SAY: {
      bool ok = false;
      if (api_key) {
        xSemaphoreTake(pet_mux, portMAX_DELAY);
        String sys = fish_system(j->fish, "Antworte mit genau einem kurzen, witzigen Satz in Ich-Form, höchstens 70 Zeichen.");
        String user = String("Gerade passiert: ") + j->text + "\n" + tank_context(j->fish);
        xSemaphoreGive(pet_mux);
        ok = ask(sys.c_str(), user, 80, reply);
      }
      show_line(j->fish, ok ? reply.c_str() : SITUATIONS[j->sit].canned[esp_random() % 3]);
      break;
    }
    case JOB_CHAT: {
      bool ok = false;
      if (api_key) {
        xSemaphoreTake(pet_mux, portMAX_DELAY);
        String sys = fish_system(j->fish, "Ein Mensch schreibt dir über die Webseite des Aquariums. Antworte frech und kurz, höchstens zwei Sätze und 100 Zeichen, sonst passt es nicht in deine Sprechblase.");
        String user = String(j->text) + "\n(" + tank_context(j->fish) + ")";
        xSemaphoreGive(pet_mux);
        ok = ask(sys.c_str(), user, 120, reply);
      }
      strlcpy(j->reply, ok ? reply.c_str() : "Blubb. Ohne Internet bin ich nicht so gesprächig.", sizeof j->reply);
      show_line(j->fish, j->reply);
      xSemaphoreGive(j->done);
      return;
    }
    case JOB_NAMES: {
      String user = "Erfinde für jeden dieser Aquarienfische einen kurzen, lustigen deutschen Spitznamen (höchstens 10 Zeichen) "
                    "und eine Charaktereigenschaft (1 bis 3 Wörter). Antworte nur mit JSON wie "
                    "[{\"slot\":0,\"name\":\"...\",\"trait\":\"...\"}]. Fische:";
      PetFish f;
      xSemaphoreTake(pet_mux, portMAX_DELAY);
      for (int i = 0; i < FISH; i++)
        if (name_wanted[i] && pet_fish(i, f)) user += String(" slot ") + i + " " + SPECIES_DE[f.kind] + (f.size < 1 ? " (Baby)" : "") + ";";
      xSemaphoreGive(pet_mux);
      if (!ask("Du gibst Fischen in einem kleinen Pixel-Aquarium Namen.", user, 300, reply)) break;
      int a = reply.indexOf('['), b = reply.lastIndexOf(']');
      JsonDocument doc;
      if (a < 0 || b < a || deserializeJson(doc, reply.substring(a, b + 1))) break;
      xSemaphoreTake(pet_mux, portMAX_DELAY);
      for (JsonObject o : doc.as<JsonArray>()) {
        int i = o["slot"] | -1;
        if (i < 0 || i >= FISH || !o["name"].is<const char *>()) continue;
        strlcpy(names.name[i], o["name"], sizeof names.name[i]);
        strlcpy(names.trait[i], o["trait"] | FALLBACK_TRAIT[i], sizeof names.trait[i]);
        name_wanted[i] = false;
        logf("fish %d is now %s (%s)\n", i, names.name[i], names.trait[i]);
      }
      prefs.putBytes("names", &names, sizeof names);
      xSemaphoreGive(pet_mux);
      break;
    }
    case JOB_DIARY: {
      bool ok = api_key && claude_ask(api_key,
                                      "Du schreibst das Tagebuch eines kleinen Pixel-Aquariums aus Sicht der Fische. "
                                      "Schreibe 2 bis 3 kurze, witzige Sätze auf Deutsch, höchstens 300 Zeichen, ohne Emojis.",
                                      j->text, 250, reply);
#ifdef NTFY_TOPIC
      ntfy_push(NTFY_TOPIC, "Aquarium-Tagebuch", ok ? reply.c_str() : j->text);
#endif
      break;
    }
    case JOB_PUSH:
#ifdef NTFY_TOPIC
      ntfy_push(NTFY_TOPIC, j->title, j->text);
#endif
      break;
  }
  delete j;
}

// One task does all HTTPS: BLE goes off for the first job and back on once the queue has been empty for 300 ms.
static void net_task(void *) {
  for (;;) {
    Job *j;
    if (xQueueReceive(jobs, &j, portMAX_DELAY) != pdTRUE) continue;
    ble_set(false);
    do run_job(j);
    while (xQueueReceive(jobs, &j, pdMS_TO_TICKS(300)) == pdTRUE);
    ble_set(true);
  }
}

static void notify(const char *title, const char *msg) {
#ifdef NTFY_TOPIC
  static uint32_t last_push = 0;  // at most one push per 30 min; the diary goes its own way
  if (last_push && millis() - last_push < 30 * 60000) return logf("push dropped: %s\n", title);
  last_push = millis();
  Job *j = new Job{JOB_PUSH};
  strlcpy(j->title, title, sizeof j->title);
  strlcpy(j->text, msg, sizeof j->text);
  queue_job(j);
#endif
}

static int pick_fish(int avoid = -1) {
  int alive[FISH], n = 0;
  PetFish f;
  for (int i = 0; i < FISH; i++)
    if (i != avoid && pet_fish(i, f) && f.state == 1) alive[n++] = i;
  return n ? alive[esp_random() % n] : -1;
}

// Called under pet_mux from the frame loop. Important lines skip the idle cooldown but never come faster than 6 s apart.
static void request_line(int sit, int fish, bool important, uint32_t now) {
  if (fish < 0) fish = pick_fish();
  if (fish < 0 || now < line_gap || (!important && now < next_line)) return;
  line_gap = now + 20000, next_line = now + 180000, next_idle = now + 900000 + esp_random() % 900000;
  if (!jobs) {  // offline: canned line right away
    char b[120];
    snprintf(b, sizeof b, "%s: %s", names.name[fish], SITUATIONS[sit].canned[esp_random() % 3]);
    pet_say(fish, b, now);
    if (lamp_is_on) babble_left = 3;
    return;
  }
  Job *j = new Job{JOB_SAY, fish, sit};
  strlcpy(j->text, SITUATIONS[sit].prompt, sizeof j->text);
  queue_job(j);
}

static void web_setup() {
  web.on("/", HTTP_GET, [] { web.send(200, "text/html", WEB_PAGE); });
  web.on("/status", HTTP_GET, [] {
    PetStatus st;
    xSemaphoreTake(pet_mux, portMAX_DELAY);
    pet_status(st);
    xSemaphoreGive(pet_mux);
    char b[280];
    snprintf(b, sizeof b, "{\"w\":%d,\"h\":%d,\"alive\":%d,\"dead\":%d,\"young\":%d,\"hunger\":%.2f,\"hunger_max\":%.2f,\"visitors\":%d,\"lamp\":%s,\"sound\":%s,\"fps\":%u,\"ai_calls\":%u,\"ai_in\":%u,\"ai_out\":%u}",
             CW, CH, st.alive, st.dead, st.young, st.hunger_avg, st.hunger_max, st.visitors_today, st.lamp_on ? "true" : "false", sound_on ? "true" : "false", last_fps, (unsigned)ai_calls, (unsigned)ai_in, (unsigned)ai_out);
    web.send(200, "application/json", b);
  });
  web.on("/fish", HTTP_GET, [] {
    JsonDocument doc;
    PetFish f;
    xSemaphoreTake(pet_mux, portMAX_DELAY);
    for (int i = 0; i < FISH; i++)
      if (pet_fish(i, f)) {
        JsonObject o = doc.add<JsonObject>();
        o["name"] = names.name[i], o["trait"] = names.trait[i], o["kind"] = SPECIES_DE[f.kind];
        o["hunger"] = (int)(f.hunger * 100), o["dead"] = f.state != 1, o["baby"] = f.size < 1;
      }
    xSemaphoreGive(pet_mux);
    String body;
    serializeJson(doc, body);
    web.send(200, "application/json", body);
  });
  web.on("/chat", HTTP_POST, [] {  // waits for the fish's answer (Claude call plus BLE pause, a few seconds)
    String q = web.arg("plain").substring(0, 200);
    xSemaphoreTake(pet_mux, portMAX_DELAY);
    int fish = pick_fish();
    last_active = millis();
    xSemaphoreGive(pet_mux);
    JsonDocument doc;
    if (fish < 0 || !q.length()) {
      doc["reply"] = "Das Becken ist leer.";
    } else {
      Job *j = new Job{JOB_CHAT, fish, SIT_IDLE};
      strlcpy(j->text, q.c_str(), sizeof j->text);
      j->done = xSemaphoreCreateBinary();
      if (queue_job(j)) xSemaphoreTake(j->done, portMAX_DELAY);
      else strlcpy(j->reply, "Moment, ich bin gerade beschäftigt.", sizeof j->reply);
      doc["name"] = names.name[fish], doc["reply"] = j->reply;
      vSemaphoreDelete(j->done);
      delete j;
    }
    String body;
    serializeJson(doc, body);
    web.send(200, "application/json", body);
  });
  web.on("/feed", HTTP_POST, [] {
    xSemaphoreTake(pet_mux, portMAX_DELAY);
    bool ok = pet_feed(last_active = millis());
    xSemaphoreGive(pet_mux);
    web.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
  });
  web.on("/lamp", HTTP_POST, [] {
    xSemaphoreTake(pet_mux, portMAX_DELAY);
    pet_toggle_lamp();
    last_active = millis();
    xSemaphoreGive(pet_mux);
    web.send(200, "application/json", "{\"ok\":true}");
  });
  web.on("/sound", HTTP_POST, [] {
    sound_on = !sound_on;
    if (!sound_on) board_buzzer(false);
    prefs.putBool("sound", sound_on);
    web.send(200, "application/json", sound_on ? "{\"sound\":true}" : "{\"sound\":false}");
  });
  web.on("/frame", HTTP_GET, [] {  // raw RGB565 canvas, little endian
    xSemaphoreTake(pet_mux, portMAX_DELAY);
    memcpy(snapshot, canvas, sizeof canvas);
    xSemaphoreGive(pet_mux);
    web.setContentLength(sizeof canvas);
    web.send(200, "application/octet-stream", "");
    web.sendContent((const char *)snapshot, sizeof canvas);
  });
  web.begin();
  xTaskCreatePinnedToCore([](void *) {
    for (;;) {
      web.handleClient();
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }, "web", 6144, nullptr, 1, nullptr, 0);
  jobs = xQueueCreate(6, sizeof(Job *));
  xTaskCreatePinnedToCore(net_task, "net", 10240, nullptr, 1, nullptr, 0);
  for (int i = 0; i < FISH; i++)
    if (name_wanted[i] && api_key) {
      queue_job(new Job{JOB_NAMES});
      break;
    }
}

static void net_loop(uint32_t now) {
#ifdef WIFI_SSID
  if (!net_up && WiFi.status() == WL_CONNECTED) {
    net_up = true;
    configTzTime(TIMEZONE, "pool.ntp.org", "time.google.com");
    ArduinoOTA.setHostname("mini-pet");
    ArduinoOTA.begin();
    log_server.begin();
    web_setup();
    logf("wifi %s  ota: mini-pet.local  log: nc mini-pet.local 23\n", WiFi.localIP().toString().c_str());
  }
  static uint32_t lost_since = 0;  // the stack's own reconnect gave up twice in a day; kick it after 30 s offline
  if (WiFi.status() == WL_CONNECTED) lost_since = 0;
  else if (!lost_since) lost_since = now;
  else if (now - lost_since > 30000) WiFi.disconnect(), WiFi.begin(WIFI_SSID, WIFI_PASS), lost_since = now;
  if (!net_up) return;
  ArduinoOTA.handle();
  if (log_server.hasClient()) {
    log_client = log_server.accept();
    logf("log attached\n");
  }
#endif
}

// Only changed blocks touch PSRAM: full 600 KB redraws starve the panel's scanout once BLE runs.
// Which screen edge points down: 0 bottom (portrait), 1 right, 2 top, 3 left. Fish and HUD turn upright for it.
static int orient = 0;
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
    int sx = i % CW * S, sy = i / CW * S;
    uint32_t cc = c | (uint32_t)c << 16, *p = (uint32_t *)(fb + sy * LCD_W + sx);  // S == 4: two words per row
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

  log_mux = xSemaphoreCreateMutex();
  ble_mux = xSemaphoreCreateMutex();
  ble_set(true);

  pet_mux = xSemaphoreCreateMutex();
  snapshot = (uint16_t *)heap_caps_malloc(sizeof canvas, MALLOC_CAP_SPIRAM);
  pet_init(millis(), (uint8_t *)heap_caps_malloc(PET_SCRATCH, MALLOC_CAP_SPIRAM));
  prefs.begin("mini-pet");
  PetSave s;
  if (prefs.getBytes("pet", &s, sizeof s) == sizeof s) pet_load(s);
  saved_ts = prefs.getULong("ts", 0);
  sound_on = prefs.getBool("sound", true);
  if (prefs.getBytes("names", &names, sizeof names) != sizeof names) memset(&names, 0, sizeof names);
  PetFish f;
  for (int i = 0; i < FISH; i++)
    if (pet_fish(i, f) && !names.name[i][0]) {
      strlcpy(names.name[i], FALLBACK_NAME[i], sizeof names.name[i]);
      strlcpy(names.trait[i], FALLBACK_TRAIT[i], sizeof names.trait[i]);
      name_wanted[i] = true;
    }
  logf("mini-pet ready\n");
}

// Paced by the panel (~36 Hz): push the frame drawn last time right after vsync, then draw the next one
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

  if (xSemaphoreTake(ble_mux, 0) == pdTRUE) {  // the net task may have BLE switched off for a TLS call
    if (ble_on) {
      BLEScan scan = BLE.getScan();
      if (!scan.isScanning()) {  // short sessions so the library's result list of rotating addresses stays small
        scan.clearResults();
        scan.start(5000);
      }
    }
    xSemaphoreGive(ble_mux);
  }

  int tx = 0, ty = 0;
  bool down = board_touch(tx, ty);
  PetInput in = {};
  in.now = now, in.tap = down && !was_down;
  in.tx = tx / S, in.ty = ty / S;
  was_down = down;

  static float gx = 0, gy = 1, ax = 0, ay = 0, az = 1;
  static uint32_t shake_until = 0;
  bool moved = false;
  if (float px = ax, py = ay, pz = az; board_accel(ax, ay, az)) {
    moved = fabsf(ax - px) + fabsf(ay - py) + fabsf(az - pz) > 0.08f;  // picked up or nudged; the noise at rest is ~0.01
    gx += (SCREEN_GX(ax, ay, az) - gx) * 0.15f, gy += (SCREEN_GY(ax, ay, az) - gy) * 0.15f;
    float jolt = fabsf(sqrtf(ax * ax + ay * ay + az * az) - 1);  // 0 at rest, whatever the orientation
    if (jolt > 0.9f && now > shake_until) in.shake = true, shake_until = now + 1500;
  }
  // Turn fish and HUD when another edge clearly points down for 0.6 s; lying flat keeps the current orientation.
  static int want = 0;
  static uint32_t want_since = 0;
  int next_orient = orient;
  if (fabsf(gx) > 0.55f || fabsf(gy) > 0.55f) {
    int cand = fabsf(gx) > fabsf(gy) ? (gx > 0 ? 1 : 3) : (gy > 0 ? 0 : 2);
    if (cand == orient || fabsf(fabsf(gx) - fabsf(gy)) < 0.25f) want = orient;
    else if (cand != want) want = cand, want_since = now;
    else if (now - want_since > 600) next_orient = cand;
  }
  in.gx = gx, in.gy = gy, in.rot = orient;

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
    last_fps = frames;
    frames = draw_us = wait_us = push_us = pre_us = paint_us = 0;
    next_log = now + 1000;
  }

  uint32_t t0 = micros();
  PetOutput out;
  xSemaphoreTake(pet_mux, portMAX_DELAY);
  if (next_orient != orient) orient = next_orient, in.rot = orient, logf("orientation %d\n", orient);
  bool lit = now - last_active < SLEEP_MS;
  if (down || moved || in.approach || in.shake) last_active = now;
  if (!lit) in.tap = false;  // the first touch on a dark screen only wakes it
  pet_update(in, out);
  static int last_orient = 0, feeds_today = 0, births_today = 0, deaths_today = 0, counted_day = -1, diary_day = -1;
  static uint32_t tilt_since = 0;
  PetStatus ps;
  pet_status(ps);
  lamp_is_on = ps.lamp_on;
  if (day >= 0 && day != counted_day) counted_day = day, feeds_today = births_today = deaths_today = 0;
  if (out.events & EV_FED) logf("food!\n"), feeds_today++, request_line(SIT_FED, -1, true, now);
  if (out.events & EV_DIED) {
    logf("a fish died\n"), deaths_today++;
    notify("Ein Fisch ist gestorben", "Er hat 24 Stunden nichts bekommen. Antippen, um ihn rauszufischen.");
    request_line(SIT_DIED, -1, true, now);
  }
  if (out.events & EV_BABY) logf("a baby fish!\n"), births_today++, notify("Nachwuchs!", "Drei Tage gute Pflege haben sich gelohnt."), request_line(SIT_BABY, -1, true, now);
  if (out.events & EV_LAMP) request_line(ps.lamp_on ? SIT_LIGHT_ON : SIT_LIGHT_OFF, -1, true, now);
  if (out.tapped_fish >= 0) request_line(SIT_TAP, out.tapped_fish, true, now);
  if (in.shake) request_line(SIT_SHAKE, -1, true, now);
  if (orient != last_orient) last_orient = orient, request_line(SIT_TURN, -1, false, now);
  float side = orient == 0 ? gx : orient == 1 ? -gy : orient == 2 ? -gx : gy;  // tilt across the upright view
  tilt_since = fabsf(side) > 0.4f ? (tilt_since ? tilt_since : now) : 0;
  if (tilt_since && now - tilt_since > 1500) request_line(SIT_TILT, -1, false, now);
  if (ps.lamp_on && now > next_idle) request_line(SIT_IDLE, -1, false, now);
  if (jobs && minute == 21 * 60 && day != diary_day && clock_synced) {  // evening diary push
    diary_day = day;
    Job *j = new Job{JOB_DIARY};
    String who;
    PetFish f;
    for (int i = 0; i < FISH; i++)
      if (pet_fish(i, f) && f.state == 1) who += String(who.length() ? ", " : "") + names.name[i] + " (" + SPECIES_DE[f.kind] + ", Hunger " + (int)(f.hunger * 100) + " %)";
    snprintf(j->text, sizeof j->text, "Heute: %d Besucher, %d Fütterungen, %d Geburten, %d Todesfälle. Im Becken: %s.", ps.visitors_today, feeds_today,
             births_today, deaths_today, who.length() ? who.c_str() : "niemand");
    queue_job(j);
  }
  static int last_state[FISH] = {-1, -1, -1, -1, -1, -1, -1, -1};  // new fish (restock, babies) get names
  bool naming = false;
  for (int i = 0; i < FISH; i++) {
    PetFish f;
    int st = pet_fish(i, f) ? f.state : 0;
    if (last_state[i] >= 0 && last_state[i] != 1 && st == 1) {
      strlcpy(names.name[i], FALLBACK_NAME[i], sizeof names.name[i]);
      strlcpy(names.trait[i], FALLBACK_TRAIT[i], sizeof names.trait[i]);
      name_wanted[i] = naming = true;
    }
    last_state[i] = st;
  }
  if (naming && api_key) queue_job(new Job{JOB_NAMES});
  static int alert = prefs.getUChar("alert", 0);  // 0 nothing sent, 1 hungry, 2 starving; re-arms once fed; survives reboots
  if (clock_synced && now >= next_alert) {
    int alert_was = alert;
    next_alert = now + 10000;
    PetStatus st;
    pet_status(st);
    if (st.alive && st.hunger_max > 0.85f && alert < 2)
      alert = 2, notify("Die Fische verhungern", "Füttere sie in den nächsten Stunden, sonst sterben sie."), request_line(SIT_HUNGRY, -1, true, now);
    else if (st.alive && st.hunger_max > 0.6f && alert < 1)
      alert = 1, notify("Die Fische haben Hunger", "Geh mit dem Handy am Aquarium vorbei, um sie zu füttern."), request_line(SIT_HUNGRY, -1, true, now);
    else if (st.hunger_max < 0.4f) alert = 0;
    if (alert != alert_was) prefs.putUChar("alert", alert);
  }
  if (out.events) next_save = min(next_save, now + 5000);
  static int backlight = -1;  // fades out over ~2 s when idle, back in within a few frames
  int bl_want = now - last_active < SLEEP_MS ? out.backlight : 0;
  int bl = backlight < 0 ? bl_want : bl_want < backlight ? backlight - 1 : min(bl_want, backlight + 5);
  if (bl != backlight) board_backlight(backlight = bl);

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
    buzz(true);
    beep_until = now + out.beep_ms;
  }
  if (beep_until && now > beep_until) {
    buzz(false);
    beep_until = 0;
  }
  static uint32_t babble_next = 0;  // talking fish: short chirps with uneven gaps
  static bool babble_high = false;
  if (!beep_until && babble_left > 0 && now >= babble_next) {
    babble_high = !babble_high;
    buzz(babble_high);
    babble_next = now + (babble_high ? 18 : 40 + esp_random() % 70);
    if (!babble_high) babble_left--;
  }

  uint32_t tpaint = micros();
  pet_draw(canvas, now);
  xSemaphoreGive(pet_mux);
  paint_us += micros() - tpaint;
  drawn = true;
  frames++, draw_us += micros() - t0;
}
