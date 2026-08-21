/*
 *
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 */

#include "widgets/bongo_status.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct bongo_status_widget bongo_widget;

lv_obj_t *zmk_display_status_screen() {
    lv_obj_t *screen = lv_obj_create(NULL);
    zmk_widget_bongo_init(&bongo_widget, screen);
    lv_obj_align(zmk_widget_bongo_obj(&bongo_widget), LV_ALIGN_TOP_LEFT, 0, 0);
    return screen;
}
