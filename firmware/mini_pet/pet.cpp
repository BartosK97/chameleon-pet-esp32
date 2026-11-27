#include "pet.h"
#include <math.h>
#include <string.h>
#include <initializer_list>
#include "font_tomthumb.h"

static constexpr uint16_t rgb(int r, int g, int b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }

static uint16_t mix(uint16_t a, uint16_t b, float t) {
  int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31, br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
  return ((int)(ar + (br - ar) * t) << 11) | ((int)(ag + (bg - ag) * t) << 5) | (int)(ab + (bb - ab) * t);
}

static const uint32_t FEED_COOLDOWN = 8000, VISITOR_GAP = 30000;
static const float HUNGER_PER_S = 1.0f / 86400;   // a full fish dies after 24 h without food
static const float GROW_PER_S = 0.55f / 172800;   // a baby reaches full size in 2 days
static const float BREED_FED_S = 72 * 3600;       // 3 days well fed (hunger < WELL_FED) to have a baby
static const float WELL_FED = 0.35f;
static const int LAMP_ON_MIN = 7 * 60, LAMP_OFF_MIN = 22 * 60;
static const float NIGHT = 0.5f;  // scene brightness with the lamp off; the backlight dims on top
static const float PI_F = 3.14159265f;
static const int SURFACE = 18, SAND = CH - 20;  // portrait layout at start; the air keeps SURFACE rows' worth of area in any pose
static const float SAND_STEPS_PER_S = 40, REPOSE = 0.44f;  // grains slide down slopes steeper than asin(REPOSE) ~ 26 deg
static const int FLAKES = 24, BUBBLES = 14, DUST = 48, HIST = 112;

static const uint16_t AIR = rgb(226, 240, 244), OUTLINE = rgb(20, 30, 45), WHITE = rgb(250, 250, 245);
static const uint16_t BAR_BG = rgb(255, 255, 255), FOOD_BAR = rgb(250, 160, 60), BUBBLE = rgb(215, 245, 255);
static const uint16_t SAND_C = rgb(222, 200, 150), INK = rgb(60, 70, 80);
static const uint16_t SANDC[4] = {SAND_C, SAND_C, rgb(190, 165, 115), rgb(240, 225, 185)};

struct Species { float rx, ry, speed; uint16_t body, belly, tail; int pattern; };
enum { CLOWN, TANG, YELLOW, NEON };
static const Species SPECIES[] = {
  {7, 4.5f, 16, rgb(250, 120, 30), rgb(255, 170, 90), rgb(240, 100, 25), CLOWN},
  {8, 5, 14, rgb(40, 90, 210), rgb(90, 140, 235), rgb(250, 210, 40), TANG},
  {6, 5, 12, rgb(250, 215, 40), rgb(255, 240, 120), rgb(245, 200, 30), YELLOW},
  {4.5f, 2, 22, rgb(60, 190, 240), rgb(230, 60, 70), rgb(150, 200, 230), NEON},
};

enum { GONE, ALIVE, DEAD };
struct Fish {
  int kind, state, dir;
  float x, y, vx, vy, tx, ty, hunger, phase, size, fed_s;
  uint32_t retarget, startle_until, sparkle_until;
};
struct Flake { float x, y, t0; bool live; uint16_t c; };
struct Bubble { float x, y, r, phase; bool live; };
struct Dust { float x, y, vx, vy; bool live; uint8_t grain; };  // a stirred-up sand grain; lands back in the grid

// Everything lives in fixed portrait canvas pixels; only fish, snail and HUD are drawn upright for the viewer (rot).
static struct {
  Fish fish[FISH] = {};
  Flake flakes[FLAKES] = {};
  Bubble bubbles[BUBBLES] = {};
  Dust dust[DUST] = {};
  struct { float x, y, tx, ty; int dir; } snail = {70, SAND, 70, SAND, 1};
  uint32_t last_ms = 0, last_feed = 0, last_approach = 0, last_visitor = 0, next_bubble = 0;
  float ripple_x = 0, ripple_y = 0;
  uint32_t ripple_t0 = 0;
  float wa = 0, wa_v = 0, wa_target = 0;  // water's down as an angle (0 = screen bottom); lags gravity like a damped spring
  float sand_acc = 0, sand_rest = 99;     // grain steps owed; gravity angle the sand last settled under
  int sand_idle = 0;
  bool sand_awake = true;
  int rot = 0;
  bool lamp_on = true;
  int last_minute = -1;
  float light = 1;
  int visitors = 0, visitor_day = -1;
  int sig = -100;
  bool debug = true;
  struct { int8_t sig, base; bool hit; } hist[HIST] = {};
  int hist_i = 0;
  struct { int fish = -1; char text[112] = {}; uint32_t until = 0; } bubble;  // text in Latin-1 for the font
} S;

static uint32_t seed = 12345;
static float rnd() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; }
static uint32_t xs = 2463534242u;
static uint32_t xr() { xs ^= xs << 13, xs ^= xs >> 17, xs ^= xs << 5; return xs; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float wrapf(float a) { return a > PI_F ? a - 2 * PI_F : a < -PI_F ? a + 2 * PI_F : a; }

// Sand grid in the host's scratch buffer: 0 water, 1-3 grain colour, ROCK. moved[] stamps grains that moved this step.
static const uint8_t ROCK = 0x40;
static uint8_t *G, *moved, stamp;
static uint16_t *ovl, *spr;  // HUD layer while the view turns; one creature at a time, drawn upright before rotating
static const int SP = 48;
static const uint16_t KEY = 0x0821;  // transparent in ovl and spr
static bool solid(int x, int y) { return x < 0 || y < 0 || x >= CW || y >= CH || G[y * CW + x]; }

// Water: GX/GY = unit "down" of the surface, SURF_S = dot(p, g) along the surface line.
static float GX = 0, GY = 1, SURF_S = SURFACE;
static float depth(float x, float y) { return GX * x + GY * y - SURF_S; }  // > 0 under water
static float SIN[256];
static float wave(float u, float t) { return 1.2f * SIN[(int)((1.8f * t + 0.18f * u) * 40.743665f) & 255]; }

// ---- drawing ----
// Frame r = canvas turned r quarter turns: the viewer's upright when edge r points down. W x H is the frame size.
static uint16_t *C;
static int W = CW, H = CH, R = 0;
static float view = 0;  // drawn rotation of creatures and HUD; eases to -rot * 90 deg instead of snapping
static const int EX[4][2] = {{1, 0}, {0, -1}, {-1, 0}, {0, 1}};  // frame x axis in canvas pixels; frame y is (-ex.y, ex.x)

static void frame(int r) { R = r, W = r & 1 ? CH : CW, H = r & 1 ? CW : CH; }

static uint16_t *at(int x, int y) {
  switch (R) {
    case 0: return C + y * W + x;
    case 1: return C + (CH - 1 - x) * CW + y;
    case 2: return C + (CH - 1 - y) * CW + CW - 1 - x;
    default: return C + x * CW + CW - 1 - y;
  }
}

static void to_frame(int r, float X, float Y, float &x, float &y) {
  switch (r) {
    case 0: x = X, y = Y; break;
    case 1: x = CH - 1 - Y, y = X; break;
    case 2: x = CW - 1 - X, y = CH - 1 - Y; break;
    default: x = Y, y = CW - 1 - X; break;
  }
}

static void px(int x, int y, uint16_t c) {
  if (x >= 0 && x < W && y >= 0 && y < H) *at(x, y) = c;
}

static bool in_ell(float x, float y, float cx, float cy, float rx, float ry) {
  float dx = (x + 0.5f - cx) / rx, dy = (y + 0.5f - cy) / ry;
  return dx * dx + dy * dy <= 1;
}

// Same pixels as testing in_ell per pixel, but one sqrt per row; rows outside [ya, yb] are skipped.
static void ell(float cx, float cy, float rx, float ry, uint16_t c, int ya = -(1 << 20), int yb = 1 << 20) {
  int y0 = (int)floorf(cy - ry - 1), y1 = (int)ceilf(cy + ry + 1);
  if (y0 < ya) y0 = ya;
  if (y1 > yb) y1 = yb;
  for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < H; y++) {
    float dy = (y + 0.5f - cy) / ry, k = 1 - dy * dy;
    if (k < 0) continue;
    float hw = rx * sqrtf(k);
    int xa = (int)ceilf(cx - hw - 0.5f), xb = (int)floorf(cx + hw - 0.5f);
    if (xa < 0) xa = 0;
    if (xb > W - 1) xb = W - 1;
    if (!R)
      for (uint16_t *p = C + y * W + xa, *e = C + y * W + xb; p <= e; p++) *p = c;
    else
      for (int x = xa; x <= xb; x++) *at(x, y) = c;
  }
}

