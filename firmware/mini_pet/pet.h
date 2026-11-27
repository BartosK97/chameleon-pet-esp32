// Aquarium game logic + drawing on a small RGB565 canvas; no Arduino deps so tools/preview can build it on the Mac.
#pragma once
#include <stdint.h>

constexpr int CW = 120, CH = 160;  // portrait canvas, fixed; sand and water follow gravity, fish and HUD turn upright
constexpr int FISH = 8;  // slots: 5 stocked, the rest for offspring

// Approach = one BLE device rising this much above its own slow baseline, ending above APPROACH_MIN.
constexpr int APPROACH_MIN = -52, APPROACH_RISE = 8;

struct PetInput {
  uint32_t now;   // ms
  bool tap;       // finger went down this frame
  int tx, ty;     // tap in canvas pixels
  bool sample;    // new signal sample this frame (every 250 ms)
  int sig, base;  // strongest device now and its slow baseline, dBm (-100 = nothing)
  bool approach;  // some device just came close
  float gx, gy;   // gravity in portrait canvas axes, g: +x right, +y down (0,0 = lying flat or no IMU)
  int rot;        // edge pointing down for fish and HUD: 0 bottom, 1 right, 2 top, 3 left
  bool shake;     // device was shaken this frame
  int minute;     // local minutes since midnight, -1 = clock unknown
  int day;        // local day number, -1 = clock unknown
};

enum { EV_FED = 1, EV_DIED = 2, EV_BABY = 4, EV_LAMP = 8 };

struct PetOutput {
  int beep_ms;    // >0: start a beep this long
  int events;     // EV_* bits that happened this frame
  int backlight;    // wanted backlight, percent
  int tapped_fish;  // fish slot tapped this frame, -1 = none
};

struct PetStatus {
  int alive, dead, young;
  float hunger_avg, hunger_max;  // 0 fed .. 1 dead
  int visitors_today;
  bool lamp_on;
  float light;  // scene brightness 0.5 (night) .. 1
};

// What survives a power cycle; the host stores it with a wall-clock timestamp.
struct PetSave {
  uint32_t magic;
  uint8_t kind[FISH], state[FISH];  // state: 0 gone, 1 alive, 2 dead (floating)
  float hunger[FISH], size[FISH], fed_s[FISH];
  uint16_t visitors;
  int32_t visitor_day;
};
constexpr uint32_t PET_SAVE_MAGIC = 0xF15A0002;

constexpr int PET_SCRATCH = CW * CH * 4 + 48 * 48 * 2;  // sand grid, turn layer, creature sprite
void pet_init(uint32_t now, uint8_t *scratch);  // scratch: PET_SCRATCH bytes (PSRAM is fine)
void pet_update(const PetInput &in, PetOutput &out);
void pet_draw(uint16_t *canvas, uint32_t now);
void pet_get_save(PetSave &s);
void pet_load(const PetSave &s);
void pet_advance(float seconds);  // time that passed while powered off
void pet_status(PetStatus &s);
bool pet_feed(uint32_t now);  // remote feeding; false while on cooldown
void pet_toggle_lamp();
struct PetFish { int kind, state; float hunger, size; };  // kind: 0 clownfish, 1 tang, 2 yellow, 3 neon; state 1 alive, 2 dead
bool pet_fish(int slot, PetFish &f);  // false for an empty slot
void pet_say(int slot, const char *utf8, uint32_t now);  // speech bubble over that fish for a few seconds
