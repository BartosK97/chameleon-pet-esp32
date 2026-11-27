// Renders pet frames on a desktop: c++ -std=c++17 -O2 tools/preview.cpp -o preview
//   ./preview > sheet.ppm        5 key moments side by side
//   ./preview anim > anim.ppm    8 s at 12.5 fps as concatenated PPMs (tools/apng.py turns it into an APNG)
#include <cstdio>
#include <cstring>
#include <vector>
#include "../firmware/mini_pet/pet.cpp"

static void emit(const std::vector<uint16_t> &canvas, std::vector<uint8_t> &img, int W, int ox, int scale) {
  for (int y = 0; y < CH * scale; y++)
    for (int x = 0; x < CW * scale; x++) {
      uint16_t c = canvas[(y / scale) * CW + x / scale];
      uint8_t *p = &img[(y * W + ox + x) * 3];
      p[0] = (c >> 11) << 3, p[1] = ((c >> 5) & 63) << 2, p[2] = (c & 31) << 3;
    }
}

int main(int argc, char **argv) {
  const bool anim = argc > 1 && !strcmp(argv[1], "anim");
  const int scale = 2;
  // idle, tap ripple, phone brought close (food drops), fish eating, 24 h later without food
  const uint32_t shots[] = {800, 1300, 3000, 7000, 14000};
  const int n = anim ? 1 : sizeof(shots) / sizeof(shots[0]), W = (CW * scale + (anim ? 0 : 4)) * n, H = CH * scale;
  std::vector<uint16_t> canvas(CW * CH);
  std::vector<uint8_t> img(W * H * 3, 255);
  pet_init(0);
  int shot = 0;
  const uint32_t end = anim ? 8000 : shots[n - 1];
  for (uint32_t t = 0; t <= end; t += 40) {
    int sig = t < 1900 ? -66 : -44;  // phone brought close at 1.9 s
    PetInput in = {t, t == 1120, 60, 80, t % 240 == 0, sig, t < 2000 ? -66 : -60, t == 2000};
    PetOutput out;
    if (!anim && t == 8000) pet_advance(86400);
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
    emit(canvas, img, W, shot * (CW * scale + 4), scale);
    shot++;
  }
  if (!anim) {
    printf("P6 %d %d 255\n", W, H);
    fwrite(img.data(), 1, img.size(), stdout);
  }
}