static void line(float x0, float y0, float x1, float y1, uint16_t c) {
  int n = (int)fmaxf(fabsf(x1 - x0), fabsf(y1 - y0)) + 1;
  for (int i = 0; i <= n; i++) px((int)(x0 + (x1 - x0) * i / n), (int)(y0 + (y1 - y0) * i / n), c);
}

// ---- water and sand ----
static float air_area(float s) {
  float a = 0;
  if (fabsf(GY) >= fabsf(GX))
    for (int x = 0; x < CW; x++) {
      float l = clampf((s - GX * (x + 0.5f)) / GY, 0, CH);
      a += GY > 0 ? l : CH - l;
    }
  else
    for (int y = 0; y < CH; y++) {
      float l = clampf((s - GY * (y + 0.5f)) / GX, 0, CW);
      a += GX > 0 ? l : CW - l;
    }
  return a;
}

static void solve_surface() {  // the level line that leaves the same air volume, whatever the pose
  float c[] = {0, GX * CW, GY * CH, GX * CW + GY * CH}, lo = c[0], hi = c[0];
  for (float v : c) lo = fminf(lo, v), hi = fmaxf(hi, v);
  for (int i = 0; i < 18; i++) {
    float mid = (lo + hi) / 2;
    (air_area(mid) < CW * SURFACE ? lo : hi) = mid;
  }
  SURF_S = (lo + hi) / 2;
}

static float sand_top(int x) { return SAND + sinf(x * 0.08f) * 2.5f + sinf(x * 0.21f + 1) * 1.0f; }

static void rock_mask(float cx, float cy, float rx, float ry) {
  for (int y = (int)(cy - ry - 1); y <= (int)(cy + ry + 1); y++)
    for (int x = (int)(cx - rx - 1); x <= (int)(cx + rx + 1); x++)
      if (x >= 0 && x < CW && y >= 0 && y < CH && in_ell(x, y, cx, cy, rx, ry)) G[y * CW + x] = ROCK;
}

static void fill_sand() {
  memset(G, 0, CW * CH * 2);
  rock_mask(24, SAND + 1, 15, 9);  // decor is glued to the glass; sand flows around it
  rock_mask(44, SAND + 6, 8, 5);
  for (int x = 0; x < CW; x++)
    for (int y = (int)sand_top(x); y < CH; y++) {
      uint32_t h = (x * 73856093u) ^ (y * 19349663u);
      uint8_t &g = G[y * CW + x];
      if (!g) g = h % 11 == 0 ? 2 : h % 17 == 0 ? 3 : 1;
    }
}

// Moves a grain may make under gravity (gx, gy): the 5x5 neighbourhood below the angle of repose, steepest first.
struct Step { int8_t x, y, mx, my; float k; };
static Step steps[24];
static int nsteps;

static void sand_steps(float gx, float gy) {
  nsteps = 0;
  for (int y = -2; y <= 2; y++)
    for (int x = -2; x <= 2; x++) {
      float len = sqrtf((float)(x * x + y * y)), p = x * gx + y * gy;
      if (len == 0 || p < REPOSE * len) continue;
      Step s = {(int8_t)x, (int8_t)y, (int8_t)(x / 2), (int8_t)(y / 2), p + 0.3f * p / len};
      int i = nsteps++;
      for (; i > 0 && steps[i - 1].k < s.k; i--) steps[i] = steps[i - 1];
      steps[i] = s;
    }
}

static bool try_step(int x, int y, const Step &s) {
  int nx = x + s.x, ny = y + s.y;
  if (solid(nx, ny) || ((s.mx || s.my) && solid(x + s.mx, y + s.my))) return false;  // two-cell moves need a free middle
  G[ny * CW + nx] = G[y * CW + x], G[y * CW + x] = 0, moved[ny * CW + nx] = stamp;
  return true;
}

// One step of falling sand, scanned from the low side so grains below move out of the way first.
static int sand_step(float gx, float gy) {
  if (!++stamp) memset(moved, 0, CW * CH), stamp = 1;
  uint32_t r = xr();
  int xd = gx > 0.05f ? -1 : gx < -0.05f ? 1 : (r & 1 ? -1 : 1), yd = gy > 0.05f ? -1 : gy < -0.05f ? 1 : (r & 2 ? -1 : 1);
  int x0 = xd < 0 ? CW - 1 : 0, y0 = yd < 0 ? CH - 1 : 0, moves = 0;
  bool rows = fabsf(gy) >= fabsf(gx);
  for (int a = 0, na = rows ? CH : CW, nb = rows ? CW : CH; a < na; a++)
    for (int b = 0; b < nb; b++) {
      int x = x0 + xd * (rows ? b : a), y = y0 + yd * (rows ? a : b), i = y * CW + x;
      uint8_t g = G[i];
      if (!g || g == ROCK || moved[i] == stamp || !(xr() & 7)) continue;  // 1 in 8 waits a step: flows grainy, not as a block
      for (int k = 0; k < nsteps; k++) {
        bool pair = k + 1 < nsteps && steps[k].k - steps[k + 1].k < 1e-3f;  // mirror-image moves in random order
        int first = pair && (xr() & 1) ? k + 1 : k;
        if (try_step(x, y, steps[first]) || (pair && try_step(x, y, steps[first == k ? k + 1 : k]))) {
          moves++;
          break;
        }
        k += pair;
      }
    }
  return moves;
}

