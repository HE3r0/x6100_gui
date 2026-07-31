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

#define BLUEZ_BUS_NAME      "org.bluez"
#define ADAPTER_OBJECT_PATH "/org/bluez/hci0"
#define ADAPTER_INTERFACE   "org.bluez.Adapter1"
#define DEVICE_INTERFACE    "org.bluez.Device1"
#define PROPS_INTERFACE     "org.freedesktop.DBus.Properties"
#define OBJMGR_INTERFACE    "org.freedesktop.DBus.ObjectManager"

#define BT_SCAN_TIMEOUT_MS   30000
#define BT_SCAN_POLL_MS      1000

static bt_status_t      status = BT_STATUS_UNAVAILABLE;
static bool             scanning = false;
static uint32_t         scan_started_tick = 0;
static bt_device_info_t devices[BT_MAX_DEVICES];
static size_t           device_count = 0;
static int              selected_index = -1;
static lv_timer_t      *restore_timer = NULL;
static lv_timer_t      *scan_timer = NULL;
static bool             scan_stop_requested = false;
static bool             scan_start_requested = false;
static bool             dbus_busy = false;

static bool power_on_internal(bool user_initiated);
static void scan_timer_cb(lv_timer_t *timer);
static void ensure_scan_timer(void);
static void stop_scan_timer(void);
static bool discovery_start(void);
static bool discovery_stop(void);
static void refresh_devices_internal(void);

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

static bool dbus_error_is_in_progress(GError *error) {
    if (!error || !error->message) {
        return false;
    }
    return strstr(error->message, "InProgress") != NULL ||
           strstr(error->message, "in progress") != NULL;
}

static bool adapter_get_bool_prop(const char *prop, bool *out, GError **error) {
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
        g_variant_new("(ss)", ADAPTER_INTERFACE, prop),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        2000,
        NULL,
        error);

    g_object_unref(conn);

    if (!result) {
        return false;
    }

    GVariant *value = NULL;
    g_variant_get(result, "(v)", &value);
    *out = g_variant_get_boolean(value);
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
        3000,
        NULL,
        error);

    g_object_unref(conn);

    if (result) {
        g_variant_unref(result);
        return true;
    }
    return false;
}

static bool adapter_call(const char *method, GError **error) {
    GDBusConnection *conn = open_system_bus(error);
    if (!conn) {
        return false;
    }

    GError   *local_error = NULL;
    GVariant *result = g_dbus_connection_call_sync(
        conn,
        BLUEZ_BUS_NAME,
        ADAPTER_OBJECT_PATH,
        ADAPTER_INTERFACE,
        method,
        NULL,
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        3000,
        NULL,
        &local_error);

    g_object_unref(conn);

    if (result) {
        g_variant_unref(result);
        return true;
    }

    if (dbus_error_is_in_progress(local_error)) {
        g_error_free(local_error);
        return true;
    }

    if (error) {
        *error = local_error;
    } else if (local_error) {
        g_error_free(local_error);
    }
    return false;
}

static bool adapter_set_discovery_filter(void) {
    GError          *error = NULL;
    GDBusConnection *conn = open_system_bus(&error);
    if (!conn) {
        if (error) {
            LV_LOG_WARN("Bluetooth filter bus: %s", error->message);
            g_error_free(error);
        }
        return false;
    }

    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&builder, "{sv}", "Transport", g_variant_new_string("auto"));
    g_variant_builder_add(&builder, "{sv}", "DuplicateData", g_variant_new_boolean(FALSE));

    GVariant *result = g_dbus_connection_call_sync(
        conn,
        BLUEZ_BUS_NAME,
        ADAPTER_OBJECT_PATH,
        ADAPTER_INTERFACE,
        "SetDiscoveryFilter",
        g_variant_new("(a{sv})", &builder),
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        3000,
        NULL,
        &error);

    g_object_unref(conn);

    if (result) {
        g_variant_unref(result);
        return true;
    }

    if (error) {
        /* Filter is optional — older stacks may lack it. */
        LV_LOG_WARN("Bluetooth SetDiscoveryFilter: %s", error->message);
        g_error_free(error);
    }
    return false;
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
    GVariant *value = g_variant_lookup_value(props, key, NULL);
    if (!value) {
        return false;
    }

    if (g_variant_is_of_type(value, G_VARIANT_TYPE_INT16)) {
        *out = g_variant_get_int16(value);
    } else if (g_variant_is_of_type(value, G_VARIANT_TYPE_INT32)) {
        *out = (int16_t)g_variant_get_int32(value);
    } else {
        g_variant_unref(value);
        return false;
    }

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
    bool had_devices = device_count > 0;
    device_count = 0;
    selected_index = -1;
    if (had_devices) {
        notify_devices_changed();
    }
}

