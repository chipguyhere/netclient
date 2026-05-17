#pragma once

// ════════════════════════════════════════════════════════════════════════
// Minimal LCD + LVGL driver using the ESP-IDF SPI master driver
// ════════════════════════════════════════════════════════════════════════
//
// For use when W5500 SPI Ethernet conflicts with SPI LCD...
//
// M5GFX writes directly to the SPI peripheral registers for performance,
// bypassing the ESP-IDF SPI master driver.  The W5500 Ethernet controller
// (used by several M5Stack boards) talks through the ESP-IDF SPI driver.
// When two independent driver stacks try to use the same SPI peripheral
// from different tasks/threads with no synchronization, they corrupt each other.
//
// This file replaces TFT_eSPI / M5GFX with a minimal LCD driver that
// sends every SPI transaction through the ESP-IDF SPI driver.
//
// Supported LCD controllers:
//   - ILI9342  (M5Stack Core, Fire, Core2)  320x240
//   - ST7789   (M5Stack StamPLC)            240x135
// Both use the standard MIPI DCS command set for pixel writes (CASET,
// RASET, RAMWR), so the runtime code is identical — only the pin
// assignments and init parameters differ per board.

#include <Arduino.h>
#include <lvgl.h>
#include <Wire.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>

// ════════════════════════════════════════════════════════════════════════
// Board-specific pin and panel configuration
// ════════════════════════════════════════════════════════════════════════
//
// Each board section defines:
//   LCD_WIDTH / LCD_HEIGHT  — panel resolution after rotation
//   LCD_CS / DC / RST / BL  — control GPIOs (-1 = managed elsewhere)
//   LCD_MOSI / MISO / SCLK  — SPI data pins
//   LCD_SPI_HZ              — SPI clock frequency
//   LCD_MADCTL              — MIPI Memory Access Control byte (rotation + color order)
//   LCD_COL_OFS / ROW_OFS   — pixel address offsets (non-zero on small panels
//                              where the visible area is a window into a larger
//                              framebuffer inside the controller)
//   LCD_INVERT              — true if the panel needs display inversion ON

#if defined(ARDUINO_M5STACK_CORE) || defined(ARDUINO_M5STACK_FIRE)
  // M5Stack Core / Fire — ILI9342, 320x240
  // Backlight on GPIO 32 (directly driven)
  #define LCD_ILI9342
  #define LCD_WIDTH   320
  #define LCD_HEIGHT  240
  #define LCD_CS      14
  #define LCD_DC      27
  #define LCD_RST     33
  #define LCD_BL      32
  #define LCD_MOSI    23
  #define LCD_MISO    19
  #define LCD_SCLK    18
  #define LCD_SPI_HZ  40000000
  #define LCD_MADCTL   0x08        // BGR  (ILI9342 is natively landscape)
  #define LCD_COL_OFS  0
  #define LCD_ROW_OFS  0
  #define LCD_INVERT   true

#elif defined(ARDUINO_M5STACK_CORE2)
  // M5Stack Core2 — ILI9342, 320x240
  // LCD power, backlight, and reset are controlled via the AXP192 PMIC over
  // I2C (SDA=21, SCL=22, addr 0x34), so the GPIO pins are set to -1 here.
  // Touch: FT6336 capacitive controller on I2C (addr 0x38, same bus as AXP).
  #define LCD_ILI9342
  #define LCD_WIDTH   320
  #define LCD_HEIGHT  240
  #define LCD_CS       5
  #define LCD_DC      15
  #define LCD_RST     -1           // controlled via AXP192
  #define LCD_BL      -1           // controlled via AXP192
  #define LCD_MOSI    23
  #define LCD_MISO    38
  #define LCD_SCLK    18
  #define LCD_SPI_HZ  40000000
  #define LCD_MADCTL   0x08
  #define LCD_COL_OFS  0
  #define LCD_ROW_OFS  0
  #define LCD_INVERT   true
  #define LCD_HAS_TOUCH              // FT6336 capacitive touch on I2C

