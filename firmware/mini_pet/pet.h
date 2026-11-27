// Chameleon game logic + drawing on a small RGB565 canvas; no Arduino deps so tools/preview can build it on the Mac.
#pragma once
#include <stdint.h>

constexpr int CW = 60, CH = 80;

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
};

void pet_init(uint32_t now);
void pet_update(const PetInput &in, PetOutput &out);
void pet_draw(uint16_t *canvas, uint32_t now);