static void update_sand(float gx, float gy, float dt) {
  if (gx * gx + gy * gy < 0.09f) return;  // lying flat: the sand stays put
  float a = atan2f(gx, gy);
  if (fabsf(wrapf(a - S.sand_rest)) > 0.05f) S.sand_awake = true;
  if (!S.sand_awake) return;
  sand_steps(sinf(a), cosf(a));
  for (S.sand_acc = fminf(3, S.sand_acc + dt * SAND_STEPS_PER_S); S.sand_acc >= 1; S.sand_acc--)
    S.sand_idle = sand_step(sinf(a), cosf(a)) ? 0 : S.sand_idle + 1;
  if (S.sand_idle > 3) S.sand_awake = false, S.sand_rest = a;
}

static void land(Dust &d) {  // back into the grid: the cell it is in, or the first free one above
  for (int k = 0; k < 10; k++) {
    int x = (int)(d.x - GX * k), y = (int)(d.y - GY * k);
    if (!solid(x, y)) {
      G[y * CW + x] = d.grain;
      break;
    }
  }
  d.live = false;
  S.sand_awake = true;
}

// ---- fish ----
static Fish make_fish(int kind, float x, float y, float size) {
  Fish f = {};
  f.kind = kind, f.state = ALIVE, f.dir = 1, f.x = f.tx = x, f.y = f.ty = y;
  f.hunger = 0.3f, f.phase = rnd() * 6.28f, f.size = size;
  return f;
}

static void stock() {
  static const int KINDS[] = {CLOWN, TANG, YELLOW, NEON, NEON};
  for (int i = 0; i < FISH; i++) S.fish[i] = i < 5 ? make_fish(KINDS[i], 20 + rnd() * (CW - 40), 40 + rnd() * (SAND - 60), 1) : Fish{};
}

static void init_scene();

void pet_init(uint32_t now, uint8_t *scratch) {
  G = scratch, moved = G + CW * CH, ovl = (uint16_t *)(G + 2 * CW * CH), spr = ovl + CW * CH;
  S.last_ms = now;
  S.last_feed = S.last_approach = now - FEED_COOLDOWN;
  S.last_visitor = now - VISITOR_GAP;
  for (auto &h : S.hist) h = {-100, -100, false};
  for (int i = 0; i < 256; i++) SIN[i] = sinf(i * 2 * PI_F / 256);
  stock();
  fill_sand();
  solve_surface();
  init_scene();
}

void pet_get_save(PetSave &s) {
  s = {};
  s.magic = PET_SAVE_MAGIC;
  for (int i = 0; i < FISH; i++) {
    const Fish &f = S.fish[i];
    s.kind[i] = f.kind, s.state[i] = f.state, s.hunger[i] = f.hunger, s.size[i] = f.size, s.fed_s[i] = f.fed_s;
  }
  s.visitors = S.visitors, s.visitor_day = S.visitor_day;
}

void pet_load(const PetSave &s) {
  if (s.magic != PET_SAVE_MAGIC) return;
  for (int i = 0; i < FISH; i++) {
    Fish &f = S.fish[i];
    f = make_fish(s.kind[i] % 4, 20 + rnd() * (CW - 40), 40 + rnd() * (SAND - 60), s.size[i]);
    f.state = s.state[i], f.hunger = s.hunger[i], f.fed_s = s.fed_s[i];
  }
  S.visitors = s.visitors, S.visitor_day = s.visitor_day;
}

void pet_advance(float seconds) {
  for (auto &f : S.fish) {
    if (f.state != ALIVE) continue;
    float well_fed = fmaxf(0, fminf(seconds, (WELL_FED - f.hunger) / HUNGER_PER_S));
    f.fed_s += well_fed;
    f.size = fminf(1, f.size + seconds * GROW_PER_S);
    if ((f.hunger += seconds * HUNGER_PER_S) >= 1) f.hunger = 1, f.state = DEAD;
  }
}

void pet_status(PetStatus &s) {
  s = {};
  for (auto &f : S.fish) {
    if (f.state == DEAD) s.dead++;
    if (f.state != ALIVE) continue;
    s.alive++;
    if (f.size < 1) s.young++;
    s.hunger_avg += f.hunger;
    s.hunger_max = fmaxf(s.hunger_max, f.hunger);
  }
  if (s.alive) s.hunger_avg /= s.alive;
  s.visitors_today = S.visitors;
  s.lamp_on = S.lamp_on, s.light = S.light;
}

static void new_target(Fish &f, uint32_t now, bool sleepy) {
  for (int k = 0; k < 12; k++) {  // a free spot under water; sleepy fish want one just above the sand
    float x = 10 + rnd() * (CW - 20), y = 10 + rnd() * (CH - 20);
    if (depth(x, y) < 12 || solid((int)x, (int)y) || solid((int)(x + GX * 8), (int)(y + GY * 8))) continue;
    if (sleepy && !solid((int)(x + GX * 22), (int)(y + GY * 22))) continue;
    f.tx = x, f.ty = y;
    break;
  }
  f.retarget = now + (sleepy ? 6000 : 2500) + (uint32_t)(rnd() * 4000);
}

static bool drop_food(uint32_t now) {
  if (now - S.last_feed <= FEED_COOLDOWN) return false;
  S.last_feed = now;
  float k = SURF_S - (GX * CW + GY * CH) / 2;  // centre of the tank, moved onto the surface
  int n = 0;
  for (auto &fl : S.flakes) {
    if (fl.live || n >= 12) continue;
    static const uint16_t C[] = {rgb(230, 120, 50), rgb(200, 70, 50), rgb(160, 110, 60)};
    float u = (rnd() - 0.5f) * 60, h = k - n * 1.5f;
    fl = {clampf(CW / 2 + GX * h + GY * u, 2, CW - 3), clampf(CH / 2 + GY * h - GX * u, 2, CH - 3), now / 1000.0f + rnd() * 6, true, C[n % 3]};
    n++;
  }
  return true;
}

bool pet_feed(uint32_t now) { return drop_food(now); }

bool pet_fish(int slot, PetFish &f) {
  if (slot < 0 || slot >= FISH || S.fish[slot].state == GONE) return false;
  const Fish &x = S.fish[slot];
  f = {x.kind, x.state, x.hunger, x.size};
  return true;
}

