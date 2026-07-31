/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  MAC6100 / X6100 LVGL GUI — Bluetooth (BlueZ adapter power)
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    BT_STATUS_UNAVAILABLE = 0,
    BT_STATUS_OFF,
    BT_STATUS_ON,
} bt_status_t;

#define BT_MAX_DEVICES 32

typedef struct {
    char path[128];
    char name[64];
    char address[18];
    int16_t rssi;
    bool    paired;
    bool    connected;
} bt_device_info_t;

void bluetooth_power_setup(void);

bt_status_t bluetooth_get_status(void);

bool bluetooth_is_powered(void);

void bluetooth_power_on(void);

void bluetooth_power_off(void);

void bluetooth_refresh_status(void);

void bluetooth_start_scan(void);

void bluetooth_stop_scan(void);

bool bluetooth_scanning(void);

void bluetooth_refresh_devices(void);

size_t bluetooth_device_count(void);

const bt_device_info_t *bluetooth_get_device(size_t index);

const bt_device_info_t *bluetooth_get_selected_device(void);

void bluetooth_set_selected_index(int index);