#elif defined(ARDUINO_M5STACK_STAMP_S3) || defined(ARDUINO_M5STACK_STAMPLC)
  // M5Stack StamPLC — an industrial PLC board with an ESP32-S3 (StampS3)
  // module plugged in.  ST7789 LCD, 240x135.
  // The LCD and W5500 Ethernet share the same SPI bus (MOSI=8, MISO=9,
  // SCK=7).  LCD CS=12, W5500 CS=11.  RST GPIO 3 is shared between them.
  // Backlight is controlled via the PI4IOE5V6408 I2C GPIO expander
  // (addr 0x43, SDA=13, SCL=15) — bit 7 of the output register, active-low.
  //
  // NOTE: The backlight code below accesses the PI4IOE5V6408 via the
  // Arduino Wire library.  This is sufficient for network-only testing
  // but conflicts with M5Unified's I2C driver if other StamPLC I2C
  // peripherals (relays, inputs, sensors, RTC) are also in use.
  // The chipguy_M5StamPLC library's NetClient_LVGL example contains a
  // StamPLC-specific lv_setup.hpp that avoids this conflict.
  #warning "StamPLC: backlight uses Wire I2C which may conflict with other I2C peripherals. See chipguy_M5StamPLC library's NetClient_LVGL example for a conflict-free version."
  #define LCD_ST7789
  #define LCD_WIDTH   240
  #define LCD_HEIGHT  135
  #define LCD_CS      12
  #define LCD_DC       6
  #define LCD_RST      3
  #define LCD_BL      -1           // controlled via PI4IOE5V6408 I2C expander
  #define LCD_MOSI     8
  #define LCD_MISO     9
  #define LCD_SCLK     7
  #define LCD_SPI_HZ  40000000
  #define LCD_MADCTL   0x60        // MV | MX  (rotation 1, RGB)
  #define LCD_COL_OFS  40
  #define LCD_ROW_OFS  53
  #define LCD_INVERT   true

#else
  #error "lv_setup.hpp: unsupported board — add your LCD configuration here"
#endif

// ════════════════════════════════════════════════════════════════════════
// Per-board rotation tables
// ════════════════════════════════════════════════════════════════════════
//
// Each entry holds the MADCTL byte, logical width/height, and column/row
// address offsets for one of the four cardinal rotations (0°, 90°, 180°, 270°).
//
// ILI9342 is natively landscape 320×240 with no address offsets.
// ST7789 (StamPLC) is a 135×240 window inside a 240×320 framebuffer,
// so the offsets change with rotation.

struct LcdRotation {
    uint8_t madctl;
    int     width;
    int     height;
    int     col_ofs;
    int     row_ofs;
};

#if defined(LCD_ILI9342)
static const LcdRotation _lcd_rotations[] = {
    { 0x08, 320, 240, 0, 0 },  //   0° — landscape (default)
    { 0x68, 240, 320, 0, 0 },  //  90° — portrait
    { 0xC8, 320, 240, 0, 0 },  // 180° — landscape flipped
    { 0xA8, 240, 320, 0, 0 },  // 270° — portrait flipped
};
#elif defined(LCD_ST7789)
static const LcdRotation _lcd_rotations[] = {
    { 0x60, 240, 135, 40, 53 }, //   0° — landscape (default)
    { 0x00, 135, 240, 52, 40 }, //  90° — portrait
    { 0xA0, 240, 135, 40, 52 }, // 180° — landscape flipped
    { 0xC0, 135, 240, 53, 40 }, // 270° — portrait flipped
};
#endif

// ════════════════════════════════════════════════════════════════════════
// lv_setup_class — LCD + LVGL initialization and runtime
// ════════════════════════════════════════════════════════════════════════

class lv_setup_class {
public:
    int width()  const { return _w; }
    int height() const { return _h; }

    /// Call once from setup().  Initializes the LCD hardware, powers on
    /// the backlight (board-specific), and sets up LVGL with a display
    /// driver and optional touch input.
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

        // ── Board-specific power-on ──────────────────────────────────
        // Core2: The AXP192 PMIC must be configured first — it supplies
        //        power to the LCD and controls the hardware reset line.
#if defined(ARDUINO_M5STACK_CORE2)
        _axp192_init();
#endif

        // ── GPIO setup ───────────────────────────────────────────────
        // DC (data/command) and CS are always GPIO-driven because we
        // manage them manually around each SPI transaction.
        gpio_set_direction((gpio_num_t)LCD_DC, GPIO_MODE_OUTPUT);
        gpio_set_direction((gpio_num_t)LCD_CS, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)LCD_CS, 1);          // deselect LCD
#if LCD_BL >= 0
        // Simple GPIO backlight (Core / Fire only)
        gpio_set_direction((gpio_num_t)LCD_BL, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)LCD_BL, 1);          // backlight ON
