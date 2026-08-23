/*
 *
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/battery.h>
#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/split/bluetooth/peripheral.h>
#include <zmk/events/split_peripheral_status_changed.h>

#include "bongo_status.h"
#include "bongocatart.h"
#include "util.h"

#define BONGO_RESET_DELAY_MS 200
#define BONGO_IDLE_INTERVAL_MS 1500

/* Local WPM-style key counter, replicating the upstream nice-view-mod
 * WPM graph. The real ZMK WPM event (zmk_wpm_state_changed) only exists on
 * the split central, so this half computes its own words-per-minute from the
 * local zmk_position_state_changed key presses, mirroring the ZMK formula
 * (5 keystrokes = 1 word, window of a few seconds). */
#define WPM_UPDATE_INTERVAL_MS 1000
#define WPM_RESET_INTERVAL_MS 5000
#define CHARS_PER_WORD 5.0
#define WPM_HISTORY_SIZE 10

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

/*
 * Animation state. The bongo cat is animated from LOCAL key presses only:
 * zmk_position_state_changed is raised on the split peripheral half for its
 * own matrix, and zmk_battery_state_changed comes from the local fuel gauge.
 * Connection state comes from zmk_split_peripheral_status_changed (local
 * link to the dongle/central). No central-only ZMK API (keymap, endpoints,
 * BLE profile, HID, WPM) is used.
 */
static bool keys_active = false;
static bool idle_toggle = false;

static uint32_t local_key_press_count = 0;
static uint32_t wpm_update_counter = 0;
static uint8_t wpm_history[WPM_HISTORY_SIZE] = {0};

static void bongo_reset_work_handler(struct k_work *work);
static void bongo_idle_work_handler(struct k_work *work);
static void wpm_tick_work_handler(struct k_work *work);
static void draw_top(lv_obj_t *widget, lv_color_t cbuf[], const struct bongo_status_state *state);

K_WORK_DELAYABLE_DEFINE(bongo_reset_work, bongo_reset_work_handler);
K_WORK_DELAYABLE_DEFINE(bongo_idle_work, bongo_idle_work_handler);
K_WORK_DELAYABLE_DEFINE(wpm_tick_work, wpm_tick_work_handler);

static void draw_bongo_cat(struct bongo_status_widget *widget, const lv_img_dsc_t *frame) {
    lv_obj_t *canvas = lv_obj_get_child(widget->obj, 1);

    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);
    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_black_dsc);

    // The bongo frames are 68x40; draw them at y=28 inside the 68x68 canvas
    // and let rotate_canvas rotate the whole canvas 90 degrees, exactly like
    // the upstream nice-view-mod widget does (draw_middle).
    lv_draw_img_dsc_t img_dsc;
    lv_draw_img_dsc_init(&img_dsc);
    lv_canvas_draw_img(canvas, 0, 28, frame, &img_dsc);

    rotate_canvas(canvas, widget->cbuf2);
}

static void draw_oliver(struct bongo_status_widget *widget) {
    lv_obj_t *canvas = lv_obj_get_child(widget->obj, 2);

    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);
    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_black_dsc);

    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &lv_font_montserrat_14, LV_TEXT_ALIGN_CENTER);
    lv_canvas_draw_text(canvas, 0, 8, CANVAS_SIZE, &label_dsc, "OLIVER");

    rotate_canvas(canvas, widget->cbuf3);
}

static void set_bongo_frame(const lv_img_dsc_t *frame) {
    struct bongo_status_widget *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { draw_bongo_cat(widget, frame); }
}

static void bongo_reset_work_handler(struct k_work *work) {
    keys_active = false;
    set_bongo_frame(&bongo_resting);
    k_work_schedule(&bongo_idle_work, K_MSEC(BONGO_IDLE_INTERVAL_MS));
}

static void bongo_idle_work_handler(struct k_work *work) {
    if (!keys_active) {
        idle_toggle = !idle_toggle;
        set_bongo_frame(idle_toggle ? &bongo_inhale : &bongo_exhale);
    }
    k_work_schedule(&bongo_idle_work, K_MSEC(BONGO_IDLE_INTERVAL_MS));
}

