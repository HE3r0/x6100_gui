/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  MAC6100 / X6100 LVGL GUI — Bluetooth
 *
 *  Adapter power uses BlueZ D-Bus on the UI thread (same as before).
 *  Discovery uses bluetoothctl in a worker thread — no GLib/GDBus there,
 *  to avoid crashing against NetworkManager's GMainLoop.
 */

#include "bluetooth.h"

#include "params/params.h"
#include "pubsub_ids.h"
#include "msg.h"
#include "scheduler.h"

#include "lvgl/lvgl.h"
#include <gio/gio.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BLUEZ_BUS_NAME      "org.bluez"
#define ADAPTER_OBJECT_PATH "/org/bluez/hci0"
#define ADAPTER_INTERFACE   "org.bluez.Adapter1"
#define PROPS_INTERFACE     "org.freedesktop.DBus.Properties"

#define BT_SCAN_TIMEOUT_SEC 30

typedef enum {
    BT_CMD_NONE = 0,
    BT_CMD_START_SCAN,
    BT_CMD_STOP_SCAN,
} bt_cmd_t;

static bt_status_t      status = BT_STATUS_UNAVAILABLE;
static bt_device_info_t devices[BT_MAX_DEVICES];
static size_t           device_count = 0;
static int              selected_index = -1;

static pthread_t       worker_thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cond = PTHREAD_COND_INITIALIZER;
static bool            worker_started = false;
static bt_cmd_t        pending_cmd = BT_CMD_NONE;
static bool            scanning = false;
static time_t          scan_started_at = 0;
static char            worker_msg[64];
static bool            worker_msg_pending = false;

static lv_timer_t *restore_timer = NULL;

static bool  power_on_internal(bool user_initiated);
static void *bt_worker_main(void *arg);
static void  ensure_worker(void);
static void  ui_notify_cb(void *arg);

static void set_status(bt_status_t val) {
    if (status == val) {
        return;
    }
    status = val;
    lv_msg_send(MSG_BT_STATE_CHANGED, NULL);
}

static void post_ui_notify(void) {
    scheduler_put_noargs(ui_notify_cb);
}

static void ui_notify_cb(void *arg) {
    char msg[64];
    bool have_msg = false;

    (void)arg;

    pthread_mutex_lock(&lock);
    if (worker_msg_pending) {
        strncpy(msg, worker_msg, sizeof(msg) - 1);
        msg[sizeof(msg) - 1] = '\0';
        worker_msg_pending = false;
        have_msg = true;
    }
    pthread_mutex_unlock(&lock);

    if (have_msg && msg[0]) {
        msg_schedule_text_fmt("%s", msg);
    }

    lv_msg_send(MSG_BT_DEVICES_CHANGED, NULL);
    lv_msg_send(MSG_BT_STATE_CHANGED, NULL);
}

static void worker_set_msg(const char *text) {
    pthread_mutex_lock(&lock);
    strncpy(worker_msg, text ? text : "", sizeof(worker_msg) - 1);
    worker_msg[sizeof(worker_msg) - 1] = '\0';
    worker_msg_pending = true;
    pthread_mutex_unlock(&lock);
}

/* ---------- D-Bus power (UI thread only) ---------- */

static GDBusConnection *open_system_bus(GError **error) {
    return g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, error);
}

static bool adapter_get_bool_prop(const char *prop, bool *out, GError **error) {
    GDBusConnection *conn = open_system_bus(error);
    if (!conn) {
        return false;
    }

    GVariant *result = g_dbus_connection_call_sync(
        conn, BLUEZ_BUS_NAME, ADAPTER_OBJECT_PATH, PROPS_INTERFACE, "Get",
        g_variant_new("(ss)", ADAPTER_INTERFACE, prop), G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, error);

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
        conn, BLUEZ_BUS_NAME, ADAPTER_OBJECT_PATH, PROPS_INTERFACE, "Set",
        g_variant_new("(ssv)", ADAPTER_INTERFACE, "Powered", g_variant_new_boolean(powered)),
        NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, error);

    g_object_unref(conn);
    if (result) {
        g_variant_unref(result);
        return true;
    }
    return false;
}

