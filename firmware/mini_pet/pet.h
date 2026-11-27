// Chameleon game logic + drawing on a small RGB565 canvas; no Arduino deps so tools/preview can build it on the Mac.
#pragma once
#include <stdint.h>

constexpr int CW = 60, CH = 80;

struct PetInput {
  uint32_t now;  // ms
  bool tap;      // finger went down this frame
  int tx, ty;    // tap in canvas pixels
  int rssi;      // smoothed strongest BLE signal nearby, dBm (-100 = nothing)
};

struct PetOutput {
  int beep_ms;  // >0: start a beep this long
};

void pet_init(uint32_t now);
void pet_update(const PetInput &in, PetOutput &out);
void pet_draw(uint16_t *canvas, uint32_t now);