static struct bongo_status_state get_state(const zmk_event_t *eh) {
    struct bongo_status_state state = {.key_pressed = false,
                                       .battery = zmk_battery_state_of_charge(),
                                       .connected = zmk_split_bt_peripheral_is_connected()};

    if (eh != NULL) {
        const struct zmk_position_state_changed *pos = as_zmk_position_state_changed(eh);
        if (pos != NULL) {
            state.key_pressed = pos->state;
            // Count key releases only, like the upstream ZMK WPM counter.
            if (!pos->state) {
                local_key_press_count++;
            }
        }

        const struct zmk_battery_state_changed *bat = as_zmk_battery_state_changed(eh);
        if (bat != NULL) {
            state.battery = bat->state_of_charge;
        }

        const struct zmk_split_peripheral_status_changed *sp =
            as_zmk_split_peripheral_status_changed(eh);
        if (sp != NULL) {
            state.connected = sp->connected;
        }
    }

    return state;
}

static void wpm_tick_work_handler(struct k_work *work) {
    // Same formula as ZMK's src/wpm.c: words = keystrokes / CHARS_PER_WORD,
    // wpm = words / (elapsed_seconds / 60).
    wpm_update_counter++;
    uint8_t wpm = (uint8_t)((local_key_press_count / CHARS_PER_WORD) /
                            (wpm_update_counter * WPM_UPDATE_INTERVAL_MS / 60000.0));

    // Shift history and store the new sample.
    for (int i = 0; i < WPM_HISTORY_SIZE - 1; i++) {
        wpm_history[i] = wpm_history[i + 1];
    }
    wpm_history[WPM_HISTORY_SIZE - 1] = wpm;

    struct bongo_status_widget *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        struct bongo_status_state st = {.battery = widget->state.battery,
                                        .connected = widget->state.connected};
        draw_top(widget->obj, widget->cbuf, &st);
    }

    if (wpm_update_counter >= WPM_RESET_INTERVAL_MS / WPM_UPDATE_INTERVAL_MS) {
        wpm_update_counter = 0;
        local_key_press_count = 0;
    }

    k_work_schedule(&wpm_tick_work, K_MSEC(WPM_UPDATE_INTERVAL_MS));
}

static void draw_top(lv_obj_t *widget, lv_color_t cbuf[], const struct bongo_status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 0);

    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &lv_font_montserrat_16, LV_TEXT_ALIGN_RIGHT);
    lv_draw_label_dsc_t label_dsc_wpm;
    init_label_dsc(&label_dsc_wpm, LVGL_FOREGROUND, &lv_font_unscii_8, LV_TEXT_ALIGN_RIGHT);
    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);
    lv_draw_rect_dsc_t rect_white_dsc;
    init_rect_dsc(&rect_white_dsc, LVGL_FOREGROUND);
    lv_draw_line_dsc_t line_dsc;
    init_line_dsc(&line_dsc, LVGL_FOREGROUND, 1);

    // Fill background
    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_black_dsc);

    // Draw battery (top left)
    struct status_state batt_state = {.battery = state->battery, .charging = false};
    draw_battery(canvas, &batt_state);

    // Draw connection status (top right)
    lv_canvas_draw_text(canvas, 0, 0, CANVAS_SIZE, &label_dsc,
                        state->connected ? LV_SYMBOL_WIFI : LV_SYMBOL_CLOSE);

    // Draw the WPM-style key counter with its graph, replicating the
    // upstream nice-view-mod widget (CONFIG_ZMK_WPM_GRAPH_ENABLED block).
    lv_canvas_draw_rect(canvas, 0, 21, 68, 42, &rect_white_dsc);
    lv_canvas_draw_rect(canvas, 1, 22, 66, 40, &rect_black_dsc);

    // Draw WPM text and graph
    char wpm_text[6] = {};
    snprintf(wpm_text, sizeof(wpm_text), "%d", wpm_history[WPM_HISTORY_SIZE - 1]);
    lv_canvas_draw_text(canvas, 42, 52, 24, &label_dsc_wpm, wpm_text);

    // Draw WPM graph
    int max = 0;
    int min = 256;

    for (int i = 0; i < WPM_HISTORY_SIZE; i++) {
        if (wpm_history[i] > max) {
            max = wpm_history[i];
        }
        if (wpm_history[i] < min) {
            min = wpm_history[i];
        }
    }

    int range = max - min;
    if (range == 0) {
        range = 1;
    }

    lv_point_t points[WPM_HISTORY_SIZE];
    for (int i = 0; i < WPM_HISTORY_SIZE; i++) {
        points[i].x = 2 + i * 7;
        points[i].y = 60 - (wpm_history[i] - min) * 36 / range;
    }
    lv_canvas_draw_line(canvas, points, WPM_HISTORY_SIZE, &line_dsc);

    // Rotate canvas
    rotate_canvas(canvas, cbuf);
}

