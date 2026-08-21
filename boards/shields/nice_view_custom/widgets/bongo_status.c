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

static void bongo_reset_work_handler(struct k_work *work);
static void bongo_idle_work_handler(struct k_work *work);

K_WORK_DELAYABLE_DEFINE(bongo_reset_work, bongo_reset_work_handler);
K_WORK_DELAYABLE_DEFINE(bongo_idle_work, bongo_idle_work_handler);

static void draw_bongo_cat(struct bongo_status_widget *widget, const lv_img_dsc_t *frame) {
    lv_obj_t *canvas = lv_obj_get_child(widget->obj, 1);

    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);
    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_black_dsc);

    // The bongo frames are 68x40; center them vertically inside the 68x68
    // canvas before rotating the whole canvas 90 degrees (the display is
    // mounted rotated, so the cat ends up standing upright and centered).
    lv_draw_img_dsc_t img_dsc;
    lv_draw_img_dsc_init(&img_dsc);
    lv_canvas_draw_img(canvas, 0, 14, frame, &img_dsc);

    rotate_canvas(canvas, widget->cbuf2);
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

static void draw_top(lv_obj_t *widget, lv_color_t cbuf[], const struct bongo_status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 0);

    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &lv_font_montserrat_16, LV_TEXT_ALIGN_RIGHT);
    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);

    // Fill background
    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_black_dsc);

    // Draw battery (top left)
    struct status_state batt_state = {.battery = state->battery, .charging = false};
    draw_battery(canvas, &batt_state);

    // Draw connection status (top right)
    lv_canvas_draw_text(canvas, 0, 0, CANVAS_SIZE, &label_dsc,
                        state->connected ? LV_SYMBOL_WIFI : LV_SYMBOL_CLOSE);

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

    // Top canvas (battery + connection), same layout as the stock nice_view
    // peripheral widget: 68x68 canvas rotated 90 degrees on the right side,
    // showing battery at top-left and the connection symbol at top-right.
    lv_obj_t *top = lv_canvas_create(widget->obj);
    lv_obj_align(top, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_canvas_set_buffer(top, widget->cbuf, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);

    // Bongo cat canvas: 68x68 rotated 90 degrees, centered in the left area
    // (between the left edge and the top-right status canvas).
    lv_obj_t *bongo = lv_canvas_create(widget->obj);
    lv_obj_align(bongo, LV_ALIGN_LEFT_MID, 12, 0);
    lv_canvas_set_buffer(bongo, widget->cbuf2, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);

    sys_slist_append(&widgets, &widget->node);
    widget_bongo_status_init();

    // Draw the initial resting frame.
    draw_bongo_cat(widget, &bongo_resting);

    return 0;
}

lv_obj_t *zmk_widget_bongo_obj(struct bongo_status_widget *widget) { return widget->obj; }
