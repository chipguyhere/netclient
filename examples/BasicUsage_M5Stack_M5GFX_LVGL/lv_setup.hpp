#pragma once

// LVGL display and touch driver for M5GFX

#include <M5GFX.h>
#include <lvgl.h>

class lv_setup_class {
public:
    void begin(M5GFX &gfx) {
        _gfx = &gfx;

        lv_init();

        int w = gfx.width();
        int h = gfx.height();

        static lv_display_t *disp = lv_display_create(w, h);
        lv_display_set_flush_cb(disp, _disp_flush);
        lv_display_set_user_data(disp, this);

        // Heap-allocated buffer — avoids using scarce DRAM BSS space
        static lv_color_t *buf = (lv_color_t *)heap_caps_malloc(240 * 5 * sizeof(lv_color_t), MALLOC_CAP_DEFAULT);
        lv_display_set_buffers(disp, buf, NULL, 240 * 5 * sizeof(lv_color_t), LV_DISPLAY_RENDER_MODE_PARTIAL);

        // Register touch input if the display has a touchscreen
        if (gfx.touch()) {
            static lv_indev_t *indev = lv_indev_create();
            lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
            lv_indev_set_read_cb(indev, _touchpad_read);
            lv_indev_set_user_data(indev, this);
        }

        lv_tick_set_cb(_tick_get);
    }

private:
    M5GFX *_gfx = nullptr;

    static uint32_t _tick_get(void) { return millis(); }

    static void _disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *pixelmap) {
        auto *self = (lv_setup_class *)lv_display_get_user_data(disp);

        int w = (area->x2 - area->x1 + 1);
        int h = (area->y2 - area->y1 + 1);

        self->_gfx->startWrite();
        self->_gfx->setAddrWindow(area->x1, area->y1, w, h);
        self->_gfx->writePixels((lgfx::rgb565_t *)pixelmap, w * h);
        self->_gfx->endWrite();

        lv_disp_flush_ready(disp);
    }

    static void _touchpad_read(lv_indev_t *indev, lv_indev_data_t *data) {
        auto *self = (lv_setup_class *)lv_indev_get_user_data(indev);
        lgfx::touch_point_t tp;
        if (self->_gfx->getTouch(&tp, 1)) {
            data->state = LV_INDEV_STATE_PR;
            data->point.x = tp.x;
            data->point.y = tp.y;
        } else {
            data->state = LV_INDEV_STATE_REL;
        }
    }
};

static lv_setup_class lv_setup;