// UTF-8 in, Latin-1 out (the font covers 0x20-0xFF); anything else, emoji included, is dropped.
void pet_say(int slot, const char *s, uint32_t now) {
  int n = 0;
  for (const uint8_t *p = (const uint8_t *)s; *p && n < (int)sizeof S.bubble.text - 1; p++) {
    uint32_t c = *p;
    if (c >= 0xC2 && c <= 0xC3 && (p[1] & 0xC0) == 0x80) c = ((c & 0x1F) << 6) | (*++p & 0x3F);
    else if (c >= 0x80) {
      while ((p[1] & 0xC0) == 0x80) p++;
      continue;
    }
    if (c == '\n' || c == '\r') c = ' ';
    if (c >= 0x20 && c != 0x7F && (c < 0x80 || c >= 0xA0)) S.bubble.text[n++] = (char)c;
  }
  S.bubble.text[n] = 0;
  S.bubble.fish = slot;
  S.bubble.until = now + 3500 + 80 * n;
}
void pet_toggle_lamp() { S.lamp_on = !S.lamp_on; }

static void startle(Fish &f, float fromx, float fromy, uint32_t now) {
  float dx = f.x - fromx, dy = f.y - fromy, d = sqrtf(dx * dx + dy * dy) + 0.01f;
  f.tx = f.x + dx / d * 45, f.ty = f.y + dy / d * 45;
  f.startle_until = now + 700;
  f.retarget = now + 1500;
}

static bool resting(const Flake &fl) { return solid((int)(fl.x + GX * 2.5f), (int)(fl.y + GY * 2.5f)); }

static bool lamp_changed;

static void update_light(const PetInput &in, float dt) {
  bool was_on = S.lamp_on;
  if (in.minute >= 0) {  // switch on the scheduled edges only, so a manual toggle holds until the next one
    if (S.last_minute >= 0 && S.last_minute < LAMP_ON_MIN && in.minute >= LAMP_ON_MIN) S.lamp_on = true;
    if (S.last_minute >= 0 && S.last_minute < LAMP_OFF_MIN && in.minute >= LAMP_OFF_MIN) S.lamp_on = false;
    if (S.last_minute < 0) S.lamp_on = in.minute >= LAMP_ON_MIN && in.minute < LAMP_OFF_MIN;
    S.last_minute = in.minute;
  }
  if (S.lamp_on != was_on) lamp_changed = true;
  float target = S.lamp_on ? 1 : NIGHT;
  S.light += (target - S.light) * fminf(1, dt * 0.8f);
}