#endif

        // ── Hardware reset ───────────────────────────────────────────
        // Pulse the reset pin low for 20 ms to put the LCD controller
        // into a known state.  On Core2 this is handled by the AXP192
        // (LCD_RST = -1), so this block is skipped.
#if LCD_RST >= 0
        gpio_set_direction((gpio_num_t)LCD_RST, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)LCD_RST, 1);
        delay(10);
        gpio_set_level((gpio_num_t)LCD_RST, 0);
        delay(20);
        gpio_set_level((gpio_num_t)LCD_RST, 1);
        delay(150);
#endif

        // ── SPI bus initialization ───────────────────────────────────
        // Register the SPI bus with the ESP-IDF SPI master driver.  If
        // the bus was already initialized (e.g. by the ETH driver for
        // W5500), spi_bus_initialize returns ESP_ERR_INVALID_STATE and
        // we treat that as success — both devices share the bus.
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

        // Register the LCD as an SPI device on the bus.  CS is managed
        // manually via GPIO (spics_io_num = -1) so we can hold it low
        // across the multi-transaction pixel-push sequence.
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
        // Send the MIPI DCS startup sequence (sleep out, pixel format,
        // rotation, inversion, display on).
        _lcd_init_panel();

        // ── StamPLC backlight ────────────────────────────────────────
        // The StamPLC's backlight is controlled by bit 7 of the
        // PI4IOE5V6408 I2C GPIO expander.  It must be enabled AFTER the
        // LCD panel init completes — doing it earlier causes the
        // backlight to turn off again (matching M5GFX's init order).
#if defined(ARDUINO_M5STACK_STAMP_S3) || defined(ARDUINO_M5STACK_STAMPLC)
        _pi4io1_init();
#endif

        // ── LVGL setup ───────────────────────────────────────────────
        lv_init();

        // Create an LVGL display backed by our SPI flush callback.
        // A single partial-render buffer (10 rows) keeps RAM usage low.
        static lv_display_t *disp = lv_display_create(_w, _h);
        lv_display_set_flush_cb(disp, _disp_flush);
        lv_display_set_user_data(disp, this);

        static lv_color_t *buf = (lv_color_t *)heap_caps_malloc(
            _w * 10 * sizeof(lv_color_t), MALLOC_CAP_DMA);
        lv_display_set_buffers(disp, buf, NULL,
            _w * 10 * sizeof(lv_color_t), LV_DISPLAY_RENDER_MODE_PARTIAL);

        // Touch input (Core2 only — FT6336 capacitive controller on I2C)
#if defined(LCD_HAS_TOUCH)
        static lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, _touchpad_read);
        lv_indev_set_user_data(indev, this);
