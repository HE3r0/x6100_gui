/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  MAC6100 / X6100 LVGL GUI — Bluetooth (BlueZ adapter power)
 */

#pragma once

#include <stdbool.h>

typedef enum {
    BT_STATUS_UNAVAILABLE = 0,
    BT_STATUS_OFF,
    BT_STATUS_ON,
} bt_status_t;

void bluetooth_power_setup(void);

bt_status_t bluetooth_get_status(void);

bool bluetooth_is_powered(void);

void bluetooth_power_on(void);

void bluetooth_power_off(void);

void bluetooth_refresh_status(void);