void pet_update(const PetInput &in, PetOutput &out) {
  uint32_t now = in.now;
  float dt = fminf(0.2f, (now - S.last_ms) / 1000.0f), t = now / 1000.0f;
  S.last_ms = now;
  out = {0, 0, 0, -1};
  S.rot = in.rot & 3;
  float turn = wrapf(-S.rot * PI_F / 2 - view);
  view = fabsf(turn) < 0.005f ? -S.rot * PI_F / 2 : wrapf(view + turn * fminf(1, dt * 10));

  S.sig = in.sig;
  if (in.sample) {
    S.hist[S.hist_i] = {(int8_t)in.sig, (int8_t)in.base, in.approach};
    S.hist_i = (S.hist_i + 1) % HIST;
  } else if (in.approach) {
    S.hist[(S.hist_i + HIST - 1) % HIST].hit = true;
  }
  if (in.day >= 0 && in.day != S.visitor_day) S.visitor_day = in.day, S.visitors = 0;
  if (in.approach) {
    S.last_approach = now;
    if (now - S.last_visitor > VISITOR_GAP) S.visitors++, S.last_visitor = now;
    if (drop_food(now)) out.beep_ms = 15, out.events |= EV_FED;
  }

  lamp_changed = false;
  update_light(in, dt);
  if (lamp_changed) out.events |= EV_LAMP;
  out.backlight = (int)(40 + 60 * (S.light - NIGHT) / (1 - NIGHT));
  bool sleepy = S.light < 0.6f;

  // the surface stays perpendicular to gravity; spring + damping on its angle gives a little slosh
  if (in.gx * in.gx + in.gy * in.gy > 0.09f) S.wa_target = atan2f(in.gx, in.gy);
  S.wa_v += (wrapf(S.wa_target - S.wa) * 28 - S.wa_v * 3.5f) * dt;
  S.wa = wrapf(S.wa + S.wa_v * dt);
  GX = sinf(S.wa), GY = cosf(S.wa);
  solve_surface();
  update_sand(in.gx, in.gy, dt);

  float lx, ly;
  to_frame(S.rot, in.tx, in.ty, lx, ly);
  int lw = S.rot & 1 ? CH : CW;
  if (in.tap && lx >= lw - 24 && ly <= 14) {  // signal icon toggles the graph
    S.debug = !S.debug;
  } else if (in.tap && lx >= lw - 66 && lx < lw - 24 && ly <= 12) {  // the lamp
    S.lamp_on = !S.lamp_on;
    out.beep_ms = 8, out.events |= EV_LAMP;
  } else if (in.tap) {
    bool any = false, scooped = false;
    for (auto &f : S.fish) any |= f.state != GONE;
    if (!any) {  // empty tank: restock
      stock();
      out.beep_ms = 40;
      scooped = true;
    }
    for (auto &f : S.fish)  // tapping a dead fish scoops it out
      if (!scooped && f.state == DEAD && fabsf(f.x - in.tx) < 12 && fabsf(f.y - in.ty) < 12) f.state = GONE, scooped = true;
    for (int i = 0; i < FISH && !scooped; i++) {  // tapping a live fish makes it talk
      const Fish &f = S.fish[i];
      float r = SPECIES[f.kind].rx * f.size + 4;
      if (f.state == ALIVE && fabsf(f.x - in.tx) < r && fabsf(f.y - in.ty) < r) out.tapped_fish = i, scooped = true;
    }
    if (!scooped) {
      S.ripple_x = in.tx, S.ripple_y = in.ty, S.ripple_t0 = now;
      for (auto &f : S.fish)  // tap on the glass: nearby fish dart away
        if (f.state == ALIVE && fabsf(f.x - in.tx) + fabsf(f.y - in.ty) < 50) startle(f, in.tx, in.ty, now);
    }
  }

  if (in.shake) {  // stir up the top layer of sand, everyone panics
    int n = 0;
    for (int k = 0; k < 600 && n < DUST; k++) {
      int x = (int)(rnd() * CW), y = (int)(rnd() * CH);
      uint8_t g = G[y * CW + x];
      if (!g || g == ROCK || solid((int)(x - GX * 1.5f), (int)(y - GY * 1.5f))) continue;
      while (n < DUST && S.dust[n].live) n++;
      if (n == DUST) break;
      float up = 12 + rnd() * 28, side = (rnd() - 0.5f) * 24;
      S.dust[n] = {x + 0.5f, y + 0.5f, -GX * up + GY * side, -GY * up - GX * side, true, g};
      G[y * CW + x] = 0;
    }
    S.sand_awake = true;
    for (auto &f : S.fish)
      if (f.state == ALIVE) startle(f, f.x + (rnd() - 0.5f) * 10, f.y + (rnd() - 0.5f) * 10, now);
    S.wa_v += (rnd() - 0.5f) * 1.5f;
    out.beep_ms = 20;
  }
  for (auto &d : S.dust) {
    if (!d.live) continue;
    float drag = 1 - fminf(1, 1.2f * dt);
    d.vx = (d.vx + GX * 14 * dt) * drag, d.vy = (d.vy + GY * 14 * dt) * drag;
    float nx = d.x + d.vx * dt, ny = d.y + d.vy * dt;
    if (solid((int)nx, (int)ny)) land(d);
    else d.x = nx, d.y = ny;
  }

  for (auto &fl : S.flakes) {
    if (!fl.live) continue;
    if (!resting(fl)) {
      float w = sinf(t * 2 + fl.t0) * 3;
      fl.x += (GX * 7 + GY * w) * dt, fl.y += (GY * 7 - GX * w) * dt;
    } else if (t - fl.t0 > 60) {
      fl.live = false;  // rots on the sand unless the snail gets it
    }
    fl.x = clampf(fl.x, 2, CW - 3), fl.y = clampf(fl.y, 1, CH - 2);
  }

  // the snail crawls over the glass towards settled flakes, otherwise to some spot on the sand
  Flake *crumb = nullptr;
  float best = 1e9;
  for (auto &fl : S.flakes) {
    float d = (fl.x - S.snail.x) * (fl.x - S.snail.x) + (fl.y - S.snail.y) * (fl.y - S.snail.y);
    if (fl.live && resting(fl) && d < best) best = d, crumb = &fl;
  }
  float sdx = S.snail.tx - S.snail.x, sdy = S.snail.ty - S.snail.y, sd = sqrtf(sdx * sdx + sdy * sdy);
  if (crumb) S.snail.tx = crumb->x, S.snail.ty = crumb->y;
  else if (sd < 1)
    for (int k = 0; k < 12; k++) {
      float x = 8 + rnd() * (CW - 16), y = 8 + rnd() * (CH - 16);
      if (!solid((int)x, (int)y) && solid((int)(x + GX * 3), (int)(y + GY * 3))) {
        S.snail.tx = x, S.snail.ty = y;
        break;
      }
    }
  if (sd > 0.5f) {
    S.snail.x += sdx / sd * 2.2f * dt, S.snail.y += sdy / sd * 2.2f * dt;
    float side = sdx * EX[S.rot][0] + sdy * EX[S.rot][1];
    if (fabsf(side) > 0.3f * sd) S.snail.dir = side > 0 ? 1 : -1;
  }
  if (crumb && best < 16) crumb->live = false;

  const float ex = EX[S.rot][0], ey = EX[S.rot][1];  // the viewer's horizontal: fish swim flatter up and down
  int free_slot = -1;
  for (int i = 0; i < FISH; i++)
    if (S.fish[i].state == GONE) free_slot = i;
  for (auto &f : S.fish) {
    if (f.state == GONE) continue;
    if (f.state == DEAD) {  // belly up, drifting to the surface
      float up = (depth(f.x, f.y) - 7) * fminf(1, dt * 0.4f), w = sinf(t * 0.4f + f.phase) * 2 * dt;
      f.x = clampf(f.x - GX * up + GY * w, 10, CW - 10), f.y = clampf(f.y - GY * up - GX * w, 10, CH - 10);
      continue;
    }
    const Species &sp = SPECIES[f.kind];
    if (f.hunger < WELL_FED) f.fed_s += dt;
    f.size = fminf(1, f.size + dt * GROW_PER_S);
    if ((f.hunger += dt * HUNGER_PER_S) >= 1) {
      f.hunger = 1, f.state = DEAD;
      out.beep_ms = 300, out.events |= EV_DIED;
      continue;
    }
    if (f.fed_s >= BREED_FED_S && f.size >= 1 && free_slot >= 0) {
      S.fish[free_slot] = make_fish(f.kind, f.x - ex * f.dir * 6, f.y - ey * f.dir * 6, 0.45f);
      S.fish[free_slot].hunger = f.hunger;
      free_slot = -1, f.fed_s = 0;
      out.beep_ms = 120, out.events |= EV_BABY;
    }
    Flake *food = nullptr;
    if (f.hunger > 0.2f) {
      float best = 1e9;
      for (auto &fl : S.flakes)
        if (fl.live) {
          float d = (fl.x - f.x) * (fl.x - f.x) + (fl.y - f.y) * (fl.y - f.y);
          if (d < best) best = d, food = &fl;
        }
    }
    bool startled = now < f.startle_until, resting_fish = sleepy && !food && !startled;
    if (food && !startled) f.tx = food->x, f.ty = food->y;
    else if (now > f.retarget || fabsf(f.tx - f.x) + fabsf(f.ty - f.y) < 4) new_target(f, now, resting_fish);
    float speed = sp.speed * (startled ? 4 : food ? 1.8f : resting_fish ? 0.3f : 1) * (1 - f.hunger * 0.4f) * (0.6f + 0.4f * f.size);
    float dx = f.tx - f.x, dy = f.ty - f.y, d = sqrtf(dx * dx + dy * dy) + 0.01f;
    float h = (dx * ex + dy * ey) / d * speed, v = (dy * ex - dx * ey) / d * speed * 0.6f;
    float k = fminf(1, dt * (startled ? 6 : 2.5f));
    f.vx += (ex * h - ey * v - f.vx) * k;
    f.vy += (ey * h + ex * v - f.vy) * k;
    float nx = clampf(f.x + f.vx * dt, 8, CW - 9), ny = clampf(f.y + f.vy * dt, 8, CH - 9), dep = depth(nx, ny);
    if (dep < 7) nx += GX * (7 - dep), ny += GY * (7 - dep);
    if (solid((int)nx, (int)ny) || solid((int)(nx + GX * 5), (int)(ny + GY * 5))) {  // sand underneath: swim up out of it
      nx = clampf(f.x - GX * 20 * dt, 8, CW - 9), ny = clampf(f.y - GY * 20 * dt, 8, CH - 9);
      f.retarget = 0;
    }
    f.x = nx, f.y = ny;
    float side = f.vx * ex + f.vy * ey;
    if (side > 1.5f) f.dir = 1;
    if (side < -1.5f) f.dir = -1;
    float mx = f.x + ex * f.dir * sp.rx * f.size, my = f.y + ey * f.dir * sp.rx * f.size;
    if (food && fabsf(food->x - mx) < 3 && fabsf(food->y - my) < 3) {
      food->live = false;
      f.hunger = fmaxf(0, f.hunger - 0.15f);
      f.sparkle_until = now + 350;
      if (!out.beep_ms) out.beep_ms = 6;
    }
  }

  if (now > S.next_bubble) {
    for (auto &b : S.bubbles)
      if (!b.live) { b = {24 + rnd() * 3, (float)SAND - 8, 1 + rnd() * 1.6f, rnd() * 6.28f, true}; break; }
    S.next_bubble = now + 250 + (uint32_t)(rnd() * 600);
  }
  for (auto &b : S.bubbles) {
    if (!b.live) continue;
    float v = (14 + b.r * 6) * dt;
    b.x -= GX * v, b.y -= GY * v;
    if (depth(b.x, b.y) < 1 || b.x < 0 || b.y < 0 || b.x >= CW || b.y >= CH) b.live = false;
  }
}

