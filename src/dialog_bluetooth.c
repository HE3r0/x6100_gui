/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  MAC6100 / X6100 LVGL GUI — Bluetooth settings
 */

#include "dialog_bluetooth.h"

#include "bluetooth.h"
#include "buttons.h"
#include "keyboard.h"
#include "msg.h"
#include "params/params.h"
#include "pubsub_ids.h"

#include "lvgl/lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIALOG_WIDTH 775
#define DIALOG_HEIGHT 320
#define PARAMS_WIDTH 300

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);
static void device_selected_cb(lv_event_t *e);

static void bt_power_toggle_cb(button_data_t *btn_data);
static void start_scan_cb(button_data_t *btn_data);
static const char *bt_on_off_label_getter(void);
static const char *bt_scan_label_getter(void);
static void bt_state_changed_cb(void *s, lv_msg_t *m);
static void bt_devices_changed_cb(void *s, lv_msg_t *m);
static void update_status_label(void);
static void update_devices_table(void);
static void clear_devices_table(void);
static void start_refresh_devices(void);
static void stop_refresh_devices(void);
static void update_devices_table_cb(lv_timer_t *timer);

static button_data_t btn_on_off = {
    .type     = BTN_TEXT_FN,
    .label_fn = bt_on_off_label_getter,
    .press    = bt_power_toggle_cb,
};

static button_data_t btn_scan = {
    .type     = BTN_TEXT_FN,
    .label_fn = bt_scan_label_getter,
    .press    = start_scan_cb,
};

static buttons_page_t btn_page = {
    {&btn_on_off, &btn_scan}
};

static lv_obj_t *label_status;
static lv_obj_t *label_scan;
static lv_obj_t *device_table;
static lv_timer_t *timer_refresh_devices = NULL;
static void       *subscription_state = NULL;
static void       *subscription_devices = NULL;

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .btn_page     = &btn_page,
    .audio_cb     = NULL,
    .key_cb       = key_cb,
};

