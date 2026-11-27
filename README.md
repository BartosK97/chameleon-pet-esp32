# chameleon-pet-esp32

A pixel-art virtual pet for the Waveshare ESP32-S3 2.8" touch display. It's a chameleon on a branch.

![Idle, petted, phone nearby (fly arrives), tongue, fed](docs/preview.png)

- **Pet it:** tap the chameleon. It runs through the rainbow, a heart floats up, and the joy bar fills.
- **Feed it:** bring any phone close to the device. The ESP32 tracks the Bluetooth LE signal strength of every device around it. When one of them rises well above its own recent baseline (+8 dB, ending above −52 dBm), a fly buzzes in and the chameleon catches it with its tongue. No app and no pairing needed, and any phone can feed it. A laptop or phone that just sits next to it never triggers, because only an approach counts.
- **Signal graph:** the panel above the chameleon shows the strongest device (blue), its baseline (grey), the minimum level (red dots) and every detected approach (pink column). Tap the signal icon in the top right to hide or show it.
- **Mood colour:** green when fed, fading to olive brown as it gets hungry.
- The turret eye follows taps and the fly, and otherwise looks around. The chameleon blinks now and then.

## Hardware

- Waveshare **ESP32-S3-Touch-LCD-2.8B**: ESP32-S3 with 16 MB flash and 8 MB PSRAM, ST7701 480×640 RGB panel, GT911 touch, TCA9554 IO expander, buzzer, QMI8658 IMU, PCF85063 RTC.
- A USB-C data cable.

## Build and flash

```bash
brew install arduino-cli
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index && arduino-cli core install esp32:esp32
```

```bash
FQBN=esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc
arduino-cli compile -b $FQBN firmware/mini_pet
arduino-cli upload -p /dev/cu.usbmodem101 -b $FQBN firmware/mini_pet
```

### WiFi, OTA and log

Copy `firmware/mini_pet/secrets.example.h` to `secrets.h` (gitignored) and enter your 2.4 GHz network. After the next USB flash, the board joins WiFi as `mini-pet.local`. It then only needs power, and updates go over the air:

```bash
arduino-cli compile -b $FQBN --output-dir build firmware/mini_pet
python3 ~/Library/Arduino15/packages/esp32/hardware/esp32/*/tools/espota.py -i mini-pet.local -f build/mini_pet.ino.bin
```

To read the log: `nc mini-pet.local 23`

Tested with arduino-esp32 core 4.0.0. LVGL is not used: the game draws a 60×80 canvas, scales it 8× and writes it into the panel's frame buffer. Only changed blocks are written, because full-frame PSRAM writes plus BLE scanning glitch the RGB panel.

## Preview on a computer

`firmware/mini_pet/pet.cpp` has no Arduino dependencies. `tools/preview.cpp` renders a strip of frames as a PPM:

```bash
c++ -std=c++17 -O2 tools/preview.cpp -o preview && ./preview > preview.ppm
```

## Tuning

The constants are at the top of `firmware/mini_pet/pet.cpp`:

- `APPROACH_MIN` / `APPROACH_RISE` (in `pet.h`): how strong a device must end up and how far it must rise above its baseline.
- `FEED_COOLDOWN`: minimum time between two feedings.
- `HUNGER_PER_MS`: how fast the chameleon gets hungry.

The log prints the strongest device and its baseline every second, plus `approach +N dB` when one fires.

## Files

| File | What |
|---|---|
| `firmware/mini_pet/board.*` | Display, touch, IO expander, buzzer and backlight for the 2.8B board |
| `firmware/mini_pet/pet.*` | Game state and pixel-art drawing |
| `firmware/mini_pet/mini_pet.ino` | Main loop: BLE scan, touch, frame push |
| `tools/preview.cpp` | Renders frames on a desktop for quick art iteration |

## Credits

The ST7701 init sequence, panel timings and pin map come from Waveshare's ESP32-S3-Touch-LCD-2.8B demo. The rest is original.

## License

MIT, see [LICENSE](LICENSE).