// Water colour by depth below the surface, plain and with a light ray.
static uint16_t water_lut[256], ray_lut[256];
static const uint16_t SURF_C = rgb(190, 235, 245);
static uint16_t weed_c[4][72], weed_e[4][72];
static const float WEED_X[] = {40, 32, 23, 13}, WEED_H[] = {58, 72, 50, 64}, WEED_P[] = {0, 1.7f, 3.1f, 4.4f};  // X from the right edge

static void init_scene() {
  for (int i = 0; i < 256; i++) {
    float d = fminf(1, i / (float)(SAND - SURFACE));
    water_lut[i] = mix(rgb(120, 205, 228), rgb(22, 72, 132), d);
    ray_lut[i] = d < 0.75f ? mix(water_lut[i], WHITE, 0.09f * (1 - d / 0.75f)) : water_lut[i];
  }
  for (int s = 0; s < 4; s++)
    for (int i = 0; i < (int)WEED_H[s]; i++) {
      weed_c[s][i] = mix(rgb(40, 140, 70), rgb(110, 200, 100), i / WEED_H[s]);
      weed_e[s][i] = mix(weed_c[s][i], OUTLINE, 0.5f);
    }
}

// Per pixel: sand grain, else air / surface / water by distance along gravity; the wave only near the surface.
static void draw_water(float t) {
  uint16_t *p = C;
  const uint8_t *g = G;
  for (int y = 0; y < CH; y++) {
    float d = GX * 0.5f + GY * (y + 0.5f) - SURF_S, u = GY * 0.5f - GX * (y + 0.5f);
    int ray = (y / 2) % 46;  // static light rays: (x + y / 2) % 46 < 9
    for (int x = 0; x < CW; x++, p++, g++, d += GX, u += GY) {
      if (*g && *g != ROCK) {
        *p = SANDC[*g & 3];
      } else {
        float e = d < -2 || d > 3 ? d : d - wave(u, t);
        int k = (int)e;
        *p = e < 0 ? AIR : e < 1 ? SURF_C : (ray < 9 ? ray_lut : water_lut)[k > 255 ? 255 : k];
      }
      if (++ray == 46) ray = 0;
    }
  }
  ell(24, SAND + 1, 15, 9, OUTLINE);  // rocks with the bubbler on top
  ell(24, SAND + 1, 14, 8, rgb(120, 125, 135));
  ell(20, SAND - 3, 7, 3, rgb(150, 155, 165));
  ell(44, SAND + 6, 8, 5, OUTLINE);
  ell(44, SAND + 6, 7, 4, rgb(105, 110, 120));
}

// Rooted where the sand started; the stem bends round to point against the water's down.
static void draw_seaweed(float t) {
  float bend = wrapf(S.wa);
  for (int s = 0; s < 4; s++) {
    float x = CW - WEED_X[s], y = sand_top((int)x), sx = 0, sy = -1;
    for (int i = 0; i < (int)WEED_H[s]; i++) {
      if (i <= 24) {
        float a = PI_F + bend * i / 24.0f;
        sx = sinf(a), sy = cosf(a);
      }
      x += sx, y += sy;
      float f = i / WEED_H[s], sway = sinf(t * 1.2f + i * 0.08f + WEED_P[s]) * f * 4;
      float cx = x - sy * sway, cy = y + sx * sway;  // sideways is (-sy, sx)
      uint16_t c = weed_c[s][i], e = weed_e[s][i];
      for (int k = -1; k <= 2; k++) px((int)(cx - sy * k), (int)(cy + sx * k), k == -1 || k == 2 ? e : c);
      if (i % 9 == 4) {  // leaf nub
        float k = s % 2 ? 3 : -2;
        px((int)(cx - sy * k), (int)(cy + sx * k), c);
      }
    }
  }
}

static void draw_snail(int x, int y, int d) {
  uint16_t body = rgb(205, 195, 150), shell = rgb(170, 105, 55), shell_dark = rgb(115, 65, 35);
  for (int i = -4; i <= 4; i++) px(x + i, y - 1, body);
  for (int i = -2; i <= 4; i++) px(x + i * d, y - 2, body);
  px(x + d * 4, y - 3, body), px(x + d * 5, y - 4, OUTLINE), px(x + d * 3, y - 4, OUTLINE);  // eye stalks
  ell(x - d * 1.0f, y - 4.5f, 4.2f, 3.8f, shell_dark);
  ell(x - d * 1.0f, y - 4.5f, 3.3f, 2.9f, shell);
  for (int i = 0; i < 14; i++) {  // shell spiral
    float a = i * 0.7f, r = 2.6f - i * 0.17f;
    px((int)(x - d + cosf(a) * r * d), (int)(y - 4.5f + sinf(a) * r), shell_dark);
  }
}

static void draw_fish(const Fish &f, uint32_t now, bool sleepy) {
  const Species &sp = SPECIES[f.kind];
  bool dead = f.state == DEAD;
  int up = dead ? -1 : 1;  // dead fish float belly up
  float hungry = dead ? 0.85f : fmaxf(0, f.hunger - 0.5f) * 0.8f;  // fades towards grey when starving
  uint16_t grey = rgb(130, 130, 140);
  uint16_t body = mix(sp.body, grey, hungry), belly = mix(sp.belly, grey, hungry), tail = mix(sp.tail, grey, hungry);
  uint16_t edge = mix(body, OUTLINE, 0.6f);
  int d = f.dir;
  float cx = f.x, cy = f.y, rx = sp.rx * f.size, ry = sp.ry * f.size;
  float wig = dead ? 0 : sinf(now * 0.012f * (1 + fabsf(f.vx + f.vy) * 0.05f) + f.phase);

  for (int i = 0; i < (int)(rx * 0.7f) + 2; i++) {  // tail fan
    float x = cx - d * (rx + i - 1), half = 1 + i * ry * 0.22f, off = wig * i * 0.35f;
    for (int y = (int)(cy - half + off); y <= (int)(cy + half + off); y++)
      px((int)x, y, (y == (int)(cy - half + off) || y == (int)(cy + half + off)) ? edge : tail);
  }
  for (int k = -2; k <= 2; k++) {  // dorsal fin
    int h = (int)((2 - (k < 0 ? -k : k) / 2) * f.size + 0.5f);
    for (int j = 1; j <= h; j++) px((int)(cx - d * k), (int)(cy - up * (ry + j - 1)), j == h ? edge : tail);
  }
  ell(cx, cy, rx + 0.8f, ry + 0.8f, edge);
  ell(cx, cy, rx, ry, body);
  ell(cx, cy, rx, ry, belly, (int)(cy + (dead ? -ry : ry * 0.25f)), (int)(cy + (dead ? -ry * 0.25f : ry)));

  if (sp.pattern == CLOWN)
    for (float bx : {cx + d * rx * 0.45f, cx - d * rx * 0.2f})
      for (int y = (int)(cy - ry); y <= (int)(cy + ry); y++)
        for (int x = (int)bx - 1; x <= (int)bx + 1; x++)
          if (in_ell(x, y, cx, cy, rx, ry)) px(x, y, x == (int)bx ? WHITE : OUTLINE);
  if (sp.pattern == TANG)
    for (int i = -4; i <= 4; i++) px((int)(cx - d * i * f.size), (int)(cy - up + up * (i * i) / 8), OUTLINE);
  if (sp.pattern == NEON)
    for (int x = (int)(cx - rx); x <= (int)(cx + rx); x++)
      if (in_ell(x, cy - up, cx, cy, rx, ry)) px(x, (int)(cy - up), dead ? grey : rgb(120, 240, 255));

  int ex = (int)(cx + d * rx * 0.55f), ey = (int)(cy - up * ry * 0.3f);
  if (dead) {
    for (int k = -1; k <= 1; k++) px(ex + k, ey + k, OUTLINE), px(ex + k, ey - k, OUTLINE);
  } else if (sleepy) {
    px(ex, ey, OUTLINE), px(ex + d, ey, OUTLINE);
  } else {
    px(ex, ey, WHITE), px(ex + d, ey, OUTLINE);
    if (ry > 3) px(ex, ey + 1, WHITE), px(ex + d, ey + 1, OUTLINE);
  }

  if (now < f.sparkle_until) {
    int sx = (int)(cx + d * (rx + 2)), sy = (int)(cy - ry - 2);
    px(sx, sy, WHITE), px(sx - 1, sy, BUBBLE), px(sx + 1, sy, BUBBLE), px(sx, sy - 1, BUBBLE), px(sx, sy + 1, BUBBLE);
  }
}

