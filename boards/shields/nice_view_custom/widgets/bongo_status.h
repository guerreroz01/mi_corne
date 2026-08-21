/*
 *
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once

#include <lvgl.h>
#include <zephyr/kernel.h>
#include "util.h"

struct bongo_status_state {
    bool key_pressed;
    uint8_t battery;
};

struct bongo_status_widget {
    sys_snode_t node;
    lv_obj_t *obj;
    lv_obj_t *img;
    lv_obj_t *label;
    struct bongo_status_state state;
};

int zmk_widget_bongo_init(struct bongo_status_widget *widget, lv_obj_t *parent);
lv_obj_t *zmk_widget_bongo_obj(struct bongo_status_widget *widget);
