#pragma once

// ════════════════════════════════════════════════════════════════════════
// Minimal LCD + LVGL driver using the ESP-IDF SPI master driver
// ════════════════════════════════════════════════════════════════════════
//
// Waveshare ESP32-C6-LCD-1.47 — ST7789, 172x320
//
// For use when W5500 SPI Ethernet conflicts with SPI LCD...
//
// This file replaces TFT_eSPI / M5GFX with a minimal LCD driver that
// sends every SPI transaction through the ESP-IDF SPI driver.

#include <Arduino.h>
#include <lvgl.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>

// ════════════════════════════════════════════════════════════════════════
// Waveshare ESP32-C6-LCD-1.47 pin and panel configuration
// ════════════════════════════════════════════════════════════════════════
//
// ST7789 172x320, 1.47" IPS
// Reference: Waveshare ESP32-C6-LCD-1.47-Demo

#define LCD_ST7789
#define LCD_WIDTH   172
#define LCD_HEIGHT  320
#define LCD_CS      14
#define LCD_DC      15
#define LCD_RST     21
#define LCD_BL      22
#define LCD_MOSI     6
#define LCD_MISO     5
#define LCD_SCLK     7
#define LCD_SPI_HZ  80000000
#define LCD_MADCTL   0x00        // Portrait, RGB
#define LCD_COL_OFS  34
#define LCD_ROW_OFS  0
#define LCD_INVERT   true

// ════════════════════════════════════════════════════════════════════════
// Rotation table
// ════════════════════════════════════════════════════════════════════════
//
// The ST7789 controller has a 320x320 internal framebuffer.  The 172x320
// visible area sits at column offset 34.  When rotating, the offsets swap
// accordingly.
//
// MADCTL bits: MY=0x80, MX=0x40, MV=0x20, ML=0x10, RGB=0x00, BGR=0x08

struct LcdRotation {
    uint8_t madctl;
    int     width;
    int     height;
    int     col_ofs;
    int     row_ofs;
};

static const LcdRotation _lcd_rotations[] = {
    { 0x00, 172, 320, 34,  0 },  //   0° — portrait (default)
    { 0x60, 320, 172,  0, 34 },  //  90° — landscape
    { 0xC0, 172, 320, 34,  0 },  // 180° — portrait flipped
    { 0xA0, 320, 172,  0, 34 },  // 270° — landscape flipped
};

// ════════════════════════════════════════════════════════════════════════
// lv_setup_class — LCD + LVGL initialization and runtime
// ════════════════════════════════════════════════════════════════════════

class lv_setup_class {
public:
    int width()  const { return _w; }
    int height() const { return _h; }

    /// Call once from setup().  Initializes the LCD hardware, powers on
    /// the backlight, and sets up LVGL with a display driver.
    /// @param rotation  Display rotation in degrees: 0, 90, 180, or 270.
    void begin(int rotation = 0) {
        // ── Resolve rotation parameters ─────────────────────────────
        int idx = 0;
        switch (rotation) {
            case  90: idx = 1; break;
            case 180: idx = 2; break;
            case 270: idx = 3; break;
            default:  idx = 0; break;
        }
        const auto &rot = _lcd_rotations[idx];
        _w       = rot.width;
        _h       = rot.height;
        _madctl  = rot.madctl;
        _col_ofs = rot.col_ofs;
        _row_ofs = rot.row_ofs;

        // ── GPIO setup ───────────────────────────────────────────────
        gpio_set_direction((gpio_num_t)LCD_DC, GPIO_MODE_OUTPUT);
        gpio_set_direction((gpio_num_t)LCD_CS, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)LCD_CS, 1);          // deselect LCD

        // Backlight via PWM (ledcAttach on ESP32-C6 Arduino core)
        ledcAttach(LCD_BL, 1000, 10);   // 1 kHz, 10-bit resolution
        ledcWrite(LCD_BL, 1023);         // full brightness

        // ── Hardware reset ───────────────────────────────────────────
        gpio_set_direction((gpio_num_t)LCD_RST, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)LCD_RST, 1);
        delay(10);
        gpio_set_level((gpio_num_t)LCD_RST, 0);
        delay(20);
        gpio_set_level((gpio_num_t)LCD_RST, 1);
        delay(150);

        // ── SPI bus initialization ───────────────────────────────────
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = LCD_MOSI;
        buscfg.miso_io_num = LCD_MISO;
        buscfg.sclk_io_num = LCD_SCLK;
        buscfg.quadwp_io_num = -1;
        buscfg.quadhd_io_num = -1;
        buscfg.max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * 2;

        esp_err_t err = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            Serial.printf("lv_setup: spi_bus_initialize failed: 0x%x\n", err);
            return;
        }

        spi_device_interface_config_t devcfg = {};
        devcfg.clock_speed_hz = LCD_SPI_HZ;
        devcfg.mode = 0;
        devcfg.spics_io_num = -1;       // manual CS
        devcfg.queue_size = 1;
        devcfg.flags = SPI_DEVICE_NO_DUMMY;

        err = spi_bus_add_device(SPI2_HOST, &devcfg, &_lcd_spi);
        if (err != ESP_OK) {
            Serial.printf("lv_setup: spi_bus_add_device failed: 0x%x\n", err);
            _lcd_spi = nullptr;
            return;
        }

        // ── LCD controller initialization ────────────────────────────
        _lcd_init_panel();

        // ── LVGL setup ───────────────────────────────────────────────
        lv_init();

        static lv_display_t *disp = lv_display_create(_w, _h);
        lv_display_set_flush_cb(disp, _disp_flush);
        lv_display_set_user_data(disp, this);

        static lv_color_t *buf = (lv_color_t *)heap_caps_malloc(
            _w * 10 * sizeof(lv_color_t), MALLOC_CAP_DMA);
        lv_display_set_buffers(disp, buf, NULL,
            _w * 10 * sizeof(lv_color_t), LV_DISPLAY_RENDER_MODE_PARTIAL);

        lv_tick_set_cb(_tick_get);
    }