// Night: darken and cool everything drawn so far; the HUD and lamp are drawn afterwards at full brightness.
static void apply_light() {
  if (S.light > 0.98f) return;
  int lr = (int)(S.light * 0.8f * 256), lg = (int)(S.light * 0.9f * 256), lb = (int)(fminf(1, S.light * 1.15f) * 256);
  for (int i = 0; i < CW * CH; i++) {
    uint16_t c = C[i];
    C[i] = ((((c >> 11) * lr) >> 8) << 11) | (((((c >> 5) & 63) * lg) >> 8) << 5) | (((c & 31) * lb) >> 8);
  }
}

static void draw_lamp() {
  const int l = W - 62, r = W - 28;  // between the food bar and the signal icon
  for (int x = l; x <= r; x++)
    for (int y = 0; y <= 4; y++) px(x, y, (y == 4 || x == l || x == r) ? OUTLINE : rgb(70, 76, 88));
  for (int x = l + 3; x <= r - 3; x++) px(x, 5, S.lamp_on ? rgb(255, 215, 110) : rgb(95, 95, 100));
  if (S.lamp_on)  // warm glow fanning out under the lamp
    for (int y = 6; y <= 9; y++)
      for (int x = l + 3 - (y - 5) * 2; x <= r - 3 + (y - 5) * 2; x++)
        if (x >= 0 && x < W && *at(x, y) == AIR) px(x, y, mix(rgb(255, 225, 140), AIR, (y - 5) / 5.0f));
}

static void draw_digits(int x, int y, int v, uint16_t c) {
  static const char *D[] = {"111101101101111", "010110010010111", "111001111100111", "111001111001111", "101101111001001",
                            "111100111001111", "111100111101111", "111001001001001", "111101111101111", "111101111001111"};
  char buf[6];
  int n = 0;
  do buf[n++] = v % 10, v /= 10; while (v && n < 5);
  for (int i = n - 1; i >= 0; i--, x += 4)
    for (int k = 0; k < 15; k++)
      if (D[(int)buf[i]][k] == '1') px(x + k % 3, y + k / 3, c);
}

static void draw_hud(uint32_t now) {
  float hunger = 0;
  int alive = 0;
  for (auto &f : S.fish)
    if (f.state == ALIVE) hunger += f.hunger, alive++;
  int food = alive ? (int)((1 - hunger / alive) * 44) : 0;
  for (int x = 0; x < 44; x++)
    for (int y = 0; y < 3; y++) px(6 + x, 3 + y, x < food ? FOOD_BAR : BAR_BG);
  static const char *PERSON[] = {".1.", "111", ".1.", "1.1"};  // visitors today
  for (int y = 0; y < 4; y++)
    for (int x = 0; x < 3; x++)
      if (PERSON[y][x] == '1') px(6 + x, 9 + y, INK);
  draw_digits(12, 8, S.visitors, INK);
  int bars = S.sig >= APPROACH_MIN + 7 ? 4 : S.sig >= APPROACH_MIN ? 3 : S.sig >= -65 ? 2 : S.sig >= -80 ? 1 : 0;
  bool hot = now - S.last_approach < 1500;
  for (int i = 0; i < 4; i++)
    for (int h = 0; h <= i * 2 + 1; h++)
      for (int w = 0; w < 2; w++)
        px(W - 20 + i * 4 + w, 10 - h, i < bars ? (hot ? rgb(230, 40, 80) : INK) : rgb(190, 200, 205));
}

