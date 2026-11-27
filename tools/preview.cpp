// Renders pet frames on a desktop: c++ -std=c++17 -O2 tools/preview.cpp -o preview
//   ./preview > sheet.ppm        key moments side by side
//   ./preview anim > anim.ppm    8 s at 12.5 fps as concatenated PPMs (tools/apng.py turns it into an APNG)
#include <cstdio>
#include <cstring>
#include <vector>
#include "../firmware/mini_pet/pet.cpp"

static void emit(const std::vector<uint16_t> &canvas, std::vector<uint8_t> &img, int W, int ox, int scale) {
  int cw, ch;
  pet_size(cw, ch);
  for (int y = 0; y < ch * scale; y++)
    for (int x = 0; x < cw * scale; x++) {
      uint16_t c = canvas[(y / scale) * cw + x / scale];
      uint8_t *p = &img[(y * W + ox + x) * 3];
      p[0] = (c >> 11) << 3, p[1] = ((c >> 5) & 63) << 2, p[2] = (c & 31) << 3;
    }
}

int main(int argc, char **argv) {
  const bool anim = argc > 1 && !strcmp(argv[1], "anim");
  const int scale = 2;
  // day, tap ripple, phone brought close (food), tilted + shaken, night (lamp off, fish asleep), offspring + snail, landscape
  const uint32_t shots[] = {800, 1300, 3000, 4700, 10000, 13000, 16000};
  const int n = anim ? 1 : sizeof(shots) / sizeof(shots[0]), W = (CH * scale + (anim ? 0 : 4)) * n, H = CH * scale;  // cells fit landscape too
  std::vector<uint16_t> canvas(CW * CH);
  std::vector<uint8_t> img(W * H * 3, 255);
  static uint16_t scene[CW * CH];
  pet_init(0, scene);
  int shot = 0;
  const uint32_t end = anim ? 8000 : shots[n - 1];
  for (uint32_t t = 0; t <= end; t += 40) {
    PetInput in = {};
    in.now = t;
    in.tap = t == 1120 || t == 10160, in.tx = t > 10000 ? 70 : 60, in.ty = t > 10000 ? 3 : 80;  // tap glass, later the lamp
    in.sample = t % 240 == 0, in.sig = t < 1900 ? -66 : -44, in.base = t < 2000 ? -66 : -60, in.approach = t == 2000;
    bool tilted = t >= 4000 && t < 6000;
    in.gx = tilted ? 0.3f : 0, in.gy = tilted ? 0.95f : 1;
    in.shake = t == 4400;
    in.minute = t >= 6000 && t < 10100 ? 23 * 60 : 12 * 60;
    in.day = 1;
    if (!anim && t == 10100) {  // jump to a tank that has bred
      PetSave s;
      pet_get_save(s);
      s.kind[5] = CLOWN, s.state[5] = ALIVE, s.size[5] = 0.5f, s.hunger[5] = 0.2f;
      s.kind[6] = NEON, s.state[6] = ALIVE, s.size[6] = 0.6f, s.hunger[6] = 0.2f;
      pet_load(s);
    }
    if (!anim && t == 13040) pet_set_landscape(true);
    if (t == 2600) pet_say(0, "Flossi: Endlich Futter! Ich dachte schon, ihr habt mich vergessen. \xc3\xa4\xc3\xb6\xc3\xbc\xc3\x9f", t);
    if (t == 9000) pet_say(1, "Grummel: Licht aus, endlich Ruhe.", t);
    if (t == 15000) pet_say(2, "Sunny: Quer ist das neue Hoch!", t);
    PetOutput out;
    pet_update(in, out);
    if (anim) {
      if (t % 80) continue;
      pet_draw(canvas.data(), t);
      emit(canvas, img, W, 0, scale);
      printf("P6 %d %d 255\n", W, H);
      fwrite(img.data(), 1, img.size(), stdout);
      continue;
    }
    if (t < shots[shot]) continue;
    pet_draw(canvas.data(), t);
    emit(canvas, img, W, shot * (CH * scale + 4), scale);
    shot++;
  }
  if (!anim) {
    printf("P6 %d %d 255\n", W, H);
    fwrite(img.data(), 1, img.size(), stdout);
  }
}
