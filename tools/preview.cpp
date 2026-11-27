// Renders pet frames on the Mac: c++ -std=c++17 -O2 tools/preview.cpp -o /tmp/preview && /tmp/preview > sheet.ppm
#include <cstdio>
#include <vector>
#include "../firmware/mini_pet/pet.cpp"

int main() {
  const int scale = 4;
  // frame times with what happens just before them: idle, petted, phone near (fly), tongue, eaten
  const uint32_t shots[] = {800, 1400, 3000, 3700, 4300};
  const int n = sizeof(shots) / sizeof(shots[0]), W = CW * scale + 4, H = CH * scale;
  std::vector<uint16_t> canvas(CW * CH);
  std::vector<uint8_t> img(W * n * H * 3, 255);
  pet_init(0);
  int shot = 0;
  for (uint32_t t = 0; t <= shots[n - 1]; t += 20) {
    int sig = t < 1900 ? -66 : -44;  // phone brought close at 1.9 s
    PetInput in = {t, t == 1100, 28, 46, t % 240 == 0, sig, t < 2000 ? -66 : -60, t == 2000};
    PetOutput out;
    pet_update(in, out);
    if (t < shots[shot]) continue;
    pet_draw(canvas.data(), t);
    for (int y = 0; y < H; y++)
      for (int x = 0; x < CW * scale; x++) {
        uint16_t c = canvas[(y / scale) * CW + x / scale];
        uint8_t *p = &img[(y * W * n + shot * W + x) * 3];
        p[0] = (c >> 11) << 3, p[1] = ((c >> 5) & 63) << 2, p[2] = (c & 31) << 3;
      }
    shot++;
  }
  printf("P6 %d %d 255\n", W * n, H);
  fwrite(img.data(), 1, img.size(), stdout);
}