static void sanitize_name(char *name, size_t size) {
    if (!name || size == 0) {
        return;
    }

    for (size_t i = 0; name[i] != '\0' && i < size; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7f) {
            name[i] = '?';
        }
    }

    if (name[0] == '\0') {
        g_strlcpy(name, "Unknown", size);
    }
}

static void parse_device_dict(GVariant *objects) {
    GVariantIter iter;
    const char  *path = NULL;
    GVariant    *ifaces = NULL;

    device_count = 0;
    g_variant_iter_init(&iter, objects);

    while (device_count < BT_MAX_DEVICES &&
           g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &ifaces)) {
        GVariant *props = g_variant_lookup_value(ifaces, DEVICE_INTERFACE, G_VARIANT_TYPE("a{sv}"));
        g_variant_unref(ifaces);
        ifaces = NULL;

        if (!props) {
            continue;
        }

        bt_device_info_t *dev = &devices[device_count];
        memset(dev, 0, sizeof(*dev));

        if (path) {
            g_strlcpy(dev->path, path, sizeof(dev->path));
        }

        if (!prop_dict_get_string(props, "Alias", dev->name, sizeof(dev->name)) &&
            !prop_dict_get_string(props, "Name", dev->name, sizeof(dev->name))) {
            prop_dict_get_string(props, "Address", dev->name, sizeof(dev->name));
        }
        sanitize_name(dev->name, sizeof(dev->name));

        prop_dict_get_string(props, "Address", dev->address, sizeof(dev->address));
        prop_dict_get_bool(props, "Paired", &dev->paired);
        prop_dict_get_bool(props, "Connected", &dev->connected);
        prop_dict_get_int16(props, "RSSI", &dev->rssi);

        g_variant_unref(props);
        device_count++;
    }

    if (device_count > 1) {
        qsort(devices, device_count, sizeof(devices[0]), compare_devices);
    }

    if (selected_index >= (int)device_count) {
        selected_index = device_count > 0 ? 0 : -1;
    }
}

