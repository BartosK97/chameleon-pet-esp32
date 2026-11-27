#include "pet.h"
#include <math.h>

static constexpr uint16_t rgb(int r, int g, int b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }

static uint16_t hsv(float h, float s, float v) {
  h = fmodf(h + 360.0f, 360.0f) / 60.0f;
  float c = v * s, x = c * (1 - fabsf(fmodf(h, 2) - 1)), m = v - c, r = 0, g = 0, b = 0;
  if (h < 1) r = c, g = x; else if (h < 2) r = x, g = c; else if (h < 3) g = c, b = x;
  else if (h < 4) g = x, b = c; else if (h < 5) r = x, b = c; else r = c, b = x;
  return rgb((r + m) * 255, (g + m) * 255, (b + m) * 255);
}

// Measured 2026-11-27: room -56..-64 dBm, iPhone at 30 cm -44..-51, right next to it -39..-49.
static const uint32_t FEED_COOLDOWN = 8000, FLY_MS = 1600, TONGUE_OUT = 180, TONGUE_BACK = 220;
static const float HUNGER_PER_MS = 1.0f / (10 * 60 * 1000);  // test pace: starving after 10 min
static const float JOY_PER_MS = 1.0f / (5 * 60 * 1000);

static const uint16_t OUTLINE = rgb(35, 40, 35), EYE = rgb(20, 20, 25), EYE_RING = rgb(245, 245, 225);
static const uint16_t TONGUE = rgb(240, 90, 130), TONGUE_TIP = rgb(255, 130, 165), HEART = rgb(230, 40, 80);
static const uint16_t FLY = rgb(40, 40, 50), WING = rgb(215, 230, 255), BAR_BG = rgb(255, 255, 255);
static const float MOUTH_X = 45, MOUTH_Y = 47, EYE_X = 38, EYE_Y = 44;

static struct {
  float hunger = 0.6f, joy = 0.4f;
  int sig = -100;
  bool debug = true;
  struct { int8_t sig, base; bool hit; } hist[56] = {};
  int hist_i = 0;
  uint32_t last_ms = 0, last_feed = 0, last_approach = 0;
  bool fly = false, tongue = false;
  uint32_t fly_t0 = 0, tongue_t0 = 0;
  float fx = 0, fy = 0, tip_x = MOUTH_X, tip_y = MOUTH_Y;
  int flash = 0;  // 1 petted, 2 ate
  uint32_t flash_t0 = 0;
  float look_x = 55, look_y = 44;
  uint32_t look_until = 0, next_wander = 0;
  struct { float x, y; bool live; } hearts[6] = {};
} S;

static uint32_t seed = 12345;
static float rnd() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; }

void pet_init(uint32_t now) {
  S.last_ms = now;
  S.last_feed = S.last_approach = now - FEED_COOLDOWN;
  for (auto &h : S.hist) h = {-100, -100, false};
}

