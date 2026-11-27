#include "board.h"
#include <Wire.h>
#include "driver/spi_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"

// Pins and ST7701 init table from Waveshare's 2.8B Arduino demo.
static const int I2C_SDA = 15, I2C_SCL = 7, BL_PIN = 6, INIT_SDA = 1, INIT_SCL = 2, TOUCH_INT = 16;
static const uint8_t EXP_ADDR = 0x20;
static uint8_t gt911_addr = 0x5D;
enum { EXP_LCD_RST = 0, EXP_TP_RST = 1, EXP_LCD_CS = 2, EXP_BUZZER = 7 };  // bit = EXIOn - 1

static uint8_t exp_out = 0x7F;  // power-on default 0xFF would leave buzzer on
static spi_device_handle_t spi;
static esp_lcd_panel_handle_t panel;
static uint16_t *fb;
static SemaphoreHandle_t vsync_sem;

static bool IRAM_ATTR on_vsync(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t *, void *) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(vsync_sem, &woken);
  return woken == pdTRUE;
}

static void exp_write(uint8_t reg, uint8_t v) {
  Wire.beginTransmission(EXP_ADDR);
  Wire.write(reg);
  Wire.write(v);
  Wire.endTransmission();
}

static void exp_set(int bit, bool high) {
  exp_out = high ? exp_out | (1 << bit) : exp_out & ~(1 << bit);
  exp_write(1, exp_out);
}

// 9-bit SPI: 1 D/C bit then 8 bits
static void st_send(bool data, uint8_t v) {
  spi_transaction_t t = {};
  t.cmd = data;
  t.addr = v;
  spi_device_transmit(spi, &t);
}

struct InitCmd { uint8_t cmd, n; uint8_t d[16]; uint16_t ms; };
static const InitCmd ST7701_INIT[] = {
  {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x13}},
  {0xEF, 1, {0x08}},
  {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x10}},
  {0xC0, 2, {0x4F, 0x00}},
  {0xC1, 2, {0x10, 0x02}},
  {0xC2, 2, {0x07, 0x02}},
  {0xCC, 1, {0x10}},
  {0xB0, 16, {0x00, 0x10, 0x17, 0x0D, 0x11, 0x06, 0x05, 0x08, 0x07, 0x1F, 0x04, 0x11, 0x0E, 0x29, 0x30, 0x1F}},
  {0xB1, 16, {0x00, 0x0D, 0x14, 0x0E, 0x11, 0x06, 0x04, 0x08, 0x08, 0x20, 0x05, 0x13, 0x13, 0x26, 0x30, 0x1F}},
  {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x11}},
  {0xB0, 1, {0x65}}, {0xB1, 1, {0x71}}, {0xB2, 1, {0x82}}, {0xB3, 1, {0x80}},
  {0xB5, 1, {0x42}}, {0xB7, 1, {0x85}}, {0xB8, 1, {0x20}}, {0xC0, 1, {0x09}},
  {0xC1, 1, {0x78}}, {0xC2, 1, {0x78}}, {0xD0, 1, {0x88}}, {0xEE, 1, {0x42}},
  {0xE0, 3, {0x00, 0x00, 0x02}},
  {0xE1, 11, {0x04, 0xA0, 0x06, 0xA0, 0x05, 0xA0, 0x07, 0xA0, 0x00, 0x44, 0x44}},
  {0xE2, 12, {}},
  {0xE3, 4, {0x00, 0x00, 0x22, 0x22}},
  {0xE4, 2, {0x44, 0x44}},
  {0xE5, 16, {0x0C, 0x90, 0xA0, 0xA0, 0x0E, 0x92, 0xA0, 0xA0, 0x08, 0x8C, 0xA0, 0xA0, 0x0A, 0x8E, 0xA0, 0xA0}},
  {0xE6, 4, {0x00, 0x00, 0x22, 0x22}},
  {0xE7, 2, {0x44, 0x44}},
  {0xE8, 16, {0x0D, 0x91, 0xA0, 0xA0, 0x0F, 0x93, 0xA0, 0xA0, 0x09, 0x8D, 0xA0, 0xA0, 0x0B, 0x8F, 0xA0, 0xA0}},
  {0xEB, 7, {0x00, 0x00, 0xE4, 0xE4, 0x44, 0x00, 0x40}},
  {0xED, 16, {0xFF, 0xF5, 0x47, 0x6F, 0x0B, 0xA1, 0xAB, 0xFF, 0xFF, 0xBA, 0x1A, 0xB0, 0xF6, 0x74, 0x5F, 0xFF}},
  {0xEF, 6, {0x08, 0x08, 0x08, 0x40, 0x3F, 0x64}},
  {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x00}},
  {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x13}},
  {0xE6, 2, {0x16, 0x7C}},
  {0xE8, 2, {0x00, 0x0E}},
  {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x00}},
  {0x11, 1, {0x00}, 200},
  {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x13}},
  {0xE8, 2, {0x00, 0x0C}, 150},
  {0xE8, 2, {0x00, 0x00}},
  {0xFF, 5, {0x77, 0x01, 0x00, 0x00, 0x00}},
  {0x29, 1, {0x00}},
  {0x35, 1, {0x00}},
  {0x11, 1, {0x00}, 200},
  {0x29, 1, {0x00}},
  {0x29, 1, {0x00}, 100},
};

