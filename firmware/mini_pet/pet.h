// Aquarium game logic + drawing on a small RGB565 canvas; no Arduino deps so tools/preview can build it on the Mac.
#pragma once
#include <stdint.h>

constexpr int CW = 120, CH = 160;

// Approach = one BLE device rising this much above its own slow baseline, ending above APPROACH_MIN.
constexpr int APPROACH_MIN = -52, APPROACH_RISE = 8;

struct PetInput {
  uint32_t now;   // ms
  bool tap;       // finger went down this frame
  int tx, ty;     // tap in canvas pixels
  bool sample;    // new signal sample this frame (every 250 ms)
  int sig, base;  // strongest device now and its slow baseline, dBm (-100 = nothing)
  bool approach;  // some device just came close
};

struct PetOutput {
  int beep_ms;  // >0: start a beep this long
  bool fed;     // food was dropped this frame
};

// What survives a power cycle; the host stores it with a wall-clock timestamp.
constexpr int FISH = 5;
struct PetSave {
  uint32_t magic;
  float hunger[FISH];  // 0 fed .. 1 dead
  uint8_t state[FISH];  // 0 removed, 1 alive, 2 dead (floating)
};
constexpr uint32_t PET_SAVE_MAGIC = 0xF15A0001;

void pet_init(uint32_t now);
void pet_update(const PetInput &in, PetOutput &out);
void pet_draw(uint16_t *canvas, uint32_t now);
void pet_get_save(PetSave &s);
void pet_load(const PetSave &s);
void pet_advance(float seconds);  // time that passed while powered off