#endif

        // Tell LVGL to use millis() as its tick source
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
    // These send individual commands and data bytes to the LCD
    // controller.  DC (data/command) is toggled via GPIO before each
    // transaction, and CS is managed manually.

    /// Send a single command byte (DC=0)
    void _lcd_cmd(uint8_t cmd) {
        if (!_lcd_spi) return;
        gpio_set_level((gpio_num_t)LCD_DC, 0);          // command mode
        gpio_set_level((gpio_num_t)LCD_CS, 0);
        spi_transaction_t t = {};
        t.length = 8;
        t.tx_data[0] = cmd;
        t.flags = SPI_TRANS_USE_TXDATA;
        spi_device_polling_transmit(_lcd_spi, &t);
        gpio_set_level((gpio_num_t)LCD_CS, 1);
    }

    /// Send a data buffer (DC=1)
    void _lcd_data(const uint8_t *data, size_t len) {
        if (!_lcd_spi || len == 0) return;
        gpio_set_level((gpio_num_t)LCD_DC, 1);          // data mode
        gpio_set_level((gpio_num_t)LCD_CS, 0);
        spi_transaction_t t = {};
        t.length = len * 8;
        t.tx_buffer = data;
        spi_device_polling_transmit(_lcd_spi, &t);
        gpio_set_level((gpio_num_t)LCD_CS, 1);
    }

    /// Send a single data byte
    void _lcd_data8(uint8_t val) {
        _lcd_data(&val, 1);
    }

    /// Set the LCD controller's column/row address window for the next
    /// pixel write.  Uses MIPI DCS commands CASET (0x2A), RASET (0x2B),
    /// and RAMWR (0x2C).  The column/row offsets account for panels
    /// where the visible area doesn't start at (0,0) in the controller's
    /// internal framebuffer.
    void _lcd_set_addr_window(int x0, int y0, int x1, int y1) {
        x0 += _col_ofs;  x1 += _col_ofs;
        y0 += _row_ofs;  y1 += _row_ofs;

        uint8_t col[4] = {
            (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF),
            (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF)
        };
        _lcd_cmd(0x2A);               // CASET — Column Address Set
        _lcd_data(col, 4);

        uint8_t row[4] = {
            (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF),
            (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF)
        };
        _lcd_cmd(0x2B);               // RASET — Row Address Set
        _lcd_data(row, 4);

        _lcd_cmd(0x2C);               // RAMWR — Memory Write
    }

    /// Push a block of RGB565 pixel data to the LCD.  DC stays high and
    /// CS stays low for the entire transfer.  Large blocks are split
    /// into 32 KB chunks to stay within ESP-IDF DMA limits.
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
    // The startup sequence is the same for ILI9342 and ST7789 — both
    // implement the standard MIPI DCS command set.  Only the MADCTL
    // value, inversion setting, and address offsets differ (handled by
    // the board-specific #defines above).

    void _lcd_init_panel() {
        _lcd_cmd(0x01);               // SWRESET — Software Reset
        delay(150);

        _lcd_cmd(0x11);               // SLPOUT  — Sleep Out
        delay(255);

        _lcd_cmd(0x3A);               // COLMOD  — Pixel Format
        _lcd_data8(0x55);             //           16-bit RGB565

        _lcd_cmd(0x36);               // MADCTL  — Memory Access Control
        _lcd_data8(_madctl);          //           (rotation + color order)

#if LCD_INVERT
        _lcd_cmd(0x21);               // INVON   — Display Inversion ON
#else
        _lcd_cmd(0x20);               // INVOFF  — Display Inversion OFF
#endif

        _lcd_cmd(0x13);               // NORON   — Normal Display Mode ON
        delay(10);

        _lcd_cmd(0x29);               // DISPON  — Display ON
        delay(10);
    }

    // ── LVGL callbacks ───────────────────────────────────────────────

    /// Tick source for LVGL — just returns Arduino millis().
    static uint32_t _tick_get(void) { return millis(); }

    /// LVGL display flush callback.  Called by LVGL whenever a region of
    /// the screen has been rendered and needs to be sent to the LCD.
    /// Acquires exclusive SPI bus access so the W5500 Ethernet driver
    /// (running on a different core) cannot interleave transactions.
    static void _disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *pixelmap) {
        auto *self = (lv_setup_class *)lv_display_get_user_data(disp);

        if (!self->_lcd_spi) {
            lv_disp_flush_ready(disp);
            return;
        }

        int w = (area->x2 - area->x1 + 1);
        int h = (area->y2 - area->y1 + 1);
        size_t pixel_count = w * h;

        // LVGL renders RGB565 in little-endian byte order (native ESP32),
        // but SPI LCD controllers expect big-endian (MSB first).  Swap
        // bytes in-place before transmitting.
        uint16_t *px = (uint16_t *)pixelmap;
        for (size_t i = 0; i < pixel_count; i++) {
            px[i] = __builtin_bswap16(px[i]);
        }

        // Acquire exclusive bus access — blocks W5500 until we're done
        spi_device_acquire_bus(self->_lcd_spi, portMAX_DELAY);

        self->_lcd_set_addr_window(area->x1, area->y1, area->x2, area->y2);
        self->_lcd_push_pixels(pixelmap, pixel_count * 2);

        spi_device_release_bus(self->_lcd_spi);

        lv_disp_flush_ready(disp);
    }

    // ── Touch input: Core2 FT6336 capacitive controller via I2C ─────
    // The FT6336 is at I2C address 0x38 on the same bus as the AXP192
    // (SDA=21, SCL=22).  Register 0x02 gives the number of active
    // touches; registers 0x03-0x06 give the X/Y coordinates.
#if defined(LCD_HAS_TOUCH)
    static void _touchpad_read(lv_indev_t *indev, lv_indev_data_t *data) {
        Wire.beginTransmission(0x38);
        Wire.write(0x02);                   // register: number of touches
        Wire.endTransmission(false);
        Wire.requestFrom((uint8_t)0x38, (uint8_t)5);

        uint8_t touches = Wire.read() & 0x0F;
        if (touches > 0) {
            uint8_t xh = Wire.read();        // 0x03
            uint8_t xl = Wire.read();        // 0x04
            uint8_t yh = Wire.read();        // 0x05
            uint8_t yl = Wire.read();        // 0x06
            data->point.x = ((xh & 0x0F) << 8) | xl;
            data->point.y = ((yh & 0x0F) << 8) | yl;
            data->state = LV_INDEV_STATE_PR;
        } else {
            data->state = LV_INDEV_STATE_REL;
        }
    }
