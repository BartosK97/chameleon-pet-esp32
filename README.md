# ESP32 Pocket Aquarium

[![build](https://github.com/BartosK97/chameleon-pet-esp32/actions/workflows/build.yml/badge.svg)](https://github.com/BartosK97/chameleon-pet-esp32/actions/workflows/build.yml)
[![license: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![ESP32-S3](https://img.shields.io/badge/ESP32--S3-Arduino%20core%204.0-e7352c)

A pixel-art aquarium on a 2.8" touch display. To feed the fish, you walk up to it with your phone. If nobody comes by for 24 hours, the fish die.

<p align="center"><img src="docs/aquarium.png" width="240" alt="Aquarium animation: fish swim, a phone comes close, food rains down, fish eat"></p>

![Idle, tap on the glass, phone brought close, fish eating, 24 h without food](docs/preview.png)

## What it does

- **Feeding by walking up.** Bring any phone close and food flakes rain down from the surface. No app, no pairing, and any phone can do it.
- **Fish with needs.** Hungry fish chase the flakes and turn pale when they starve. A fish dies after 24 hours without food. Dead fish turn grey, roll belly up and float to the surface.
- **Touch the glass.** A tap sends a ripple across the water and nearby fish dart away. Tap a dead fish to scoop it out. Tap the empty tank to stock five new fish.
- **Remembers everything.** Fish state is saved to flash. After a power cut, the time spent powered off is caught up via NTP, so pulling the plug doesn't help.
- **Live signal graph.** Shows what the Bluetooth sensing sees. Tap the signal icon to hide or show it.
- **Wireless.** Over-the-air updates and a network log, so once it's flashed it only needs power.

## How "walking up" is detected

The ESP32 scans Bluetooth LE adverts. Phones send them constantly, iPhones especially. The firmware does not look at absolute signal strength. It tracks every device on its own:

```mermaid
flowchart LR
  A[BLE advert] --> B[per-device RSSI<br/>fast average]
  B --> C[slow ~4 s baseline]
  B --> D{fast ≥ −52 dBm<br/>and ≥ 8 dB above<br/>its own baseline?}
  C --> D
  D -- yes --> E[food drops<br/>baseline reset]
```

A laptop lying next to the tank has a strong signal, but its baseline follows it, so it never feeds. A phone that moves closer jumps 10–20 dB above its own baseline and does. In the graph, blue is the strongest device, grey is its baseline, red dots mark the minimum level and a pink column marks each detected approach.

## Hardware

| Part | Notes |
|---|---|
| [Waveshare ESP32-S3-Touch-LCD-2.8B](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.8B) | ESP32-S3, 16 MB flash, 8 MB PSRAM, 480×640 ST7701 RGB panel, GT911 touch, buzzer, IMU, RTC, LiPo charger |
| USB-C cable | Data-capable, for the first flash |
| USB power supply or 3.7 V LiPo (MX1.25) | Everything after the first flash runs over WiFi |

No wiring and no soldering needed.

## Quick start

```bash
brew install arduino-cli   # or see arduino.github.io/arduino-cli
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index && arduino-cli core install esp32:esp32@4.0.0
```

```bash
cp firmware/mini_pet/secrets.example.h firmware/mini_pet/secrets.h   # your 2.4 GHz WiFi, optional
FQBN=esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc
arduino-cli compile -b $FQBN --output-dir build firmware/mini_pet
arduino-cli upload -p /dev/cu.usbmodem101 -b $FQBN firmware/mini_pet
```

### Over the air

Once it's on WiFi, the board shows up as `mini-pet.local`:

```bash
python3 ~/Library/Arduino15/packages/esp32/hardware/esp32/4.0.0/tools/espota.py -i mini-pet.local -f build/mini_pet.ino.bin
nc mini-pet.local 23   # live log: signal, approaches, feedings
```

Without `secrets.h` the firmware runs offline. State is still saved, but without a wall clock, time spent powered off isn't counted.

## Preview without hardware

All game logic and drawing live in `firmware/mini_pet/pet.cpp`, which has no Arduino dependencies. To render frames on any desktop:

```bash
c++ -std=c++17 -O2 tools/preview.cpp -o preview
./preview | python3 tools/apng.py sheet.png              # five key moments
./preview anim | python3 tools/apng.py anim.png 80        # 8 s animation
```

## Tuning

| Constant | Where | Default |
|---|---|---|
| `APPROACH_MIN`, `APPROACH_RISE` | `pet.h` | −52 dBm, +8 dB |
| `FEED_COOLDOWN` | `pet.cpp` | 8 s |
| `HUNGER_PER_S` | `pet.cpp` | full to dead in 24 h |

## Notes for ESP32-S3 RGB panels

The RGB panel reads its frame buffer out of PSRAM continuously. With WiFi and BLE running, this board glitched until these changes:

- **Draw small, write little.** The game draws a 120×160 canvas in internal RAM. Only canvas pixels that changed are written into the 480×640 frame buffer, each as a 4×4 block.
- **Lower pixel clock.** Waveshare's demo uses 30 MHz. 18 MHz still gives about 42 Hz refresh and leaves PSRAM bandwidth for everything else.
- **Write after vsync.** Changes are written right after the panel's vsync, so they stay ahead of the scanline.
- **Bounce buffers** of 20 lines, 24 fps, and `WiFi.persistent(false)` so the WiFi stack never writes to flash.
- **GT911 touch:** hold INT low during reset, or its I2C address comes up as 0x5D or 0x14 at random.

## Layout

| Path | What |
|---|---|
| `firmware/mini_pet/board.*` | Display, touch, IO expander, buzzer and backlight for the 2.8B |
| `firmware/mini_pet/pet.*` | Aquarium simulation and pixel-art renderer |
| `firmware/mini_pet/mini_pet.ino` | BLE approach detection, WiFi/OTA/NTP, persistence, frame loop |
| `tools/preview.cpp`, `tools/apng.py` | Desktop renderer for previews and the README animation |

## Ideas

- Shake the device (QMI8658 IMU) to stir up the sand
- Dim at night using the RTC
- More species, breeding, a snail that cleans up leftover flakes
- Use the spare ESP32-S3 devkit to drive a 64×64 HUB75 LED panel version

## Credits

The ST7701 init sequence, panel timings and pin map come from Waveshare's ESP32-S3-Touch-LCD-2.8B demo. Everything else is original.

## License

[MIT](LICENSE)