static void st7701_init() {
  spi_bus_config_t bus = {};
  bus.mosi_io_num = INIT_SDA;
  bus.miso_io_num = -1;
  bus.sclk_io_num = INIT_SCL;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = 64;
  spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
  spi_device_interface_config_t dev = {};
  dev.command_bits = 1;
  dev.address_bits = 8;
  dev.mode = 0;
  dev.clock_speed_hz = 10 * 1000 * 1000;
  dev.spics_io_num = -1;  // CS sits on the expander
  dev.queue_size = 1;
  spi_bus_add_device(SPI2_HOST, &dev, &spi);

  exp_set(EXP_LCD_CS, false);
  delay(10);
  for (const auto &c : ST7701_INIT) {
    st_send(false, c.cmd);
    for (int i = 0; i < c.n; i++) st_send(true, c.d[i]);
    if (c.ms) delay(c.ms);
  }
  exp_set(EXP_LCD_CS, true);
}

static bool rgb_init() {
  static const int data[16] = {5, 45, 48, 47, 21, 14, 13, 12, 11, 10, 9, 46, 3, 8, 18, 17};
  esp_lcd_rgb_panel_config_t cfg = {};
  cfg.clk_src = LCD_CLK_SRC_PLL240M;
  cfg.timings.pclk_hz = 18 * 1000 * 1000;  // ~42 Hz refresh; Waveshare's 30 MHz eats the PSRAM bandwidth WiFi+BLE need
  cfg.timings.h_res = LCD_W;
  cfg.timings.v_res = LCD_H;
  cfg.timings.hsync_pulse_width = 10;
  cfg.timings.hsync_back_porch = 70;
  cfg.timings.hsync_front_porch = 60;
  cfg.timings.vsync_pulse_width = 10;
  cfg.timings.vsync_back_porch = 20;
  cfg.timings.vsync_front_porch = 20;
  cfg.data_width = 16;
  cfg.in_color_format = LCD_COLOR_FMT_RGB565;
  cfg.out_color_format = LCD_COLOR_FMT_RGB565;
  cfg.num_fbs = 1;
  cfg.bounce_buffer_size_px = 20 * LCD_W;  // without it the PSRAM frame drifts; frame must be an even multiple
  cfg.dma_burst_size = 64;
  cfg.hsync_gpio_num = GPIO_NUM_38;
  cfg.vsync_gpio_num = GPIO_NUM_39;
  cfg.de_gpio_num = GPIO_NUM_40;
  cfg.pclk_gpio_num = GPIO_NUM_41;
  cfg.disp_gpio_num = GPIO_NUM_NC;
  for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = (gpio_num_t)data[i];
  cfg.flags.fb_in_psram = 1;
  if (esp_lcd_new_rgb_panel(&cfg, &panel) != ESP_OK) return false;
  vsync_sem = xSemaphoreCreateBinary();
  esp_lcd_rgb_panel_event_callbacks_t cbs = {};
  cbs.on_vsync = on_vsync;
  esp_lcd_rgb_panel_register_event_callbacks(panel, &cbs, nullptr);
  esp_lcd_panel_reset(panel);
  esp_lcd_panel_init(panel);
  return esp_lcd_rgb_panel_get_frame_buffer(panel, 1, (void **)&fb) == ESP_OK;
}

static void gt911_reg(uint16_t reg) {
  Wire.beginTransmission(gt911_addr);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
}

bool board_init() {
  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  exp_write(3, 0x00);  // all expander pins output
  exp_write(1, exp_out);

  exp_set(EXP_LCD_RST, false);
  delay(10);
  exp_set(EXP_LCD_RST, true);
  delay(50);
  st7701_init();
  if (!rgb_init()) return false;

  // GT911 latches its I2C address from INT during reset: low selects 0x5D
  pinMode(TOUCH_INT, OUTPUT);
  digitalWrite(TOUCH_INT, LOW);
  exp_set(EXP_TP_RST, false);
  delay(10);
  exp_set(EXP_TP_RST, true);
  delay(200);
  pinMode(TOUCH_INT, INPUT);
  for (uint8_t a : {0x5D, 0x14}) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { gt911_addr = a; break; }
  }

  ledcAttach(BL_PIN, 20000, 10);
  board_backlight(80);
  return true;
}

uint16_t *board_fb() { return fb; }

void board_wait_vsync() {
  xSemaphoreTake(vsync_sem, 0);  // drop a stale edge, wait for a fresh one
  xSemaphoreTake(vsync_sem, pdMS_TO_TICKS(50));
}

bool board_touch(int &x, int &y) {
  gt911_reg(0x814E);
  if (Wire.endTransmission() != 0 || Wire.requestFrom(gt911_addr, (uint8_t)7) != 7) return false;
  uint8_t b[7];
  for (auto &v : b) v = Wire.read();
  bool down = (b[0] & 0x80) && (b[0] & 0x0F);
  gt911_reg(0x814E);  // ack status so the next sample arrives
  Wire.write(0);
  Wire.endTransmission();
  if (!down) return false;
  x = b[2] | (b[3] << 8);
  y = b[4] | (b[5] << 8);
  return true;
}

void board_buzzer(bool on) { exp_set(EXP_BUZZER, on); }

void board_backlight(uint8_t pct) { ledcWrite(BL_PIN, pct >= 100 ? 1024 : pct * 10); }