/* ---------- bluetoothctl helpers (worker thread only) ---------- */

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

static int btctl_run(const char *command) {
    char cmd[320];
    /*
     * Keep bluetoothctl off a TTY and bounded. Discovery continues in bluetoothd
     * after "scan on" returns.
     */
    snprintf(cmd, sizeof(cmd),
             "timeout 3 sh -c \"printf '%%s\\n' '%s' | bluetoothctl\" >/dev/null 2>&1",
             command);
    return system(cmd);
}

static size_t btctl_list_devices(bt_device_info_t *out, size_t max) {
    FILE  *fp;
    char   line[256];
    size_t count = 0;

    fp = popen("bluetoothctl devices 2>/dev/null", "r");
    if (!fp) {
        return 0;
    }

    while (count < max && fgets(line, sizeof(line), fp)) {
        char addr[32];
        char name[64];
        char *p;

        /* Device AA:BB:CC:DD:EE:FF Name may have spaces */
        if (strncmp(line, "Device ", 7) != 0) {
            continue;
        }

        name[0] = '\0';
        if (sscanf(line + 7, "%31s %63[^\n]", addr, name) < 1) {
            continue;
        }

        /* trim trailing CR */
        p = strchr(name, '\r');
        if (p) {
            *p = '\0';
        }

        memset(&out[count], 0, sizeof(out[count]));
        strncpy(out[count].address, addr, sizeof(out[count].address) - 1);
        if (name[0]) {
            strncpy(out[count].name, name, sizeof(out[count].name) - 1);
        } else {
            strncpy(out[count].name, addr, sizeof(out[count].name) - 1);
        }
        sanitize_name(out[count].name, sizeof(out[count].name));
        snprintf(out[count].path, sizeof(out[count].path),
                 "/org/bluez/hci0/dev_%c%c_%c%c_%c%c_%c%c_%c%c_%c%c",
                 addr[0], addr[1], addr[3], addr[4], addr[6], addr[7],
                 addr[9], addr[10], addr[12], addr[13], addr[15], addr[16]);
        count++;
    }

    pclose(fp);
    return count;
}

static void publish_devices(const bt_device_info_t *src, size_t count) {
    pthread_mutex_lock(&lock);
    if (count > BT_MAX_DEVICES) {
        count = BT_MAX_DEVICES;
    }
    memcpy(devices, src, count * sizeof(bt_device_info_t));
    device_count = count;
    if (selected_index >= (int)device_count) {
        selected_index = device_count > 0 ? 0 : -1;
    }
    pthread_mutex_unlock(&lock);
    post_ui_notify();
}

static void worker_start_scan(void) {
    if (btctl_run("scan on") != 0) {
        pthread_mutex_lock(&lock);
        scanning = false;
        pthread_mutex_unlock(&lock);
        worker_set_msg("BT scan failed");
        post_ui_notify();
        return;
    }

    pthread_mutex_lock(&lock);
    scanning = true;
    scan_started_at = time(NULL);
    pthread_mutex_unlock(&lock);

    worker_set_msg("Scanning...");
    post_ui_notify();

    bt_device_info_t tmp[BT_MAX_DEVICES];
    size_t           count = btctl_list_devices(tmp, BT_MAX_DEVICES);
    publish_devices(tmp, count);
}

static void worker_stop_scan(const char *msg) {
    btctl_run("scan off");

    pthread_mutex_lock(&lock);
    scanning = false;
    scan_started_at = 0;
    pthread_mutex_unlock(&lock);

    if (msg) {
        worker_set_msg(msg);
    }
    post_ui_notify();
}

