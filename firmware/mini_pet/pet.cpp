#include "pet.h"
#include <math.h>
#include <initializer_list>

static constexpr uint16_t rgb(int r, int g, int b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }

static uint16_t mix(uint16_t a, uint16_t b, float t) {
  int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31, br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
  return ((int)(ar + (br - ar) * t) << 11) | ((int)(ag + (bg - ag) * t) << 5) | (int)(ab + (bb - ab) * t);
}

static const uint32_t FEED_COOLDOWN = 8000;
static const float HUNGER_PER_S = 1.0f / 86400;  // a full fish dies after 24 h without food
static const int SURFACE = 14, SAND = 140;
static const int FLAKES = 24, BUBBLES = 14, HIST = 112;

static const uint16_t AIR = rgb(226, 240, 244), OUTLINE = rgb(20, 30, 45), WHITE = rgb(250, 250, 245);
static const uint16_t BAR_BG = rgb(255, 255, 255), FOOD_BAR = rgb(250, 160, 60), BUBBLE = rgb(215, 245, 255);

struct Species { float rx, ry, speed; uint16_t body, belly, tail; int pattern; };
enum { CLOWN, TANG, YELLOW, NEON };
static const Species SPECIES[] = {
  {7, 4.5f, 16, rgb(250, 120, 30), rgb(255, 170, 90), rgb(240, 100, 25), CLOWN},
  {8, 5, 14, rgb(40, 90, 210), rgb(90, 140, 235), rgb(250, 210, 40), TANG},
  {6, 5, 12, rgb(250, 215, 40), rgb(255, 240, 120), rgb(245, 200, 30), YELLOW},
  {4.5f, 2, 22, rgb(60, 190, 240), rgb(230, 60, 70), rgb(150, 200, 230), NEON},
};

enum { GONE, ALIVE, DEAD };
struct Fish { int kind; float x, y, vx, vy, tx, ty, hunger, phase; int dir; uint32_t retarget, startle_until, sparkle_until; int state; };
struct Flake { float x, y, t0; bool live; uint16_t c; };
struct Bubble { float x, y, r, phase; bool live; };

static struct {
  Fish fish[FISH];
  Flake flakes[FLAKES] = {};
  Bubble bubbles[BUBBLES] = {};
  uint32_t last_ms = 0, last_feed = 0, last_approach = 0, next_bubble = 0;
  float ripple_x = 0, ripple_y = 0;
  uint32_t ripple_t0 = 0;
  int sig = -100;
  bool debug = true;
  struct { int8_t sig, base; bool hit; } hist[HIST] = {};
  int hist_i = 0;
} S;

static uint32_t seed = 12345;
static float rnd() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; }
static float sand_y[CW];

static void stock(float hunger) {
  static const int KINDS[FISH] = {CLOWN, TANG, YELLOW, NEON, NEON};
  for (int i = 0; i < FISH; i++) {
    Fish &f = S.fish[i];
    f = {KINDS[i], 20 + rnd() * 80, 40 + rnd() * 80, 0, 0, 0, 0, hunger, rnd() * 6.28f, 1, 0, 0, 0, ALIVE};
    f.tx = f.x, f.ty = f.y;
  }
}

void pet_init(uint32_t now) {
  S.last_ms = now;
  S.last_feed = S.last_approach = now - FEED_COOLDOWN;
  for (auto &h : S.hist) h = {-100, -100, false};
  stock(0.3f);
  for (int x = 0; x < CW; x++) sand_y[x] = SAND + sinf(x * 0.08f) * 2.5f + sinf(x * 0.21f + 1) * 1.0f;
}

void pet_get_save(PetSave &s) {
  s.magic = PET_SAVE_MAGIC;
  for (int i = 0; i < FISH; i++) s.hunger[i] = S.fish[i].hunger, s.state[i] = S.fish[i].state;
}

void pet_load(const PetSave &s) {
  if (s.magic != PET_SAVE_MAGIC) return;
  for (int i = 0; i < FISH; i++) S.fish[i].hunger = s.hunger[i], S.fish[i].state = s.state[i];
}