static void parse_managed_objects(GVariant *result) {
    if (g_variant_is_of_type(result, G_VARIANT_TYPE("a{oa{sa{sv}}}"))) {
        parse_device_dict(result);
        return;
    }

    if (g_variant_is_of_type(result, G_VARIANT_TYPE("(a{oa{sa{sv}}})"))) {
        GVariant *objects = g_variant_get_child_value(result, 0);
        parse_device_dict(objects);
        g_variant_unref(objects);
        return;
    }

    LV_LOG_WARN("Bluetooth: unexpected GetManagedObjects type %s",
                g_variant_get_type_string(result));
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

static bool discovery_start(void) {
    GError *error = NULL;

    adapter_set_discovery_filter();

    if (!adapter_call("StartDiscovery", &error)) {
        if (error) {
            LV_LOG_ERROR("Bluetooth StartDiscovery: %s", error->message);
            msg_schedule_text_fmt("BT scan failed");
            g_error_free(error);
        }
        return false;
    }
    return true;
}

static bool discovery_stop(void) {
    GError *error = NULL;
    if (!adapter_call("StopDiscovery", &error)) {
        if (error) {
            LV_LOG_WARN("Bluetooth StopDiscovery: %s", error->message);
            g_error_free(error);
        }
        return false;
    }
    return true;
}

static void refresh_devices_internal(void) {
    GError          *error = NULL;
    GDBusConnection *conn = open_system_bus(&error);
    if (!conn) {
        if (error) {
            LV_LOG_WARN("Bluetooth devices bus: %s", error->message);
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
        G_VARIANT_TYPE("a{oa{sa{sv}}}"),
        G_DBUS_CALL_FLAGS_NONE,
        2500,
        NULL,
        &error);

    if (!result) {
        if (error) {
            g_error_free(error);
            error = NULL;
        }
        result = g_dbus_connection_call_sync(
            conn,
            BLUEZ_BUS_NAME,
            "/org/bluez",
            OBJMGR_INTERFACE,
            "GetManagedObjects",
            NULL,
            G_VARIANT_TYPE("a{oa{sa{sv}}}"),
            G_DBUS_CALL_FLAGS_NONE,
            2500,
            NULL,
            &error);
    }

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

static void finish_scan(const char *msg) {
    if (scanning && bluetooth_is_powered()) {
        discovery_stop();
    }

    scanning = false;
    scan_started_tick = 0;
    scan_start_requested = false;
    scan_stop_requested = false;

    /* Never delete the timer from inside its callback — just pause it. */
    if (scan_timer) {
        lv_timer_pause(scan_timer);
    }

    if (msg) {
        msg_schedule_text_fmt("%s", msg);
    }
    notify_devices_changed();
    lv_msg_send(MSG_BT_STATE_CHANGED, NULL);
}

static void ensure_scan_timer(void) {
    if (!scan_timer) {
        scan_timer = lv_timer_create(scan_timer_cb, BT_SCAN_POLL_MS, NULL);
    } else {
        lv_timer_resume(scan_timer);
    }
}

static void stop_scan_timer(void) {
    if (scan_timer) {
        lv_timer_del(scan_timer);
        scan_timer = NULL;
    }
}

static void scan_timer_cb(lv_timer_t *timer) {
    (void)timer;

    if (dbus_busy) {
        return;
    }
    dbus_busy = true;

    if (scan_stop_requested) {
        finish_scan("Scan stopped");
        dbus_busy = false;
        return;
    }

    if (scan_start_requested) {
        scan_start_requested = false;

        if (!bluetooth_is_powered()) {
            finish_scan(NULL);
            msg_schedule_text_fmt("Turn Bluetooth on first");
            dbus_busy = false;
            return;
        }

        if (!discovery_start()) {
            finish_scan(NULL);
            dbus_busy = false;
            return;
        }

        scanning = true;
        scan_started_tick = lv_tick_get();
        notify_devices_changed();
        lv_msg_send(MSG_BT_STATE_CHANGED, NULL);
        refresh_devices_internal();
        dbus_busy = false;
        return;
    }

    if (!scanning) {
        if (scan_timer) {
            lv_timer_pause(scan_timer);
        }
        dbus_busy = false;
        return;
    }

    if (!bluetooth_is_powered()) {
        finish_scan(NULL);
        dbus_busy = false;
        return;
    }

    if (lv_tick_elaps(scan_started_tick) >= BT_SCAN_TIMEOUT_MS) {
        finish_scan("Scan timeout");
        dbus_busy = false;
        return;
    }

    refresh_devices_internal();
    dbus_busy = false;
}

void bluetooth_refresh_devices(void) {
    if (dbus_busy || !bluetooth_is_powered()) {
        return;
    }
    dbus_busy = true;
    refresh_devices_internal();
    dbus_busy = false;
}

void bluetooth_refresh_status(void) {
    if (!params.wifi_enabled.x) {
        set_status(BT_STATUS_UNAVAILABLE);
        return;
    }

    GError *error = NULL;
    bool    powered = false;

    if (!adapter_get_bool_prop("Powered", &powered, &error)) {
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
    scan_start_requested = false;
    scan_stop_requested = false;
    if (scanning && status == BT_STATUS_ON) {
        discovery_stop();
    }
    scanning = false;
    scan_started_tick = 0;
    stop_scan_timer();
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

    if (scanning || scan_start_requested) {
        return;
    }

    scan_stop_requested = false;
    scan_start_requested = true;
    ensure_scan_timer();
    if (scan_timer) {
        lv_timer_ready(scan_timer);
    }
    notify_devices_changed();
    lv_msg_send(MSG_BT_STATE_CHANGED, NULL);
}

void bluetooth_stop_scan(void) {
    scan_start_requested = false;

    if (dbus_busy) {
        scan_stop_requested = true;
        ensure_scan_timer();
        if (scan_timer) {
            lv_timer_ready(scan_timer);
        }
        return;
    }

    if (!scanning) {
        if (scan_timer) {
            lv_timer_pause(scan_timer);
        }
        return;
    }

    dbus_busy = true;
    finish_scan("Scan stopped");
    dbus_busy = false;
}

bool bluetooth_scanning(void) {
    return scanning || scan_start_requested;
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