static void worker_poll(void) {
    bool   do_poll = false;
    time_t started = 0;

    pthread_mutex_lock(&lock);
    if (scanning) {
        do_poll = true;
        started = scan_started_at;
    }
    pthread_mutex_unlock(&lock);

    if (!do_poll) {
        return;
    }

    if (started != 0 && (time(NULL) - started) >= BT_SCAN_TIMEOUT_SEC) {
        worker_stop_scan("Scan timeout");
        return;
    }

    bt_device_info_t tmp[BT_MAX_DEVICES];
    size_t           count = btctl_list_devices(tmp, BT_MAX_DEVICES);
    publish_devices(tmp, count);
}

static void *bt_worker_main(void *arg) {
    (void)arg;

    while (true) {
        bt_cmd_t cmd = BT_CMD_NONE;
        bool     is_scanning = false;

        pthread_mutex_lock(&lock);
        if (pending_cmd == BT_CMD_NONE && !scanning) {
            pthread_cond_wait(&cond, &lock);
        } else if (pending_cmd == BT_CMD_NONE && scanning) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&cond, &lock, &ts);
        }

        cmd = pending_cmd;
        pending_cmd = BT_CMD_NONE;
        is_scanning = scanning;
        pthread_mutex_unlock(&lock);

        if (cmd == BT_CMD_START_SCAN) {
            worker_start_scan();
        } else if (cmd == BT_CMD_STOP_SCAN) {
            worker_stop_scan("Scan stopped");
        } else if (is_scanning) {
            worker_poll();
        }
    }

    return NULL;
}

static void ensure_worker(void) {
    if (worker_started) {
        return;
    }
    worker_started = true;
    if (pthread_create(&worker_thread, NULL, bt_worker_main, NULL) != 0) {
        worker_started = false;
        return;
    }
    pthread_detach(worker_thread);
}

static void queue_cmd(bt_cmd_t cmd) {
    ensure_worker();
    pthread_mutex_lock(&lock);
    pending_cmd = cmd;
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&lock);
}

/* ---------- public API ---------- */

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

    pthread_mutex_lock(&lock);
    device_count = 0;
    selected_index = -1;
    pthread_mutex_unlock(&lock);

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
    if (!bluetooth_is_powered()) {
        msg_schedule_text_fmt("Turn Bluetooth on first");
        return;
    }

    bool already;
    pthread_mutex_lock(&lock);
    already = scanning || pending_cmd == BT_CMD_START_SCAN;
    pthread_mutex_unlock(&lock);
    if (already) {
        return;
    }

    queue_cmd(BT_CMD_START_SCAN);
}

void bluetooth_stop_scan(void) {
    bool need_stop;
    pthread_mutex_lock(&lock);
    need_stop = scanning || pending_cmd == BT_CMD_START_SCAN;
    if (pending_cmd == BT_CMD_START_SCAN) {
        pending_cmd = BT_CMD_NONE;
    }
    pthread_mutex_unlock(&lock);

    if (!need_stop) {
        return;
    }

    queue_cmd(BT_CMD_STOP_SCAN);
}

bool bluetooth_scanning(void) {
    bool val;
    pthread_mutex_lock(&lock);
    val = scanning || pending_cmd == BT_CMD_START_SCAN || pending_cmd == BT_CMD_STOP_SCAN;
    pthread_mutex_unlock(&lock);
    return val;
}

size_t bluetooth_device_count(void) {
    size_t n;
    pthread_mutex_lock(&lock);
    n = device_count;
    pthread_mutex_unlock(&lock);
    return n;
}

size_t bluetooth_copy_devices(bt_device_info_t *out, size_t max) {
    size_t n;

    if (!out || max == 0) {
        return 0;
    }

    pthread_mutex_lock(&lock);
    n = device_count < max ? device_count : max;
    if (n > 0) {
        memcpy(out, devices, n * sizeof(bt_device_info_t));
    }
    pthread_mutex_unlock(&lock);
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
    pthread_mutex_lock(&lock);
    if (index < 0 || (size_t)index >= device_count) {
        selected_index = -1;
    } else {
        selected_index = index;
    }
    pthread_mutex_unlock(&lock);
}