void pet_advance(float seconds) {
  for (auto &f : S.fish)
    if (f.state == ALIVE && (f.hunger += seconds * HUNGER_PER_S) >= 1) f.hunger = 1, f.state = DEAD;
}

static void new_target(Fish &f, uint32_t now) {
  f.tx = 10 + rnd() * 100;
  f.ty = 26 + rnd() * 100;
  f.retarget = now + 2500 + (uint32_t)(rnd() * 4000);
}

static void drop_food(uint32_t now) {
  int n = 0;
  for (auto &fl : S.flakes) {
    if (fl.live || n >= 12) continue;
    static const uint16_t C[] = {rgb(230, 120, 50), rgb(200, 70, 50), rgb(160, 110, 60)};
    fl = {30 + rnd() * 60, (float)SURFACE - n * 1.5f, now / 1000.0f + rnd() * 6, true, C[n % 3]};
    n++;
  }
}

void pet_update(const PetInput &in, PetOutput &out) {
  uint32_t now = in.now;
  float dt = fminf(0.2f, (now - S.last_ms) / 1000.0f), t = now / 1000.0f;
  S.last_ms = now;
  out = {0, false};

  S.sig = in.sig;
  if (in.approach) S.last_approach = now;
  if (in.sample) {
    S.hist[S.hist_i] = {(int8_t)in.sig, (int8_t)in.base, in.approach};
    S.hist_i = (S.hist_i + 1) % HIST;
  } else if (in.approach) {
    S.hist[(S.hist_i + HIST - 1) % HIST].hit = true;
  }
  if (in.approach && now - S.last_feed > FEED_COOLDOWN) {
    S.last_feed = now;
    drop_food(now);
    out.beep_ms = 15;
    out.fed = true;
  }

  if (in.tap && in.tx >= 92 && in.ty <= 14) {  // signal icon toggles the graph
    S.debug = !S.debug;
  } else if (in.tap) {
    bool any = false, scooped = false;
    for (auto &f : S.fish) any |= f.state != GONE;
    if (!any) {  // empty tank: restock
      stock(0.3f);
      out.beep_ms = 40;
      scooped = true;
    }
    for (auto &f : S.fish)  // tapping a dead fish scoops it out
      if (!scooped && f.state == DEAD && fabsf(f.x - in.tx) < 12 && fabsf(f.y - in.ty) < 10) f.state = GONE, scooped = true;
    if (!scooped) S.ripple_x = in.tx, S.ripple_y = in.ty, S.ripple_t0 = now;
    for (auto &f : S.fish) {  // tap on the glass: nearby fish dart away
      if (scooped || f.state != ALIVE) continue;
      float dx = f.x - in.tx, dy = f.y - in.ty, d = sqrtf(dx * dx + dy * dy) + 0.01f;
      if (d > 40) continue;
      f.tx = f.x + dx / d * 45, f.ty = f.y + dy / d * 45;
      f.startle_until = now + 700;
      f.retarget = now + 1500;
    }
  }

  for (auto &fl : S.flakes) {
    if (!fl.live) continue;
    if (fl.y < sand_y[(int)fl.x] - 1) fl.y += 7 * dt, fl.x += sinf(t * 2 + fl.t0) * 3 * dt;
    else if (t - fl.t0 > 30) fl.live = false;  // rots on the sand
    if (fl.x < 2) fl.x = 2;
    if (fl.x > CW - 3) fl.x = CW - 3;
  }

  for (auto &f : S.fish) {
    if (f.state == GONE) continue;
    if (f.state == DEAD) {  // belly up, drifting to the surface
      f.y += (SURFACE + 7 - f.y) * fminf(1, dt * 0.4f);
      f.x = fminf(CW - 10, fmaxf(10, f.x + sinf(t * 0.4f + f.phase) * 2 * dt));
      continue;
    }
    const Species &sp = SPECIES[f.kind];
    if ((f.hunger += dt * HUNGER_PER_S) >= 1) {
      f.hunger = 1, f.state = DEAD;
      out.beep_ms = 300;
      continue;
    }
    Flake *food = nullptr;
    if (f.hunger > 0.2f) {
      float best = 1e9;
      for (auto &fl : S.flakes)
        if (fl.live && fl.y > SURFACE) {
          float d = (fl.x - f.x) * (fl.x - f.x) + (fl.y - f.y) * (fl.y - f.y);
          if (d < best) best = d, food = &fl;
        }
    }
    bool startled = now < f.startle_until;
    if (food && !startled) f.tx = food->x, f.ty = food->y;
    else if (now > f.retarget || fabsf(f.tx - f.x) + fabsf(f.ty - f.y) < 4) new_target(f, now);
    float speed = sp.speed * (startled ? 4 : food ? 1.8f : 1) * (1 - f.hunger * 0.4f);
    float dx = f.tx - f.x, dy = f.ty - f.y, d = sqrtf(dx * dx + dy * dy) + 0.01f;
    float k = fminf(1, dt * (startled ? 6 : 2.5f));
    f.vx += (dx / d * speed - f.vx) * k;
    f.vy += (dy / d * speed * 0.6f - f.vy) * k;
    f.x = fminf(CW - 8, fmaxf(8, f.x + f.vx * dt));
    f.y = fminf(SAND - 6, fmaxf(SURFACE + 8, f.y + f.vy * dt));
    if (f.vx > 1.5f) f.dir = 1;
    if (f.vx < -1.5f) f.dir = -1;
    if (food && fabsf(food->x - (f.x + f.dir * sp.rx)) < 3 && fabsf(food->y - f.y) < 3) {
      food->live = false;
      f.hunger = fmaxf(0, f.hunger - 0.15f);
      f.sparkle_until = now + 350;
      if (!out.beep_ms) out.beep_ms = 6;
    }
  }

  if (now > S.next_bubble) {
    for (auto &b : S.bubbles)
      if (!b.live) { b = {24 + rnd() * 3, 132, 1 + rnd() * 1.6f, rnd() * 6.28f, true}; break; }
    S.next_bubble = now + 250 + (uint32_t)(rnd() * 600);
  }
  for (auto &b : S.bubbles)
    if (b.live && (b.y -= (14 + b.r * 6) * dt) < SURFACE + 1) b.live = false;
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

static uint16_t water_at(int x, int y) {
  float d = (y - SURFACE) / (float)(SAND - SURFACE);
  uint16_t c = mix(rgb(120, 205, 228), rgb(22, 72, 132), d);
  int band = (x + y / 2) % 46;  // static light rays from the surface
  if (band < 9 && d < 0.75f) c = mix(c, WHITE, 0.09f * (1 - d / 0.75f));
  return c;
}

static void draw_background(float t) {
  for (int y = 0; y < CH; y++)
    for (int x = 0; x < CW; x++) {
      uint16_t c;
      float surf = SURFACE + sinf(t * 1.8f + x * 0.18f) * 1.2f;
      if (y < surf - 1) c = AIR;
      else if (y < surf + 0.5f) c = rgb(190, 235, 245);
      else if (y >= sand_y[x]) {
        uint32_t h = (x * 73856093u) ^ (y * 19349663u);
        c = (h % 11 == 0) ? rgb(190, 165, 115) : (h % 17 == 0) ? rgb(240, 225, 185) : rgb(222, 200, 150);
      } else c = water_at(x, y);
      C[y * CW + x] = c;
    }
  // rocks with the bubbler on top
  ell(24, 141, 15, 9, OUTLINE);
  ell(24, 141, 14, 8, rgb(120, 125, 135));
  ell(20, 137, 7, 3, rgb(150, 155, 165));
  ell(44, 146, 8, 5, OUTLINE);
  ell(44, 146, 7, 4, rgb(105, 110, 120));
}

static void draw_seaweed(float t) {
  static const float X[] = {80, 88, 97, 107}, H[] = {58, 72, 50, 64}, P[] = {0, 1.7f, 3.1f, 4.4f};
  for (int s = 0; s < 4; s++)
    for (int i = 0; i < (int)H[s]; i++) {
      float y = sand_y[(int)X[s]] - i, f = i / H[s];
      int x = (int)(X[s] + sinf(t * 1.2f + i * 0.08f + P[s]) * f * 4);
      uint16_t c = mix(rgb(40, 140, 70), rgb(110, 200, 100), f);
      px(x - 1, (int)y, mix(c, OUTLINE, 0.5f));
      px(x, (int)y, c);
      px(x + 1, (int)y, c);
      px(x + 2, (int)y, mix(c, OUTLINE, 0.5f));
      if (i % 9 == 4) px(x + 3 * (s % 2 ? 1 : -1) + (s % 2 ? 0 : 0), (int)y, c);  // leaf nub
    }
}

static void draw_fish(const Fish &f, uint32_t now) {
  const Species &sp = SPECIES[f.kind];
  bool dead = f.state == DEAD;
  int up = dead ? -1 : 1;  // dead fish float belly up
  float hungry = dead ? 0.85f : fmaxf(0, f.hunger - 0.5f) * 0.8f;  // fades towards grey when starving
  uint16_t grey = rgb(130, 130, 140);
  uint16_t body = mix(sp.body, grey, hungry), belly = mix(sp.belly, grey, hungry), tail = mix(sp.tail, grey, hungry);
  uint16_t edge = mix(body, OUTLINE, 0.6f);
  int d = f.dir;
  float cx = f.x, cy = f.y, rx = sp.rx, ry = sp.ry;
  float wig = dead ? 0 : sinf(now * 0.012f * (1 + fabsf(f.vx) * 0.05f) + f.phase);

  for (int i = 0; i < (int)(rx * 0.7f) + 2; i++) {  // tail fan
    float x = cx - d * (rx + i - 1), half = 1 + i * ry * 0.22f, off = wig * i * 0.35f;
    for (int y = (int)(cy - half + off); y <= (int)(cy + half + off); y++)
      px((int)x, y, (y == (int)(cy - half + off) || y == (int)(cy + half + off)) ? edge : tail);
  }
  for (int k = -2; k <= 2; k++) {  // dorsal fin
    int h = 2 - (k < 0 ? -k : k) / 2;
    for (int j = 1; j <= h; j++) px((int)(cx - d * k), (int)(cy - up * (ry + j - 1)), j == h ? edge : tail);
  }
  ell(cx, cy, rx + 0.8f, ry + 0.8f, edge);
  ell(cx, cy, rx, ry, body);
  for (int y = (int)(cy + (dead ? -ry : ry * 0.25f)); y <= (int)(cy + (dead ? -ry * 0.25f : ry)); y++)
    for (int x = (int)(cx - rx); x <= (int)(cx + rx); x++)
      if (in_ell(x, y, cx, cy, rx, ry)) px(x, y, belly);

  if (sp.pattern == CLOWN)
    for (float bx : {cx + d * rx * 0.45f, cx - d * rx * 0.2f})
      for (int y = (int)(cy - ry); y <= (int)(cy + ry); y++)
        for (int x = (int)bx - 1; x <= (int)bx + 1; x++)
          if (in_ell(x, y, cx, cy, rx, ry)) px(x, y, x == (int)bx ? WHITE : OUTLINE);
  if (sp.pattern == TANG)
    for (int i = -4; i <= 4; i++) px((int)(cx - d * i), (int)(cy - up + up * (i * i) / 8), OUTLINE);
  if (sp.pattern == NEON)
    for (int x = (int)(cx - rx); x <= (int)(cx + rx); x++)
      if (in_ell(x, cy - up, cx, cy, rx, ry)) px(x, (int)(cy - up), dead ? grey : rgb(120, 240, 255));

  int ex = (int)(cx + d * rx * 0.55f), ey = (int)(cy - up * ry * 0.3f);
  if (dead) {
    for (int k = -1; k <= 1; k++) px(ex + k, ey + k, OUTLINE), px(ex + k, ey - k, OUTLINE);
  } else {
    px(ex, ey, WHITE), px(ex + d, ey, OUTLINE);
    if (sp.ry > 3) px(ex, ey + 1, WHITE), px(ex + d, ey + 1, OUTLINE);
  }

  if (now < f.sparkle_until) {
    int sx = (int)(cx + d * (rx + 2)), sy = (int)(cy - ry - 2);
    px(sx, sy, WHITE), px(sx - 1, sy, BUBBLE), px(sx + 1, sy, BUBBLE), px(sx, sy - 1, BUBBLE), px(sx, sy + 1, BUBBLE);
  }
}

static void draw_hud(uint32_t now) {
  float hunger = 0;
  int alive = 0;
  for (auto &f : S.fish)
    if (f.state == ALIVE) hunger += f.hunger, alive++;
  int food = alive ? (int)((1 - hunger / alive) * 44) : 0;
  for (int x = 0; x < 44; x++)
    for (int y = 0; y < 3; y++) px(6 + x, 5 + y, x < food ? FOOD_BAR : BAR_BG);
  int bars = S.sig >= APPROACH_MIN + 7 ? 4 : S.sig >= APPROACH_MIN ? 3 : S.sig >= -65 ? 2 : S.sig >= -80 ? 1 : 0;
  bool hot = now - S.last_approach < 1500;
  for (int i = 0; i < 4; i++)
    for (int h = 0; h <= i * 2 + 1; h++)
      for (int w = 0; w < 2; w++)
        px(100 + i * 4 + w, 10 - h, i < bars ? (hot ? rgb(230, 40, 80) : rgb(60, 70, 80)) : rgb(190, 200, 205));
}

// Signal graph, oldest left: blue = strongest device, grey = its baseline, red dots = APPROACH_MIN, pink column = approach.
static void draw_graph() {
  const int X0 = 4, Y0 = 20, GH = 40, TOP = -30, RANGE = 55;
  auto y_of = [&](int dbm) { int y = Y0 + (TOP - dbm) * (GH - 1) / RANGE; return y < Y0 ? Y0 : y > Y0 + GH - 1 ? Y0 + GH - 1 : y; };
  for (int y = Y0 - 1; y <= Y0 + GH; y++)
    for (int x = X0 - 1; x <= X0 + HIST; x++)
      if (y == Y0 - 1 || y == Y0 + GH || x == X0 - 1 || x == X0 + HIST) px(x, y, rgb(200, 225, 235));
      else px(x, y, mix(C[y * CW + x], WHITE, 0.45f));
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

void pet_draw(uint16_t *canvas, uint32_t now) {
  C = canvas;
  float t = now / 1000.0f;
  draw_background(t);
  if (S.debug) draw_graph();
  draw_seaweed(t);

  float rt = (now - S.ripple_t0) / 1000.0f;
  if (S.ripple_t0 && rt < 0.5f) {
    float r = 4 + rt * 50;
    for (int a = 0; a < 64; a++)
      px((int)(S.ripple_x + cosf(a * 0.098f) * r), (int)(S.ripple_y + sinf(a * 0.098f) * r), BUBBLE);
  }
  for (auto &fl : S.flakes)
    if (fl.live) px((int)fl.x, (int)fl.y, fl.c), px((int)fl.x + 1, (int)fl.y, fl.c), px((int)fl.x, (int)fl.y + 1, mix(fl.c, OUTLINE, 0.3f));
  for (auto &f : S.fish)
    if (f.state != GONE) draw_fish(f, now);
  for (auto &b : S.bubbles) {
    if (!b.live) continue;
    float x = b.x + sinf(t * 3 + b.phase) * 1.5f;
    if (b.r < 1.5f) px((int)x, (int)b.y, BUBBLE);
    else {
      for (int a = 0; a < 12; a++) px((int)(x + cosf(a * 0.524f) * b.r), (int)(b.y + sinf(a * 0.524f) * b.r), BUBBLE);
      px((int)(x - 0.5f), (int)(b.y - 0.5f), WHITE);
    }
  }
  draw_hud(now);
}