#endif

    // ── AXP192 power management for M5Stack Core2 ───────────────────
    // The AXP192 PMIC (I2C addr 0x34, SDA=21, SCL=22) controls:
    //   - LDO2: LCD logic power
    //   - DC-DC3: backlight voltage (~3.0 V)
    //   - GPIO4: LCD hardware reset (directly wired to the ILI9342)
    // This must run before any SPI traffic to the LCD.
#if defined(ARDUINO_M5STACK_CORE2)
    void _axp192_init() {
        Wire.begin(21, 22);
        Wire.setClock(400000);

        // Enable DC-DC3 (backlight) and LDO2 (LCD logic power)
        uint8_t val = _axp_read(0x12);
        val |= (1 << 1) | (1 << 2);
        _axp_write(0x12, val);

        // DC-DC3 voltage ~3.0 V (backlight brightness)
        _axp_write(0x27, 0x5C);

        // Pulse GPIO4 low to hardware-reset the LCD controller
        _axp_write(0x96, _axp_read(0x96) & ~(1 << 1));
        delay(20);
        _axp_write(0x96, _axp_read(0x96) | (1 << 1));
        delay(150);
    }

    uint8_t _axp_read(uint8_t reg) {
        Wire.beginTransmission(0x34);
        Wire.write(reg);
        Wire.endTransmission(false);
        Wire.requestFrom((uint8_t)0x34, (uint8_t)1);
        return Wire.read();
    }

    void _axp_write(uint8_t reg, uint8_t val) {
        Wire.beginTransmission(0x34);
        Wire.write(reg);
        Wire.write(val);
        Wire.endTransmission();
    }
#endif

    // ── PI4IOE5V6408 backlight control for StamPLC ──────────────────
    // The StamPLC (an industrial PLC with an ESP32-S3 StampS3 module)
    // uses a PI4IOE5V6408 I2C GPIO expander (addr 0x43, SDA=13, SCL=15)
    // to control the LCD backlight on bit 7.  The backlight signal is
    // active-low: clearing bit 7 of the output register turns it ON.
    //
    // Register map (only the bits we touch):
    //   0x03  I/O Direction    — 1 = output, 0 = input
    //   0x05  Output Port      — drive level for output pins
    //   0x07  High-Impedance   — 1 = high-Z, 0 = normal drive
    //   0x0D  Pull Enable/Dir  — pull-up/down configuration
#if defined(ARDUINO_M5STACK_STAMP_S3) || defined(ARDUINO_M5STACK_STAMPLC)
    void _pi4io1_init() {
        Wire.begin(13, 15);
        Wire.setClock(400000);

        // Configure bit 7 as a driven output
        _pi4io_bitOn(0x03, 1 << 7);     // direction = output
        _pi4io_bitOff(0x0D, 1 << 7);    // disable pull-down
        _pi4io_bitOff(0x07, 1 << 7);    // disable high-impedance

        // Turn backlight ON (active-low: clear bit 7)
        _pi4io_bitOff(0x05, 1 << 7);
    }

    uint8_t _pi4io_read(uint8_t reg) {
        Wire.beginTransmission(0x43);
        Wire.write(reg);
        Wire.endTransmission(false);
        Wire.requestFrom((uint8_t)0x43, (uint8_t)1);
        return Wire.read();
    }

    /// Read-modify-write: set bits in a PI4IOE5V6408 register
    void _pi4io_bitOn(uint8_t reg, uint8_t mask) {
        uint8_t val = _pi4io_read(reg) | mask;
        Wire.beginTransmission(0x43);
        Wire.write(reg);
        Wire.write(val);
        Wire.endTransmission();
    }

    /// Read-modify-write: clear bits in a PI4IOE5V6408 register
    void _pi4io_bitOff(uint8_t reg, uint8_t mask) {
        uint8_t val = _pi4io_read(reg) & ~mask;
        Wire.beginTransmission(0x43);
        Wire.write(reg);
        Wire.write(val);
        Wire.endTransmission();
    }
#endif
};

static lv_setup_class lv_setup;
