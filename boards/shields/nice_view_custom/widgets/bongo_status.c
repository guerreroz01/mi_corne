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
 * No central-only ZMK API (keymap, endpoints, BLE profile, HID, WPM) is used.
 */
static bool keys_active = false;
static bool idle_toggle = false;

static void bongo_reset_work_handler(struct k_work *work);
static void bongo_idle_work_handler(struct k_work *work);

K_WORK_DELAYABLE_DEFINE(bongo_reset_work, bongo_reset_work_handler);
K_WORK_DELAYABLE_DEFINE(bongo_idle_work, bongo_idle_work_handler);

static void set_bongo_frame(const lv_img_dsc_t *frame) {
    struct bongo_status_widget *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { lv_img_set_src(widget->img, frame); }
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
                                       .battery = zmk_battery_state_of_charge()};

    if (eh != NULL) {
        const struct zmk_position_state_changed *pos = as_zmk_position_state_changed(eh);
        if (pos != NULL) {
            state.key_pressed = pos->state;
        }

        const struct zmk_battery_state_changed *bat = as_zmk_battery_state_changed(eh);
        if (bat != NULL) {
            state.battery = bat->state_of_charge;
        }
    }

    return state;
}

static void set_bongo_status(struct bongo_status_widget *widget,
                             struct bongo_status_state state) {
    bool was_active = keys_active;

    if (state.battery != widget->state.battery) {
        widget->state.battery = state.battery;
        lv_label_set_text_fmt(widget->label, "%d%%", state.battery);
    }

    if (state.key_pressed) {
        keys_active = true;
        lv_img_set_src(widget->img, &bongo_casualright);
        k_work_schedule(&bongo_reset_work, K_MSEC(BONGO_RESET_DELAY_MS));
    } else if (was_active) {
        keys_active = false;
        k_work_cancel(&bongo_reset_work);
        k_work_cancel(&bongo_idle_work);
        idle_toggle = false;
        lv_img_set_src(widget->img, &bongo_resting);
        k_work_schedule(&bongo_idle_work, K_MSEC(BONGO_IDLE_INTERVAL_MS));
    }
}

static void bongo_status_update_cb(struct bongo_status_state state) {
    struct bongo_status_widget *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_bongo_status(widget, state); }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_bongo_status, struct bongo_status_state, bongo_status_update_cb,
                            get_state)

ZMK_SUBSCRIPTION(widget_bongo_status, zmk_position_state_changed);
ZMK_SUBSCRIPTION(widget_bongo_status, zmk_battery_state_changed);

int zmk_widget_bongo_init(struct bongo_status_widget *widget, lv_obj_t *parent) {
    widget->state.key_pressed = false;
    widget->state.battery = 0;

    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 160, 68);
    lv_obj_set_style_bg_color(widget->obj, LVGL_BACKGROUND, 0);
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_COVER, 0);

    widget->img = lv_img_create(widget->obj);
    lv_img_set_src(widget->img, &bongo_resting);
    lv_obj_align(widget->img, LV_ALIGN_LEFT_MID, 0, 0);

    widget->label = lv_label_create(widget->obj);
    lv_obj_set_style_text_color(widget->label, LVGL_FOREGROUND, 0);
    lv_obj_set_style_text_font(widget->label, &lv_font_montserrat_14, 0);
    lv_label_set_text(widget->label, "--%");
    lv_obj_align(widget->label, LV_ALIGN_RIGHT_MID, 0, 0);

    sys_slist_append(&widgets, &widget->node);
    widget_bongo_status_init();

    return 0;
}

lv_obj_t *zmk_widget_bongo_obj(struct bongo_status_widget *widget) { return widget->obj; }