static void redraw_bongo_status(struct bongo_status_widget *widget,
                                struct bongo_status_state state) {
    bool was_active = keys_active;

    if (state.battery != widget->state.battery || state.connected != widget->state.connected) {
        widget->state.battery = state.battery;
        widget->state.connected = state.connected;
        draw_top(widget->obj, widget->cbuf, &state);
    }

    if (state.key_pressed) {
        keys_active = true;
        set_bongo_frame(&bongo_casualright);
        k_work_schedule(&bongo_reset_work, K_MSEC(BONGO_RESET_DELAY_MS));
    } else if (was_active) {
        keys_active = false;
        k_work_cancel(&bongo_reset_work);
        k_work_cancel(&bongo_idle_work);
        idle_toggle = false;
        set_bongo_frame(&bongo_resting);
        k_work_schedule(&bongo_idle_work, K_MSEC(BONGO_IDLE_INTERVAL_MS));
    }
}

static void bongo_status_update_cb(struct bongo_status_state state) {
    struct bongo_status_widget *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { redraw_bongo_status(widget, state); }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_bongo_status, struct bongo_status_state, bongo_status_update_cb,
                            get_state)

ZMK_SUBSCRIPTION(widget_bongo_status, zmk_position_state_changed);
ZMK_SUBSCRIPTION(widget_bongo_status, zmk_battery_state_changed);
ZMK_SUBSCRIPTION(widget_bongo_status, zmk_split_peripheral_status_changed);

int zmk_widget_bongo_init(struct bongo_status_widget *widget, lv_obj_t *parent) {
    widget->state.key_pressed = false;
    widget->state.battery = 0;
    widget->state.connected = false;

    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 160, 68);

    // Three 68x68 canvases, each rotated 90 degrees, laid out exactly like
    // the upstream nice-view-mod status widget: battery/connection on the
    // right, the bongo cat in the middle, and the "OLIVER" text on the left
    // (which ends up at the bottom once the panel rotation is applied).
    lv_obj_t *top = lv_canvas_create(widget->obj);
    lv_obj_align(top, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_canvas_set_buffer(top, widget->cbuf, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);

    lv_obj_t *bongo = lv_canvas_create(widget->obj);
    lv_obj_align(bongo, LV_ALIGN_TOP_LEFT, 24, 0);
    lv_canvas_set_buffer(bongo, widget->cbuf2, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);

    lv_obj_t *oliver = lv_canvas_create(widget->obj);
    lv_obj_align(oliver, LV_ALIGN_TOP_LEFT, -44, 0);
    lv_canvas_set_buffer(oliver, widget->cbuf3, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);

    sys_slist_append(&widgets, &widget->node);
    widget_bongo_status_init();

    // Draw the "OLIVER" text once (static) and the initial resting frame.
    draw_oliver(widget);
    draw_bongo_cat(widget, &bongo_resting);

    // Start the local WPM-style key counter tick.
    k_work_schedule(&wpm_tick_work, K_MSEC(WPM_UPDATE_INTERVAL_MS));

    return 0;
}

lv_obj_t *zmk_widget_bongo_obj(struct bongo_status_widget *widget) { return widget->obj; }