// Signal graph, oldest left: blue = strongest device, grey = its baseline, red dots = APPROACH_MIN, pink column = approach.
static void draw_graph() {
  const int X0 = 4, Y0 = 24, GH = 40, TOP = -30, RANGE = 55;
  auto y_of = [&](int dbm) { int y = Y0 + (TOP - dbm) * (GH - 1) / RANGE; return y < Y0 ? Y0 : y > Y0 + GH - 1 ? Y0 + GH - 1 : y; };
  for (int y = Y0 - 1; y <= Y0 + GH; y++)
    for (int x = X0 - 1; x <= X0 + HIST; x++)
      if (y == Y0 - 1 || y == Y0 + GH || x == X0 - 1 || x == X0 + HIST) px(x, y, rgb(200, 225, 235));
      else px(x, y, ((*at(x, y) & 0xF7DE) >> 1) + ((WHITE & 0xF7DE) >> 1));  // 50 % white without float maths
  int prev = -1;
  for (int i = 0; i < HIST; i++) {
    const auto &h = S.hist[(S.hist_i + i) % HIST];
    int x = X0 + i;
    if (h.hit)
      for (int y = Y0; y < Y0 + GH; y++) px(x, y, rgb(255, 190, 205));
    if (i % 3 == 0) px(x, y_of(APPROACH_MIN), rgb(230, 40, 80));
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

static int draw_glyph(int x, int baseline, uint8_t c, uint16_t col) {
  if (c < TomThumb.first || c > TomThumb.last) c = '?';
  const GFXglyph &g = TomThumb.glyph[c - TomThumb.first];
  const uint8_t *bm = TomThumb.bitmap + g.bitmapOffset;
  for (int i = 0, n = g.width * g.height; i < n; i++)
    if (bm[i >> 3] & (0x80 >> (i & 7))) px(x + g.xOffset + i % g.width, baseline + g.yOffset + i / g.width, col);
  return g.xAdvance;
}

// Word-wrapped bubble above (or, near the top, below) the speaking fish, with a little tail.
static void draw_bubble(uint32_t now) {
  if (S.bubble.fish < 0 || now >= S.bubble.until || S.fish[S.bubble.fish].state == GONE) return;
  const Fish &f = S.fish[S.bubble.fish];
  float fx, fy;
  to_frame(R, f.x, f.y, fx, fy);
  const int cols = (W - 10) / 4, max_lines = 4;
  const char *lines[max_lines];
  int lens[max_lines], n = 0, widest = 0;
  for (const char *p = S.bubble.text; *p && n < max_lines;) {
    while (*p == ' ') p++;
    int len = (int)strlen(p) < cols ? (int)strlen(p) : cols;
    if (p[len] && p[len] != ' ')
      for (int k = len; k > cols / 2; k--)
        if (p[k] == ' ') { len = k; break; }
    lines[n] = p, lens[n] = len, n++;
    widest = len > widest ? len : widest;
    p += len;
  }
  if (!n) return;
  int bw = widest * 4 + 5, bh = n * 6 + 3, fr = (int)(SPECIES[f.kind].ry * f.size);
  int bx = (int)fx - bw / 2, by = (int)fy - fr - bh - 5;
  bx = bx < 2 ? 2 : bx > W - bw - 2 ? W - bw - 2 : bx;
  bool below = by < 13;
  if (below) by = (int)fy + fr + 5;
  const uint16_t fill = rgb(255, 255, 248), ink = OUTLINE;
  for (int y = by; y < by + bh; y++)
    for (int x = bx; x < bx + bw; x++) {
      bool edge = y == by || y == by + bh - 1 || x == bx || x == bx + bw - 1;
      bool corner = (y == by || y == by + bh - 1) && (x == bx || x == bx + bw - 1);
      if (!corner) px(x, y, edge ? ink : fill);
    }
  int tx = (int)fx < bx + 3 ? bx + 3 : (int)fx > bx + bw - 4 ? bx + bw - 4 : (int)fx;
  for (int k = 1; k <= 3; k++) {  // tail towards the fish
    int ty = below ? by - k : by + bh - 1 + k;
    px(tx - 1, ty, ink), px(tx, ty, k < 3 ? fill : ink), px(tx + 1, ty, k < 2 ? fill : ink);
  }
  for (int i = 0; i < n; i++) {
    int x = bx + 3;
    for (int k = 0; k < lens[i]; k++) x += draw_glyph(x, by + 7 + i * 6, (uint8_t)lines[i][k], ink);
  }
}

// Copies pixels from src (rotated by angle a about (sx, sy)) onto the canvas around (X, Y); KEY stays transparent.
static void blit(const uint16_t *src, int sw, int sh, float sx, float sy, float a, int x0, int y0, int x1, int y1, float X, float Y) {
  float c = cosf(a), s = sinf(a);
  for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < CH; y++)
    for (int x = x0 < 0 ? 0 : x0; x <= x1 && x < CW; x++) {
      float dx = x + 0.5f - X, dy = y + 0.5f - Y, u = dx * c + dy * s + sx, v = dy * c - dx * s + sy;
      if (u < 0 || v < 0 || u >= sw || v >= sh) continue;
      uint16_t p = src[(int)v * sw + (int)u];
      if (p != KEY) C[y * CW + x] = p;
    }
}

template <class F> static void creature(float X, float Y, F draw) {  // drawn upright in spr, turned by view
  uint16_t *canvas = C;
  for (int i = 0; i < SP * SP; i++) spr[i] = KEY;
  C = spr, W = H = SP, R = 0;
  draw(SP / 2, SP / 2);
  C = canvas, frame(0);
  int x = (int)X, y = (int)Y;
  blit(spr, SP, SP, SP / 2 + 0.5f, SP / 2 + 0.5f, view, x - SP / 2, y - SP / 2, x + SP / 2, y + SP / 2, x + 0.5f, y + 0.5f);
}

void pet_draw(uint16_t *canvas, uint32_t now) {
  C = canvas;
  float t = now / 1000.0f;
  bool sleepy = S.light < 0.6f;
  frame(0);
  draw_water(t);
  draw_seaweed(t);

  float rt = (now - S.ripple_t0) / 1000.0f;
  if (S.ripple_t0 && rt < 0.5f) {
    float r = 4 + rt * 50;
    for (int a = 0; a < 64; a++)
      px((int)(S.ripple_x + cosf(a * 0.098f) * r), (int)(S.ripple_y + sinf(a * 0.098f) * r), BUBBLE);
  }
  for (auto &fl : S.flakes)
    if (fl.live) px((int)fl.x, (int)fl.y, fl.c), px((int)fl.x + 1, (int)fl.y, fl.c), px((int)fl.x, (int)fl.y + 1, mix(fl.c, OUTLINE, 0.3f));

  creature(S.snail.x, S.snail.y, [&](int x, int y) { draw_snail(x, y, S.snail.dir); });
  for (auto &f : S.fish)
    if (f.state != GONE)
      creature(f.x, f.y, [&](int x, int y) {
        Fish g = f;
        g.x = x, g.y = y;
        draw_fish(g, now, sleepy && f.state == ALIVE && now >= f.startle_until);
      });

  for (auto &d : S.dust)
    if (d.live) px((int)d.x, (int)d.y, SANDC[d.grain & 3]);
  for (auto &b : S.bubbles) {
    if (!b.live) continue;
    float o = sinf(t * 3 + b.phase) * 1.5f, x = b.x + GY * o, y = b.y - GX * o;
    if (b.r < 1.5f) px((int)x, (int)y, BUBBLE);
    else {
      for (int a = 0; a < 12; a++) px((int)(x + cosf(a * 0.524f) * b.r), (int)(y + sinf(a * 0.524f) * b.r), BUBBLE);
      px((int)(x - 0.5f), (int)(y - 0.5f), WHITE);
    }
  }
  apply_light();
  float turn = wrapf(view + S.rot * PI_F / 2);
  bool turning = fabsf(turn) > 0.005f;  // HUD goes to a layer that turns with the creatures
  if (turning) {
    for (int i = 0; i < CW * CH; i++) ovl[i] = KEY;
    C = ovl;
  }
  frame(S.rot);
  if (S.debug && !turning) draw_graph();  // the graph blends with the scene below, so it waits for the turn
  draw_lamp();
  draw_hud(now);
  draw_bubble(now);
  frame(0);
  if (turning) C = canvas, blit(ovl, CW, CH, CW / 2.0f, CH / 2.0f, turn, 0, 0, CW - 1, CH - 1, CW / 2.0f, CH / 2.0f);
}