void pet_update(const PetInput &in, PetOutput &out) {
  uint32_t now = in.now;
  float dt = (float)(now - S.last_ms);
  S.last_ms = now;
  out.beep_ms = 0;
  S.hunger = fminf(1, S.hunger + dt * HUNGER_PER_MS);
  S.joy = fmaxf(0, S.joy - dt * JOY_PER_MS);

  S.sig = in.sig;
  if (in.approach) S.last_approach = now;
  if (in.sample) {
    S.hist[S.hist_i] = {(int8_t)in.sig, (int8_t)in.base, in.approach};
    S.hist_i = (S.hist_i + 1) % 56;
  } else if (in.approach) {
    S.hist[(S.hist_i + 55) % 56].hit = true;
  }

  if (in.approach && !S.fly && now - S.last_feed > FEED_COOLDOWN) {
    S.fly = true;
    S.fly_t0 = S.last_feed = now;
    out.beep_ms = 15;
  }

  if (S.fly && !S.tongue) {
    float t = (now - S.fly_t0) / (float)FLY_MS;
    if (t >= 1) {
      S.tongue = true;
      S.tongue_t0 = now;
    } else {
      S.fx = 58 + (51 - 58) * t + sinf(t * 21) * 2.5f;
      S.fy = 6 + (41 - 6) * t + cosf(t * 17) * 2.5f;
    }
  }
  if (S.tongue) {
    uint32_t tt = now - S.tongue_t0;
    if (tt < TONGUE_OUT) {
      float e = tt / (float)TONGUE_OUT;
      S.tip_x = MOUTH_X + (S.fx - MOUTH_X) * e;
      S.tip_y = MOUTH_Y + (S.fy - MOUTH_Y) * e;
    } else if (tt < TONGUE_OUT + TONGUE_BACK) {
      float e = 1 - (tt - TONGUE_OUT) / (float)TONGUE_BACK;
      S.tip_x = S.fx = MOUTH_X + (S.fx - MOUTH_X) * e;
      S.tip_y = S.fy = MOUTH_Y + (S.fy - MOUTH_Y) * e;
    } else {
      S.tongue = S.fly = false;
      S.hunger = fmaxf(0, S.hunger - 0.35f);
      S.flash = 2;
      S.flash_t0 = now;
      out.beep_ms = 60;
    }
  }

  if (in.tap && in.tx >= 46 && in.ty <= 11) {  // signal icon toggles the graph
    S.debug = !S.debug;
  } else if (in.tap) {
    if (in.tx >= 8 && in.tx <= 48 && in.ty >= 34 && in.ty <= 66) {
      S.joy = fminf(1, S.joy + 0.15f);
      S.flash = 1;
      S.flash_t0 = now;
      out.beep_ms = 30;
      for (auto &h : S.hearts)
        if (!h.live) { h = {(float)in.tx - 2, (float)in.ty - 2, true}; break; }
    }
    S.look_x = in.tx;
    S.look_y = in.ty;
    S.look_until = now + 1500;
  }
  if (S.fly) {
    S.look_x = S.fx;
    S.look_y = S.fy;
  } else if (now > S.look_until && now > S.next_wander) {
    S.look_x = 30 + rnd() * 30;
    S.look_y = 20 + rnd() * 40;
    S.next_wander = now + 1500 + (uint32_t)(rnd() * 2500);
  }

  for (auto &h : S.hearts)
    if (h.live && (h.y -= 0.4f) < -5) h.live = false;
}

// ---- drawing ----
static uint16_t *C;

static void px(int x, int y, uint16_t c) {
  if (x >= 0 && x < CW && y >= 0 && y < CH) C[y * CW + x] = c;
}

static bool in_ell(float x, float y, float cx, float cy, float rx, float ry) {
  float dx = (x + 0.5f - cx) / rx, dy = (y + 0.5f - cy) / ry;
  return dx * dx + dy * dy <= 1;
}

static void ell(float cx, float cy, float rx, float ry, uint16_t c) {
  for (int y = (int)(cy - ry - 1); y <= (int)(cy + ry + 1); y++)
    for (int x = (int)(cx - rx - 1); x <= (int)(cx + rx + 1); x++)
      if (in_ell(x, y, cx, cy, rx, ry)) px(x, y, c);
}

static void line(float x0, float y0, float x1, float y1, uint16_t c) {
  int n = (int)fmaxf(fabsf(x1 - x0), fabsf(y1 - y0)) + 1;
  for (int i = 0; i <= n; i++) px((int)(x0 + (x1 - x0) * i / n), (int)(y0 + (y1 - y0) * i / n), c);
}

struct Ell { float cx, cy, rx, ry; };
// facing right, sitting on the branch at y 56
static const Ell BODY = {26, 48, 10, 6}, HEAD = {37, 45, 6, 5};
static const Ell SHAPES[] = {
  BODY, HEAD,
  {33, 41, 4, 3},       // casque
  {42, 46, 3.5f, 3},    // snout
  {16, 51, 4, 3},       // tail root
  {13, 58.5f, 5, 5},    // tail curl, spiral line drawn on top
  {20, 54, 1.6f, 3.5f}, {32, 53, 1.6f, 4},    // legs
  {20, 57, 2.5f, 1.2f}, {32, 57, 2.5f, 1.2f}, // feet
};

