// Chameleon pet: touch pets it, a phone held right next to the device feeds it (strongest BLE advert RSSI).
#include <BLE.h>
#include "board.h"
#include "pet.h"

constexpr int S = LCD_W / CW;  // canvas scale, 8x
static uint16_t canvas[CW * CH];

static volatile int ble_best = -128;  // strongest RSSI since last read, written by the BLE task
static volatile uint16_t ble_best_mfg = 0;  // its manufacturer id, 0x004C = Apple
static float rssi_s = -100;
static uint32_t beep_until = 0, next_rssi = 0, next_log = 0;
static bool was_down = false;

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
  if (!board_init()) Serial.println("display init failed");

  if (!BLE.begin("mini-pet")) Serial.println("BLE init failed");
  BLEScan scan = BLE.getScan();
  scan.setActiveScan(false);
  scan.setInterval(100);
  scan.setWindow(99);
  scan.setFilterDuplicates(false);
  scan.onResult([](const BLEAdvertisedDevice &d) {
    int r = d.getRSSI();
    if (r > ble_best) ble_best = r, ble_best_mfg = d.haveManufacturerData() ? d.getManufacturerCompanyId() : 0;
  });

  pet_init(millis());
  Serial.println("mini-pet ready");
}

void loop() {
  uint32_t now = millis();

  BLEScan scan = BLE.getScan();
  if (!scan.isScanning()) {  // short sessions so the result list of rotating addresses stays small
    scan.clearResults();
    scan.start(5000);
  }
  if (now >= next_rssi) {  // smooth the per-250ms maximum; drift down when nothing is heard
    int best = ble_best;
    ble_best = -128;
    rssi_s = best > -128 ? rssi_s * 0.5f + best * 0.5f : fmaxf(-100, rssi_s - 3);
    next_rssi = now + 250;
  }
  if (now >= next_log) {
    Serial.printf("rssi %.0f dBm mfg 0x%04X\n", rssi_s, ble_best_mfg);
    next_log = now + 1000;
  }

  int tx = 0, ty = 0;
  bool down = board_touch(tx, ty);
  PetInput in = {now, down && !was_down, tx / S, ty / S, (int)rssi_s};
  was_down = down;
  PetOutput out;
  pet_update(in, out);
  if (out.beep_ms == 15) Serial.println("fly!");
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
