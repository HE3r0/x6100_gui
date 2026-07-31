/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  MAC6100 / X6100 LVGL GUI — Bluetooth
 *
 *  All BlueZ D-Bus work runs asynchronously on the default GLib main
 *  context (already pumped by wifi.cpp). No worker threads, no system().
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

#define BT_SCAN_TIMEOUT_MS 30000
#define BT_SCAN_POLL_MS    1000
#define BT_LOG_PATH        "/tmp/bt.log"

static bt_status_t      status = BT_STATUS_UNAVAILABLE;
static bt_device_info_t devices[BT_MAX_DEVICES];
static size_t           device_count = 0;
static int              selected_index = -1;

static GDBusConnection *bus = NULL;
static bool             scanning = false;
static bool             scan_busy = false;
static uint32_t         scan_started_tick = 0;
static lv_timer_t      *scan_timer = NULL;
static lv_timer_t      *restore_timer = NULL;

static bool power_on_internal(bool user_initiated);
static void ensure_bus(void);
static void bt_log(const char *msg);
static void notify_ui(void);
static void refresh_devices_async(void);
static void stop_discovery_async(const char *reason);
static void scan_timer_cb(lv_timer_t *timer);

static void set_status(bt_status_t val) {
    if (status == val) {
        return;
    }
    status = val;
    lv_msg_send(MSG_BT_STATE_CHANGED, NULL);
}

static void notify_ui(void) {
    lv_msg_send(MSG_BT_DEVICES_CHANGED, NULL);
    lv_msg_send(MSG_BT_STATE_CHANGED, NULL);
}

static void bt_log(const char *msg) {
    FILE *fp = fopen(BT_LOG_PATH, "a");
    if (!fp) {
        return;
    }
    fprintf(fp, "%u %s\n", (unsigned)lv_tick_get(), msg ? msg : "");
    fclose(fp);
}

static void ensure_bus(void) {
    GError *error = NULL;

    if (bus) {
        return;
    }

    bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!bus) {
        bt_log(error ? error->message : "bus get failed");
        if (error) {
            g_error_free(error);
        }
    }
}

static bool adapter_get_bool_prop(const char *prop, bool *out, GError **error) {
    ensure_bus();
    if (!bus) {
        return false;
    }

    GVariant *result = g_dbus_connection_call_sync(
        bus, BLUEZ_BUS_NAME, ADAPTER_OBJECT_PATH, PROPS_INTERFACE, "Get",
        g_variant_new("(ss)", ADAPTER_INTERFACE, prop), G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, error);

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
    ensure_bus();
    if (!bus) {
        return false;
    }

    GVariant *result = g_dbus_connection_call_sync(
        bus, BLUEZ_BUS_NAME, ADAPTER_OBJECT_PATH, PROPS_INTERFACE, "Set",
        g_variant_new("(ssv)", ADAPTER_INTERFACE, "Powered", g_variant_new_boolean(powered)),
        NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, error);

    if (result) {
        g_variant_unref(result);
        return true;
    }
    return false;
}

static void sanitize_name(char *name, size_t size) {
    if (!name || size == 0) {
        return;
    }
    for (size_t i = 0; name[i] != '\0' && i + 1 < size; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c > 0x7e) {
            name[i] = '?';
        }
    }
    name[size - 1] = '\0';
    if (name[0] == '\0') {
        strncpy(name, "Unknown", size - 1);
        name[size - 1] = '\0';
    }
}

static bool prop_get_string(GVariant *props, const char *key, char *buf, size_t buf_size) {
    GVariant *value = g_variant_lookup_value(props, key, G_VARIANT_TYPE_STRING);
    if (!value) {
        return false;
    }
    strncpy(buf, g_variant_get_string(value, NULL), buf_size - 1);
    buf[buf_size - 1] = '\0';
    g_variant_unref(value);
    return true;
}

static bool prop_get_bool(GVariant *props, const char *key, bool *out) {
    GVariant *value = g_variant_lookup_value(props, key, G_VARIANT_TYPE_BOOLEAN);
    if (!value) {
        return false;
    }
    *out = g_variant_get_boolean(value);
    g_variant_unref(value);
    return true;
}

static bool prop_get_rssi(GVariant *props, int16_t *out) {
    GVariant *value = g_variant_lookup_value(props, "RSSI", NULL);
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
        return (int)db->connected - (int)da->connected;
    }
    if (da->paired != db->paired) {
        return (int)db->paired - (int)da->paired;
    }
    return (int)db->rssi - (int)da->rssi;
}

static void parse_objects(GVariant *objects) {
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
            strncpy(dev->path, path, sizeof(dev->path) - 1);
        }
        if (!prop_get_string(props, "Alias", dev->name, sizeof(dev->name)) &&
            !prop_get_string(props, "Name", dev->name, sizeof(dev->name))) {
            prop_get_string(props, "Address", dev->name, sizeof(dev->name));
        }
        sanitize_name(dev->name, sizeof(dev->name));
        prop_get_string(props, "Address", dev->address, sizeof(dev->address));
        prop_get_bool(props, "Paired", &dev->paired);
        prop_get_bool(props, "Connected", &dev->connected);
        prop_get_rssi(props, &dev->rssi);
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