static void draw_background() {
  for (int y = 0; y < CH; y++) {
    float t = y / (float)CH;
    uint16_t c = rgb(200 + 40 * t, 232 + 12 * t, 205 + 10 * t);
    for (int x = 0; x < CW; x++) px(x, y, c);
  }
  ell(3, 22, 8, 14, rgb(160, 210, 160));
  ell(58, 32, 7, 12, rgb(150, 205, 150));
  ell(6, 76, 11, 8, rgb(140, 195, 140));
  ell(55, 72, 9, 9, rgb(160, 210, 160));
  for (int x = 0; x < CW; x++) {
    px(x, 56, rgb(165, 115, 75));
    px(x, 57, rgb(135, 90, 55));
    px(x, 58, rgb(135, 90, 55));
    px(x, 59, rgb(95, 62, 38));
  }
  px(9, 57, rgb(95, 62, 38));
  px(50, 58, rgb(95, 62, 38));
}

static void draw_chameleon(uint32_t now) {
  float hue = 115 - S.hunger * 75, sat = 0.65f - S.hunger * 0.25f, val = 0.85f;
  float k = (now - S.flash_t0) / 1000.0f;
  if (S.flash == 1 && k < 1) hue += 360 * k, sat = 0.8f;
  if (S.flash == 2 && k < 0.6f) hue = 55, sat = 0.8f, val = 1;
  uint16_t body = hsv(hue, sat, val), dark = hsv(hue, sat, val * 0.62f), light = hsv(hue, sat * 0.45f, fminf(1, val * 1.1f));
  int bob = (now / 700) % 2;

  for (const auto &s : SHAPES) ell(s.cx, s.cy + bob, s.rx + 1, s.ry + 1, OUTLINE);
  for (int x = 18; x <= 34; x += 3) {  // dorsal ridge
    float u = (x + 0.5f - BODY.cx) / BODY.rx;
    px(x, (int)(BODY.cy - BODY.ry * sqrtf(1 - u * u)) - 2 + bob, OUTLINE);
  }
  for (const auto &s : SHAPES) ell(s.cx, s.cy + bob, s.rx, s.ry, body);

  for (int y = 38; y < 56; y++)
    for (int x = 14; x < 48; x++) {
      bool in_body = in_ell(x, y - bob, BODY.cx, BODY.cy, BODY.rx, BODY.ry);
      bool in_head = in_ell(x, y - bob, HEAD.cx, HEAD.cy, HEAD.rx, HEAD.ry);
      if ((in_body && y - bob >= 51) || (in_head && y - bob >= 48)) px(x, y, light);
      else if (in_body && (x + (y - bob) / 2) % 5 == 0) px(x, y, dark);
    }

  for (int i = 0; i < 60; i++) {  // tail spiral, from the root inwards
    float t = i / 59.0f, a = -0.6f - t * 7.5f, r = 3.6f - t * 3.2f;
    px((int)(13.5f + r * cosf(a)), (int)(58.8f + r * sinf(a)) + bob, dark);
  }

  line(38, MOUTH_Y + bob, MOUTH_X, MOUTH_Y + bob, OUTLINE);
  px(37, MOUTH_Y + bob + (S.joy > 0.5f ? -1 : 1), OUTLINE);
  px(44, 44 + bob, dark);  // nostril

  ell(EYE_X, EYE_Y + bob, 3.8f, 3.8f, OUTLINE);
  ell(EYE_X, EYE_Y + bob, 2.9f, 2.9f, body);
  bool blink = now % 4200 < 140;
  if (blink) {
    line(EYE_X - 2, EYE_Y + bob, EYE_X + 2, EYE_Y + bob, OUTLINE);
  } else {
    ell(EYE_X, EYE_Y + bob, 1.9f, 1.9f, EYE_RING);
    float vx = S.look_x - EYE_X, vy = S.look_y - EYE_Y, len = sqrtf(vx * vx + vy * vy) + 0.01f;
    int ox = (int)roundf(vx / len), oy = (int)roundf(vy / len);
    for (int y = 0; y < 2; y++)
      for (int x = 0; x < 2; x++) px((int)EYE_X - 1 + ox + x, (int)EYE_Y - 1 + oy + y + bob, EYE);
  }

  if (S.tongue) {
    line(MOUTH_X, MOUTH_Y + bob, S.tip_x, S.tip_y, TONGUE);
    ell(S.tip_x, S.tip_y, 1.4f, 1.4f, TONGUE_TIP);
  }
}

