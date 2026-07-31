/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  MAC6100 / X6100 LVGL GUI — Bluetooth (BlueZ adapter power)
 */

#include "bluetooth.h"

#include "params/params.h"
#include "pubsub_ids.h"
#include "msg.h"

#include "lvgl/lvgl.h"
#include <gio/gio.h>
#include <stdio.h>

#define BLUEZ_BUS_NAME      "org.bluez"
#define ADAPTER_OBJECT_PATH "/org/bluez/hci0"
#define ADAPTER_INTERFACE   "org.bluez.Adapter1"
#define PROPS_INTERFACE     "org.freedesktop.DBus.Properties"

static bt_status_t status = BT_STATUS_UNAVAILABLE;

static void set_status(bt_status_t val) {
    if (status == val) {
        return;
    }
    status = val;
    lv_msg_send(MSG_BT_STATE_CHANGED, NULL);
}

static GDBusConnection *open_system_bus(GError **error) {
    return g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, error);
}

static bool adapter_get_powered(bool *powered, GError **error) {
    GDBusConnection *conn = open_system_bus(error);
    if (!conn) {
        return false;
    }

    GVariant *result = g_dbus_connection_call_sync(
        conn,
        BLUEZ_BUS_NAME,
        ADAPTER_OBJECT_PATH,
        PROPS_INTERFACE,
        "Get",
        g_variant_new("(ss)", ADAPTER_INTERFACE, "Powered"),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        error);

    g_object_unref(conn);

    if (!result) {
        return false;
    }

    GVariant *value = NULL;
    g_variant_get(result, "(v)", &value);
    *powered = g_variant_get_boolean(value);
    g_variant_unref(value);
    g_variant_unref(result);
    return true;
}

static bool adapter_set_powered(bool powered, GError **error) {
    GDBusConnection *conn = open_system_bus(error);
    if (!conn) {
        return false;
    }

    GVariant *result = g_dbus_connection_call_sync(
        conn,
        BLUEZ_BUS_NAME,
        ADAPTER_OBJECT_PATH,
        PROPS_INTERFACE,
        "Set",
        g_variant_new("(ssv)", ADAPTER_INTERFACE, "Powered", g_variant_new_boolean(powered)),
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        error);

    g_object_unref(conn);

    if (result) {
        g_variant_unref(result);
    }

    return result != NULL;
}

void bluetooth_refresh_status(void) {
    if (!params.wifi_enabled.x) {
        set_status(BT_STATUS_UNAVAILABLE);
        return;
    }

    GError *error = NULL;
    bool    powered = false;

    if (!adapter_get_powered(&powered, &error)) {
        if (error) {
            LV_LOG_WARN("Bluetooth status: %s", error->message);
            g_error_free(error);
        }
        set_status(BT_STATUS_UNAVAILABLE);
        return;
    }

    set_status(powered ? BT_STATUS_ON : BT_STATUS_OFF);
}

void bluetooth_power_setup(void) {
    if (!params.wifi_enabled.x) {
        status = BT_STATUS_UNAVAILABLE;
        return;
    }

    if (params.bt_enabled.x) {
        bluetooth_power_on();
    } else {
        bluetooth_refresh_status();
    }
}

bt_status_t bluetooth_get_status(void) {
    return status;
}

bool bluetooth_is_powered(void) {
    return status == BT_STATUS_ON;
}

void bluetooth_power_on(void) {
    if (!params.wifi_enabled.x) {
        msg_schedule_text_fmt("Turn WiFi radio on first");
        set_status(BT_STATUS_UNAVAILABLE);
        return;
    }

    GError *error = NULL;
    if (!adapter_set_powered(true, &error)) {
        if (error) {
            LV_LOG_ERROR("Bluetooth power on: %s", error->message);
            msg_schedule_text_fmt("Bluetooth power on failed");
            g_error_free(error);
        }
        bluetooth_refresh_status();
        return;
    }

    params_bool_set(&params.bt_enabled, true);
    set_status(BT_STATUS_ON);
}

void bluetooth_power_off(void) {
    if (!params.wifi_enabled.x) {
        params_bool_set(&params.bt_enabled, false);
        set_status(BT_STATUS_UNAVAILABLE);
        return;
    }

    GError *error = NULL;
    if (!adapter_set_powered(false, &error)) {
        if (error) {
            LV_LOG_ERROR("Bluetooth power off: %s", error->message);
            g_error_free(error);
        }
        bluetooth_refresh_status();
        return;
    }

    params_bool_set(&params.bt_enabled, false);
    set_status(BT_STATUS_OFF);
}