static void get_managed_objects_cb(GObject *source, GAsyncResult *res, gpointer user_data) {
    GError   *error = NULL;
    GVariant *result;

    (void)user_data;
    scan_busy = false;

    result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    if (!result) {
        bt_log(error ? error->message : "GetManagedObjects failed");
        if (error) {
            g_error_free(error);
        }
        return;
    }

    if (g_variant_is_of_type(result, G_VARIANT_TYPE("a{oa{sa{sv}}}"))) {
        parse_objects(result);
    } else if (g_variant_is_of_type(result, G_VARIANT_TYPE("(a{oa{sa{sv}}})"))) {
        GVariant *objects = g_variant_get_child_value(result, 0);
        parse_objects(objects);
        g_variant_unref(objects);
    } else {
        bt_log("unexpected objects type");
    }

    g_variant_unref(result);
    notify_ui();
}

static void refresh_devices_async(void) {
    if (!bus || scan_busy) {
        return;
    }

    scan_busy = true;
    g_dbus_connection_call(bus, BLUEZ_BUS_NAME, "/", OBJMGR_INTERFACE, "GetManagedObjects", NULL,
                           NULL, G_DBUS_CALL_FLAGS_NONE, 2500, NULL, get_managed_objects_cb, NULL);
}

static void stop_discovery_cb(GObject *source, GAsyncResult *res, gpointer user_data) {
    GError   *error = NULL;
    GVariant *result;
    char     *reason = user_data;

    result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    if (result) {
        g_variant_unref(result);
    }
    if (error) {
        /* Ignore InProgress / not discovering */
        g_error_free(error);
    }

    scanning = false;
    scan_started_tick = 0;
    if (scan_timer) {
        lv_timer_pause(scan_timer);
    }

    if (reason) {
        msg_schedule_text_fmt("%s", reason);
        g_free(reason);
    }
    bt_log("scan stopped");
    notify_ui();
}

static void stop_discovery_async(const char *reason) {
    ensure_bus();
    if (!bus) {
        scanning = false;
        notify_ui();
        return;
    }

    g_dbus_connection_call(bus, BLUEZ_BUS_NAME, ADAPTER_OBJECT_PATH, ADAPTER_INTERFACE,
                           "StopDiscovery", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL,
                           stop_discovery_cb, g_strdup(reason ? reason : ""));
}

static void start_discovery_cb(GObject *source, GAsyncResult *res, gpointer user_data) {
    GError   *error = NULL;
    GVariant *result;

    (void)user_data;
    result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    if (!result) {
        bool in_progress = error && error->message && strstr(error->message, "InProgress");
        bt_log(error ? error->message : "StartDiscovery failed");
        if (error) {
            g_error_free(error);
        }
        if (!in_progress) {
            msg_schedule_text_fmt("BT scan failed");
            scanning = false;
            notify_ui();
            return;
        }
    } else {
        g_variant_unref(result);
    }

    scanning = true;
    scan_started_tick = lv_tick_get();
    if (!scan_timer) {
        scan_timer = lv_timer_create(scan_timer_cb, BT_SCAN_POLL_MS, NULL);
    } else {
        lv_timer_resume(scan_timer);
    }

    msg_schedule_text_fmt("Scanning...");
    bt_log("scan running");
    notify_ui();
    refresh_devices_async();
}

static void scan_timer_cb(lv_timer_t *timer) {
    (void)timer;

    if (!scanning) {
        lv_timer_pause(scan_timer);
        return;
    }

    if (lv_tick_elaps(scan_started_tick) >= BT_SCAN_TIMEOUT_MS) {
        stop_discovery_async("Scan timeout");
        return;
    }

    refresh_devices_async();
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

void bluetooth_refresh_devices(void) {
    if (scanning) {
        refresh_devices_async();
    }
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
            g_error_free(error);
        }
        set_status(BT_STATUS_UNAVAILABLE);
        return;
    }
    set_status(powered ? BT_STATUS_ON : BT_STATUS_OFF);
}

void bluetooth_power_setup(void) {
    bt_log("power_setup");
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
    device_count = 0;
    selected_index = -1;

    if (!params.wifi_enabled.x) {
        params_bool_set(&params.bt_enabled, false);
        set_status(BT_STATUS_UNAVAILABLE);
        return;
    }

    GError *error = NULL;
    if (!adapter_set_powered(false, &error)) {
        if (error) {
            g_error_free(error);
        }
        bluetooth_refresh_status();
        return;
    }
    params_bool_set(&params.bt_enabled, false);
    set_status(BT_STATUS_OFF);
}

void bluetooth_start_scan(void) {
    bt_log("start_scan pressed");

    if (!bluetooth_is_powered()) {
        msg_schedule_text_fmt("Turn Bluetooth on first");
        return;
    }
    if (scanning) {
        return;
    }

    ensure_bus();
    if (!bus) {
        msg_schedule_text_fmt("BT bus unavailable");
        return;
    }

    /* Non-blocking: completion runs on GLib context (wifi timer). */
    g_dbus_connection_call(bus, BLUEZ_BUS_NAME, ADAPTER_OBJECT_PATH, ADAPTER_INTERFACE,
                           "StartDiscovery", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL,
                           start_discovery_cb, NULL);
}

void bluetooth_stop_scan(void) {
    bt_log("stop_scan");
    if (!scanning) {
        return;
    }
    stop_discovery_async("Scan stopped");
}

bool bluetooth_scanning(void) {
    return scanning;
}

size_t bluetooth_device_count(void) {
    return device_count;
}

size_t bluetooth_copy_devices(bt_device_info_t *out, size_t max) {
    size_t n;
    if (!out || max == 0) {
        return 0;
    }
    n = device_count < max ? device_count : max;
    if (n > 0) {
        memcpy(out, devices, n * sizeof(bt_device_info_t));
    }
    return n;
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