static void draw_hud(uint32_t now) {
  int food = (int)((1 - S.hunger) * 22), joy = (int)(S.joy * 22);
  for (int x = 0; x < 22; x++)
    for (int y = 0; y < 2; y++) {
      px(3 + x, 3 + y, x < food ? rgb(120, 200, 60) : BAR_BG);
      px(3 + x, 7 + y, x < joy ? HEART : BAR_BG);
    }
  int bars = S.sig >= APPROACH_MIN + 7 ? 4 : S.sig >= APPROACH_MIN ? 3 : S.sig >= -65 ? 2 : S.sig >= -80 ? 1 : 0;
  bool hot = now - S.last_approach < 1500;
  for (int i = 0; i < 4; i++)
    for (int h = 0; h <= i; h++)
      px(49 + i * 2, 8 - h, i < bars ? (hot ? HEART : rgb(70, 80, 70)) : rgb(190, 205, 190));
}

// Signal graph, oldest left: blue = strongest device, grey = its baseline, red dots = APPROACH_MIN, pink column = approach.
static void draw_graph() {
  const int X0 = 2, Y0 = 12, GH = 22, TOP = -30, RANGE = 55;
  auto y_of = [&](int dbm) { int y = Y0 + (TOP - dbm) * (GH - 1) / RANGE; return y < Y0 ? Y0 : y > Y0 + GH - 1 ? Y0 + GH - 1 : y; };
  for (int y = Y0 - 1; y <= Y0 + GH; y++)
    for (int x = X0 - 1; x <= X0 + 56; x++)
      px(x, y, (y == Y0 - 1 || y == Y0 + GH || x == X0 - 1 || x == X0 + 56) ? rgb(150, 170, 150) : rgb(250, 252, 246));
  int prev = -1;
  for (int i = 0; i < 56; i++) {
    const auto &h = S.hist[(S.hist_i + i) % 56];
    int x = X0 + i;
    if (h.hit)
      for (int y = Y0; y < Y0 + GH; y++) px(x, y, rgb(255, 190, 205));
    if (i % 2 == 0) px(x, y_of(APPROACH_MIN), HEART);
    if (h.base > -100) px(x, y_of(h.base), rgb(160, 160, 160));
    if (h.sig > -100) {
      int y = y_of(h.sig);
      if (prev >= 0) line(x - 1, prev, x, y, rgb(40, 90, 200));
      else px(x, y, rgb(40, 90, 200));
      prev = y;
    } else {
      prev = -1;
    }
  }
}

void pet_draw(uint16_t *canvas, uint32_t now) {
  C = canvas;
  draw_background();
  if (S.debug) draw_graph();
  draw_chameleon(now);
  if (S.fly) {
    int x = (int)S.fx, y = (int)S.fy;
    px(x, y, FLY), px(x + 1, y, FLY), px(x, y + 1, FLY), px(x + 1, y + 1, FLY);
    if ((now / 50) % 2) px(x - 1, y - 1, WING), px(x + 2, y - 1, WING);
    else px(x, y - 1, WING), px(x + 1, y - 1, WING);
  }
  static const char *HEART_PX[] = {".h.h.", "hhhhh", "hhhhh", ".hhh.", "..h.."};
  for (auto &h : S.hearts)
    if (h.live)
      for (int y = 0; y < 5; y++)
        for (int x = 0; x < 5; x++)
          if (HEART_PX[y][x] == 'h') px((int)h.x + x, (int)h.y + y, HEART);
  draw_hud(now);
}
