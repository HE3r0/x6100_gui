/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  MAC6100 / X6100 LVGL GUI — Bluetooth settings
 */

#include "dialog_bluetooth.h"

#include "bluetooth.h"
#include "buttons.h"
#include "msg.h"
#include "params/params.h"
#include "pubsub_ids.h"

#include "lvgl/lvgl.h"

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);

static void bt_power_toggle_cb(button_data_t *btn_data);
static const char *bt_on_off_label_getter(void);
static void bt_state_changed_cb(void *s, lv_msg_t *m);
static void update_status_label(void);

static button_data_t btn_on_off = {
    .type     = BTN_TEXT_FN,
    .label_fn = bt_on_off_label_getter,
    .press    = bt_power_toggle_cb,
};

static buttons_page_t btn_page = {
    {&btn_on_off}
};

static lv_obj_t *label_status;
static void     *subscription = NULL;

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
    lv_obj_set_size(cont, 500, 200);
    lv_obj_center(cont);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 20, 0);

    lv_obj_t *title = lv_label_create(cont);
    lv_label_set_text(title, "Bluetooth");

    label_status = lv_label_create(cont);
    lv_obj_set_style_text_line_space(label_status, 8, 0);
    update_status_label();

    subscription = lv_msg_subscribe(MSG_BT_STATE_CHANGED, bt_state_changed_cb, NULL);
    lv_msg_send(MSG_BT_STATE_CHANGED, NULL);
}

static void destruct_cb(void) {
    if (subscription) {
        lv_msg_unsubscribe(subscription);
        subscription = NULL;
    }
}

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    if (key == LV_KEY_ESC) {
        dialog_destruct();
    }
}

static void bt_power_toggle_cb(button_data_t *btn_data) {
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
}

static void bt_state_changed_cb(void *s, lv_msg_t *m) {
    (void)s;
    (void)m;

    bluetooth_refresh_status();
    update_status_label();

    buttons_page_t *page = buttons_get_cur_page();
    if (page) {
        for (size_t i = 0; i < BUTTONS; i++) {
            if (page->items[i]) {
                buttons_refresh(page->items[i]);
            }
        }
    }
}
