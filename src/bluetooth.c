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
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BLUEZ_BUS_NAME      "org.bluez"
#define ADAPTER_OBJECT_PATH "/org/bluez/hci0"
#define ADAPTER_INTERFACE   "org.bluez.Adapter1"
#define DEVICE_INTERFACE    "org.bluez.Device1"
#define PROPS_INTERFACE     "org.freedesktop.DBus.Properties"
#define OBJMGR_INTERFACE    "org.freedesktop.DBus.ObjectManager"

#define BT_SCAN_TIMEOUT_SEC 30

static bt_status_t      status = BT_STATUS_UNAVAILABLE;
static bool             scanning = false;
static time_t           scan_started_at = 0;
static bt_device_info_t devices[BT_MAX_DEVICES];
static size_t           device_count = 0;
static int              selected_index = -1;
static lv_timer_t      *restore_timer = NULL;

static bool power_on_internal(bool user_initiated);

static void set_status(bt_status_t val) {
    if (status == val) {
        return;
    }
    status = val;
    lv_msg_send(MSG_BT_STATE_CHANGED, NULL);
}

static void notify_devices_changed(void) {
    lv_msg_send(MSG_BT_DEVICES_CHANGED, NULL);
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

static bool adapter_get_discovering(bool *discovering, GError **error) {
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
        g_variant_new("(ss)", ADAPTER_INTERFACE, "Discovering"),
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
    *discovering = g_variant_get_boolean(value);
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

static bool adapter_call(const char *method, GError **error) {
    GDBusConnection *conn = open_system_bus(error);
    if (!conn) {
        return false;
    }

    GVariant *result = g_dbus_connection_call_sync(
        conn,
        BLUEZ_BUS_NAME,
        ADAPTER_OBJECT_PATH,
        ADAPTER_INTERFACE,
        method,
        NULL,
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        10000,
        NULL,
        error);

    g_object_unref(conn);

    if (!result) {
        return false;
    }

    g_variant_unref(result);
    return true;
}

static bool prop_dict_get_string(GVariant *props, const char *key, char *buf, size_t buf_size) {
    GVariant *value = g_variant_lookup_value(props, key, G_VARIANT_TYPE("s"));
    if (!value) {
        return false;
    }
    g_strlcpy(buf, g_variant_get_string(value, NULL), buf_size);
    g_variant_unref(value);
    return true;
}

static bool prop_dict_get_bool(GVariant *props, const char *key, bool *out) {
    GVariant *value = g_variant_lookup_value(props, key, G_VARIANT_TYPE("b"));
    if (!value) {
        return false;
    }
    *out = g_variant_get_boolean(value);
    g_variant_unref(value);
    return true;
}

static bool prop_dict_get_int16(GVariant *props, const char *key, int16_t *out) {
    GVariant *value = g_variant_lookup_value(props, key, G_VARIANT_TYPE("n"));
    if (!value) {
        return false;
    }
    *out = g_variant_get_int16(value);
    g_variant_unref(value);
    return true;
}

static int compare_devices(const void *a, const void *b) {
    const bt_device_info_t *da = a;
    const bt_device_info_t *db = b;

    if (da->connected != db->connected) {
        return db->connected - da->connected;
    }
    if (da->paired != db->paired) {
        return db->paired - da->paired;
    }
    return db->rssi - da->rssi;
}

static void clear_devices(void) {
    device_count = 0;
    selected_index = -1;
    notify_devices_changed();
}

static void restore_timer_cb(lv_timer_t *timer) {
    (void)timer;
    restore_timer = NULL;

    if (!params.wifi_enabled.x || !params.bt_enabled.x) {
        return;
    }

    if (bluetooth_is_powered()) {
        return;
    }

    power_on_internal(false);
}

void bluetooth_schedule_restore(void) {
    if (!params.wifi_enabled.x || !params.bt_enabled.x) {
        return;
    }

    if (restore_timer) {
        lv_timer_del(restore_timer);
    }

    restore_timer = lv_timer_create(restore_timer_cb, 2000, NULL);
    lv_timer_set_repeat_count(restore_timer, 1);
}

void bluetooth_cancel_restore(void) {
    if (restore_timer) {
        lv_timer_del(restore_timer);
        restore_timer = NULL;
    }
}

static void parse_managed_objects(GVariant *result) {
    GVariantIter *objects_iter = NULL;
    const char   *path = NULL;
    GVariant     *interfaces = NULL;

    device_count = 0;
    g_variant_get(result, "(a{oa{sa{sv}}})", &objects_iter);

    /* Use g_variant_iter_next (not loop): we own the returned interfaces variant. */
    while (device_count < BT_MAX_DEVICES &&
           g_variant_iter_next(objects_iter, "{&o@a{sa{sv}}}", &path, &interfaces)) {
        GVariant *props = g_variant_lookup_value(interfaces, DEVICE_INTERFACE, G_VARIANT_TYPE("a{sv}"));
        g_variant_unref(interfaces);
        interfaces = NULL;

        if (!props) {
            continue;
        }

        bt_device_info_t *dev = &devices[device_count];
        memset(dev, 0, sizeof(*dev));
        g_strlcpy(dev->path, path, sizeof(dev->path));

        if (!prop_dict_get_string(props, "Alias", dev->name, sizeof(dev->name)) &&
            !prop_dict_get_string(props, "Name", dev->name, sizeof(dev->name))) {
            prop_dict_get_string(props, "Address", dev->name, sizeof(dev->name));
        }

        prop_dict_get_string(props, "Address", dev->address, sizeof(dev->address));
        prop_dict_get_bool(props, "Paired", &dev->paired);
        prop_dict_get_bool(props, "Connected", &dev->connected);
        prop_dict_get_int16(props, "RSSI", &dev->rssi);

        g_variant_unref(props);
        device_count++;
    }

    g_variant_iter_free(objects_iter);

    if (device_count > 1) {
        qsort(devices, device_count, sizeof(devices[0]), compare_devices);
    }

    if (selected_index >= (int)device_count) {
        selected_index = device_count > 0 ? 0 : -1;
    }
}

static void refresh_discovering_state(void) {
    GError *error = NULL;
    bool    discovering = false;

    if (!bluetooth_is_powered()) {
        scanning = false;
        scan_started_at = 0;
        return;
    }

    if (adapter_get_discovering(&discovering, &error)) {
        scanning = discovering;
        if (!discovering) {
            scan_started_at = 0;
        }
    } else if (error) {
        g_error_free(error);
    }
}

static void maybe_stop_scan_timeout(void) {
    if (!scanning || scan_started_at == 0) {
        return;
    }

    if ((time(NULL) - scan_started_at) >= BT_SCAN_TIMEOUT_SEC) {
        GError *error = NULL;
        if (!adapter_call("StopDiscovery", &error)) {
            if (error) {
                LV_LOG_WARN("Bluetooth scan timeout stop: %s", error->message);
                g_error_free(error);
            }
        }
        scanning = false;
        scan_started_at = 0;
    }
}

void bluetooth_refresh_devices(void) {
    maybe_stop_scan_timeout();

    if (!bluetooth_is_powered()) {
        clear_devices();
        return;
    }

    refresh_discovering_state();

    GError          *error = NULL;
    GDBusConnection *conn = open_system_bus(&error);
    if (!conn) {
        if (error) {
            LV_LOG_WARN("Bluetooth devices: %s", error->message);
            g_error_free(error);
        }
        return;
    }

    GVariant *result = g_dbus_connection_call_sync(
        conn,
        BLUEZ_BUS_NAME,
        "/",
        OBJMGR_INTERFACE,
        "GetManagedObjects",
        NULL,
        G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);

    g_object_unref(conn);

    if (!result) {
        if (error) {
            LV_LOG_WARN("Bluetooth GetManagedObjects: %s", error->message);
            g_error_free(error);
        }
        return;
    }

    parse_managed_objects(result);
    g_variant_unref(result);
    notify_devices_changed();
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

    bluetooth_refresh_status();

    if (params.bt_enabled.x && status != BT_STATUS_ON) {
        bluetooth_schedule_restore();
    }
}

bt_status_t bluetooth_get_status(void) {
    return status;
}

bool bluetooth_is_powered(void) {
    return status == BT_STATUS_ON;
}

static bool power_on_internal(bool user_initiated) {
    if (!params.wifi_enabled.x) {
        if (user_initiated) {
            msg_schedule_text_fmt("Turn WiFi radio on first");
        }
        set_status(BT_STATUS_UNAVAILABLE);
        return false;
    }

    GError *error = NULL;
    if (!adapter_set_powered(true, &error)) {
        if (error) {
            LV_LOG_WARN("Bluetooth power on: %s", error->message);
            if (user_initiated) {
                msg_schedule_text_fmt("Bluetooth power on failed");
            }
            g_error_free(error);
        }
        bluetooth_refresh_status();
        return false;
    }

    params_bool_set(&params.bt_enabled, true);
    set_status(BT_STATUS_ON);
    return true;
}

void bluetooth_power_on(void) {
    bluetooth_cancel_restore();
    power_on_internal(true);
}

void bluetooth_power_off(void) {
    bluetooth_cancel_restore();
    bluetooth_stop_scan();
    clear_devices();

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

void bluetooth_start_scan(void) {
    if (!bluetooth_is_powered()) {
        msg_schedule_text_fmt("Turn Bluetooth on first");
        return;
    }

    if (scanning) {
        return;
    }

    GError *error = NULL;
    if (!adapter_call("StartDiscovery", &error)) {
        if (error) {
            LV_LOG_ERROR("Bluetooth StartDiscovery: %s", error->message);
            msg_schedule_text_fmt("Bluetooth scan failed");
            g_error_free(error);
        }
        return;
    }

    scanning = true;
    scan_started_at = time(NULL);
    bluetooth_refresh_devices();
}

void bluetooth_stop_scan(void) {
    if (!bluetooth_is_powered()) {
        scanning = false;
        scan_started_at = 0;
        return;
    }

    GError *error = NULL;
    if (!adapter_call("StopDiscovery", &error)) {
        if (error) {
            LV_LOG_WARN("Bluetooth StopDiscovery: %s", error->message);
            g_error_free(error);
        }
    }

    scanning = false;
    scan_started_at = 0;
    refresh_discovering_state();
    notify_devices_changed();
}

bool bluetooth_scanning(void) {
    return scanning;
}

size_t bluetooth_device_count(void) {
    return device_count;
}

const bt_device_info_t *bluetooth_get_device(size_t index) {
    if (index >= device_count) {
        return NULL;
    }
    return &devices[index];
}

const bt_device_info_t *bluetooth_get_selected_device(void) {
    if (selected_index < 0 || (size_t)selected_index >= device_count) {
        return NULL;
    }
    return &devices[selected_index];
}

void bluetooth_set_selected_index(int index) {
    if (index < 0 || (size_t)index >= device_count) {
        selected_index = -1;
    } else {
        selected_index = index;
    }
}