dialog_t *dialog_bluetooth = &dialog;

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);

    lv_obj_t *cont = lv_obj_create(dialog.obj);
    lv_obj_remove_style(cont, NULL, LV_STATE_ANY | LV_PART_MAIN);
    lv_obj_set_size(cont, DIALOG_WIDTH, DIALOG_HEIGHT);
    lv_obj_center(cont);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW);

    lv_obj_t *param_cont = lv_obj_create(cont);
    lv_obj_remove_style(param_cont, NULL, LV_STATE_ANY | LV_PART_MAIN);
    lv_obj_set_size(param_cont, PARAMS_WIDTH, DIALOG_HEIGHT);
    lv_obj_set_flex_flow(param_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(param_cont, 20, 0);

    lv_obj_t *title = lv_label_create(param_cont);
    lv_label_set_text(title, "Bluetooth");

    lv_obj_t *label = lv_label_create(param_cont);
    lv_label_set_text(label, "Status:");
    label_status = lv_label_create(param_cont);
    lv_obj_set_style_text_line_space(label_status, 8, 0);
    update_status_label();

    label = lv_label_create(param_cont);
    lv_label_set_text(label, "Scan:");
    label_scan = lv_label_create(param_cont);
    lv_label_set_text(label_scan, "Idle");

    device_table = lv_table_create(cont);
    lv_table_set_col_cnt(device_table, 1);
    lv_table_set_col_width(device_table, 0, DIALOG_WIDTH - PARAMS_WIDTH - 2);
    lv_obj_set_height(device_table, DIALOG_HEIGHT);
    lv_obj_center(device_table);
    lv_obj_set_flex_grow(device_table, 1);

    lv_obj_remove_style(device_table, NULL, LV_PART_MAIN | LV_PART_ITEMS | LV_STATE_ANY);
    lv_obj_set_style_bg_opa(device_table, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_bg_color(device_table, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(device_table, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(device_table, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_opa(device_table, 128, LV_PART_MAIN);

    lv_obj_set_style_border_width(device_table, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_color(device_table, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(device_table, lv_color_white(), LV_PART_ITEMS | LV_STATE_EDITED);
    lv_obj_set_style_bg_opa(device_table, LV_OPA_30, LV_PART_ITEMS | LV_STATE_EDITED);
    lv_obj_set_style_pad_top(device_table, 3, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(device_table, 3, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(device_table, 5, LV_PART_ITEMS);
    lv_obj_set_style_pad_right(device_table, 0, LV_PART_ITEMS);

    lv_obj_add_event_cb(device_table, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(device_table, device_selected_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_group_add_obj(keyboard_group, device_table);
    lv_group_set_editing(keyboard_group, true);

    clear_devices_table();
    start_refresh_devices();

    subscription_state = lv_msg_subscribe(MSG_BT_STATE_CHANGED, bt_state_changed_cb, NULL);
    subscription_devices = lv_msg_subscribe(MSG_BT_DEVICES_CHANGED, bt_devices_changed_cb, NULL);
    update_status_label();
    update_devices_table();
}

static void destruct_cb(void) {
    bluetooth_stop_scan();
    stop_refresh_devices();
    clear_devices_table();

    if (subscription_state) {
        lv_msg_unsubscribe(subscription_state);
        subscription_state = NULL;
    }
    if (subscription_devices) {
        lv_msg_unsubscribe(subscription_devices);
        subscription_devices = NULL;
    }
}

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    if (key == LV_KEY_ESC) {
        dialog_destruct();
    }
}

static void device_selected_cb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    uint16_t  row;
    uint16_t  col;

    lv_table_get_selected_cell(obj, &row, &col);
    if ((row == LV_TABLE_CELL_NONE) || (col == LV_TABLE_CELL_NONE)) {
        bluetooth_set_selected_index(-1);
        return;
    }

    const char *user_data = (const char *)lv_table_get_cell_user_data(obj, row, col);
    if (!user_data || user_data[0] == '\0') {
        bluetooth_set_selected_index(-1);
        return;
    }

    char *end = NULL;
    long  index = strtol(user_data, &end, 10);
    if (end == user_data || (end && *end != '\0')) {
        bluetooth_set_selected_index(-1);
        return;
    }

    bluetooth_set_selected_index((int)index);
}

static void bt_power_toggle_cb(button_data_t *btn_data) {
    (void)btn_data;

    if (!params.wifi_enabled.x) {
        msg_update_text_fmt("Turn WiFi radio on first");
        return;
    }

    if (bluetooth_is_powered()) {
        bluetooth_power_off();
        msg_update_text_fmt("Bluetooth off");
    } else {
        bluetooth_power_on();
        msg_update_text_fmt("Bluetooth on");
    }
}

static void start_scan_cb(button_data_t *btn_data) {
    (void)btn_data;

    if (!bluetooth_is_powered()) {
        msg_update_text_fmt("Turn Bluetooth on first");
        return;
    }

    if (bluetooth_scanning()) {
        msg_update_text_fmt("Stop scan");
        bluetooth_stop_scan();
    } else {
        msg_update_text_fmt("Start scan");
        bluetooth_start_scan();
    }
}

static const char *bt_on_off_label_getter(void) {
    switch (bluetooth_get_status()) {
    case BT_STATUS_UNAVAILABLE:
        return "Bluetooth:\nUnavailable";
    case BT_STATUS_OFF:
        return "Bluetooth:\nOff";
    default:
        return "Bluetooth:\nOn";
    }
}

static const char *bt_scan_label_getter(void) {
    if (!bluetooth_is_powered()) {
        return "Scan";
    }
    return bluetooth_scanning() ? "Scanning..." : "Scan";
}

static void update_status_label(void) {
    const char *text;

    if (!params.wifi_enabled.x) {
        text = "Radio module off.\nTurn WiFi on to use Bluetooth.";
    } else {
        switch (bluetooth_get_status()) {
        case BT_STATUS_ON:
            text = "Adapter powered on.";
            break;
        case BT_STATUS_OFF:
            text = "Adapter powered off.";
            break;
        default:
            text = "Adapter unavailable.";
            break;
        }
    }

    lv_label_set_text(label_status, text);

    if (label_scan) {
        if (!bluetooth_is_powered()) {
            lv_label_set_text(label_scan, "Idle");
        } else if (bluetooth_scanning()) {
            lv_label_set_text(label_scan, "Scanning...");
        } else {
            lv_label_set_text_fmt(label_scan, "%u device(s)", (unsigned)bluetooth_device_count());
        }
    }
}

static void clear_devices_table(void) {
    if (!device_table) {
        return;
    }

    uint16_t rows = lv_table_get_row_cnt(device_table);
    for (uint16_t row = 0; row < rows; row++) {
        void *user_data = lv_table_get_cell_user_data(device_table, row, 0);
        if (user_data) {
            free(user_data);
            lv_table_set_cell_user_data(device_table, row, 0, NULL);
        }
    }

    lv_table_set_cell_value(device_table, 0, 0, "");
    lv_table_set_row_cnt(device_table, 1);
    lv_event_send(device_table, LV_EVENT_VALUE_CHANGED, NULL);
}

static void update_devices_table(void) {
    if (!device_table) {
        return;
    }

    clear_devices_table();

    size_t count = bluetooth_device_count();
    if (count == 0) {
        lv_table_set_cell_value(device_table, 0, 0, bluetooth_scanning() ? "Scanning..." : "No devices");
        return;
    }

    for (size_t i = 0; i < count; i++) {
        const bt_device_info_t *dev = bluetooth_get_device(i);
        char                    suffix[16] = "";
        char                   *index_str = NULL;

        if (dev->connected) {
            snprintf(suffix, sizeof(suffix), " [C]");
        } else if (dev->paired) {
            snprintf(suffix, sizeof(suffix), " [P]");
        }

        if (dev->rssi != 0) {
            lv_table_set_cell_value_fmt(device_table, i, 0, "%s  %d dBm%s", dev->name, dev->rssi, suffix);
        } else {
            lv_table_set_cell_value_fmt(device_table, i, 0, "%s%s", dev->name, suffix);
        }

        if (asprintf(&index_str, "%zu", i) >= 0) {
            lv_table_set_cell_user_data(device_table, i, 0, index_str);
        }
    }

    lv_table_set_row_cnt(device_table, count);
    lv_event_send(device_table, LV_EVENT_VALUE_CHANGED, NULL);
}

static void refresh_buttons(void) {
    buttons_page_t *page = buttons_get_cur_page();
    if (!page) {
        return;
    }

    for (size_t i = 0; i < BUTTONS; i++) {
        if (page->items[i]) {
            buttons_refresh(page->items[i]);
        }
    }
}

static void bt_state_changed_cb(void *s, lv_msg_t *m) {
    (void)s;
    (void)m;

    update_status_label();

    if (!bluetooth_is_powered()) {
        bluetooth_stop_scan();
        update_devices_table();
    } else if (device_table) {
        bluetooth_refresh_devices();
        update_devices_table();
    }

    refresh_buttons();
}

static void bt_devices_changed_cb(void *s, lv_msg_t *m) {
    (void)s;
    (void)m;

    update_status_label();
    update_devices_table();
    refresh_buttons();
}

static void start_refresh_devices(void) {
    if (!timer_refresh_devices) {
        timer_refresh_devices = lv_timer_create(update_devices_table_cb, 1000, NULL);
    }
}

static void stop_refresh_devices(void) {
    if (timer_refresh_devices) {
        lv_timer_del(timer_refresh_devices);
        timer_refresh_devices = NULL;
    }
}

static void update_devices_table_cb(lv_timer_t *timer) {
    (void)timer;

    if (!bluetooth_is_powered()) {
        return;
    }

    bluetooth_refresh_devices();
    update_status_label();
    update_devices_table();
    refresh_buttons();
}