private:
    spi_device_handle_t _lcd_spi = nullptr;
    int     _w       = LCD_WIDTH;
    int     _h       = LCD_HEIGHT;
    uint8_t _madctl  = LCD_MADCTL;
    int     _col_ofs = LCD_COL_OFS;
    int     _row_ofs = LCD_ROW_OFS;

    // ── Low-level SPI helpers ──────────────────────────────────────

    void _lcd_cmd(uint8_t cmd) {
        if (!_lcd_spi) return;
        gpio_set_level((gpio_num_t)LCD_DC, 0);
        gpio_set_level((gpio_num_t)LCD_CS, 0);
        spi_transaction_t t = {};
        t.length = 8;
        t.tx_data[0] = cmd;
        t.flags = SPI_TRANS_USE_TXDATA;
        spi_device_polling_transmit(_lcd_spi, &t);
        gpio_set_level((gpio_num_t)LCD_CS, 1);
    }

    void _lcd_data(const uint8_t *data, size_t len) {
        if (!_lcd_spi || len == 0) return;
        gpio_set_level((gpio_num_t)LCD_DC, 1);
        gpio_set_level((gpio_num_t)LCD_CS, 0);
        spi_transaction_t t = {};
        t.length = len * 8;
        t.tx_buffer = data;
        spi_device_polling_transmit(_lcd_spi, &t);
        gpio_set_level((gpio_num_t)LCD_CS, 1);
    }

    void _lcd_data8(uint8_t val) {
        _lcd_data(&val, 1);
    }

    void _lcd_set_addr_window(int x0, int y0, int x1, int y1) {
        x0 += _col_ofs;  x1 += _col_ofs;
        y0 += _row_ofs;  y1 += _row_ofs;

        uint8_t col[4] = {
            (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF),
            (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF)
        };
        _lcd_cmd(0x2A);
        _lcd_data(col, 4);

        uint8_t row[4] = {
            (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF),
            (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF)
        };
        _lcd_cmd(0x2B);
        _lcd_data(row, 4);

        _lcd_cmd(0x2C);
    }

    void _lcd_push_pixels(const uint8_t *data, size_t bytes) {
        if (!_lcd_spi || bytes == 0) return;
        gpio_set_level((gpio_num_t)LCD_DC, 1);
        gpio_set_level((gpio_num_t)LCD_CS, 0);

        const size_t max_chunk = 32768;
        while (bytes > 0) {
            size_t chunk = (bytes > max_chunk) ? max_chunk : bytes;
            spi_transaction_t t = {};
            t.length = chunk * 8;
            t.tx_buffer = data;
            spi_device_polling_transmit(_lcd_spi, &t);
            data += chunk;
            bytes -= chunk;
        }

        gpio_set_level((gpio_num_t)LCD_CS, 1);
    }

    // ── LCD panel initialization (MIPI DCS) ──────────────────────────

    void _lcd_init_panel() {
        _lcd_cmd(0x01);               // SWRESET
        delay(150);

        _lcd_cmd(0x11);               // SLPOUT
        delay(255);

        _lcd_cmd(0x3A);               // COLMOD — 16-bit RGB565
        _lcd_data8(0x55);

        _lcd_cmd(0x36);               // MADCTL
        _lcd_data8(_madctl);

        _lcd_cmd(0x21);               // INVON — Display Inversion ON

        _lcd_cmd(0x13);               // NORON
        delay(10);

        _lcd_cmd(0x29);               // DISPON
        delay(10);
    }

    // ── LVGL callbacks ───────────────────────────────────────────────

    static uint32_t _tick_get(void) { return millis(); }

    static void _disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *pixelmap) {
        auto *self = (lv_setup_class *)lv_display_get_user_data(disp);

        if (!self->_lcd_spi) {
            lv_disp_flush_ready(disp);
            return;
        }

        int w = (area->x2 - area->x1 + 1);
        int h = (area->y2 - area->y1 + 1);
        size_t pixel_count = w * h;

        uint16_t *px = (uint16_t *)pixelmap;
        for (size_t i = 0; i < pixel_count; i++) {
            px[i] = __builtin_bswap16(px[i]);
        }

        spi_device_acquire_bus(self->_lcd_spi, portMAX_DELAY);

        self->_lcd_set_addr_window(area->x1, area->y1, area->x2, area->y2);
        self->_lcd_push_pixels(pixelmap, pixel_count * 2);

        spi_device_release_bus(self->_lcd_spi);

        lv_disp_flush_ready(disp);
    }
};

static lv_setup_class lv_setup;
