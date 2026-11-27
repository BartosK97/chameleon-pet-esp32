// Waveshare ESP32-S3-Touch-LCD-2.8B: ST7701 RGB panel, GT911 touch, TCA9554 expander (LCD/touch reset, buzzer).
#pragma once
#include <Arduino.h>

constexpr int LCD_W = 480, LCD_H = 640;

bool board_init();
uint16_t *board_fb();  // RGB565, LCD_W x LCD_H, scanned out continuously
void board_wait_vsync();  // write right after this to stay ahead of the scanline (no tearing)
bool board_touch(int &x, int &y);
bool board_accel(float &x, float &y, float &z);  // QMI8658 raw chip axes, g
void board_buzzer(bool on);
void board_backlight(uint8_t pct);
