// SPDX-License-Identifier: GPL-2.0
// test-fan40.c — userspace validation for x-fan40 overlay and module
//
// Mirrors the device discovery and fan detection logic from
// x-fan40-aux-thermal.c, using standard POSIX file I/O against the
// same sysfs paths that the kernel module uses.
//
// Build:  cc -O2 -o test-fan40 test-fan40.c
// Run:    ./test-fan40                  (read-only checks)
//         sudo ./test-fan40 --spin      (briefly spin fan and read RPM)
//         sudo ./test-fan40 --pwm 255   (set manual PWM 0-255, read RPM; fan stays in manual)
//         sudo ./test-fan40 --auto      (restore thermal control)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#define AUX_TRIP_HI_MAX_STATE  5
#define MAX_APEX_DEVS          2
#define MAX_NVME_DEVS          4

/* ── Test accounting ─────────────────────────────────────────────────────── */

static int g_pass = 0, g_fail = 0, g_warn = 0;

static void ok(const char *msg)   { printf("[  OK  ] %s\n", msg); g_pass++; }
static void bad(const char *msg)  { printf("[ FAIL ] %s\n", msg); g_fail++; }
static void note(const char *msg) { printf("[ WARN ] %s\n", msg); g_warn++; }
static void info(const char *msg) { printf("[ INFO ] %s\n", msg); }

/* ── sysfs helpers ───────────────────────────────────────────────────────── */

static int read_int(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return INT_MIN;
    int v;
    int r = fscanf(f, "%d", &v);
    fclose(f);
    return r == 1 ? v : INT_MIN;
}

static int read_str(const char *path, char *buf, size_t len)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, len - 1, f);
    fclose(f);
    if (n > 0) {
        buf[n] = '\0';
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r'))
            buf[--n] = '\0';
    }
    return (int)n;
}

static int write_int(const char *path, int val)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "%d\n", val);
    fclose(f);
    return 0;
}

/* ── Thermal zone discovery (mirrors discover_aux_zone_trips) ────────────── */

static char g_hwmon_dir[128];
static char g_cdev_state_path[128];

#define AUX_SENSOR_PATH "/sys/bus/platform/devices/x-fan40-aux-sensor"

static void check_thermal_zones(void)
{
    char path[128], type[64];
    int found_aux = 0, found_cpu = 0;

    printf("\n--- Thermal Zones ---\n");

    for (int z = 0; z <= 9; z++) {
        snprintf(path, sizeof(path),
                 "/sys/class/thermal/thermal_zone%d/type", z);
        if (read_str(path, type, sizeof(type)) < 0)
            break;

        snprintf(path, sizeof(path),
                 "/sys/class/thermal/thermal_zone%d/temp", z);
        int temp = read_int(path);

        char msg[80];
        snprintf(msg, sizeof(msg), "thermal_zone%d: type=%s temp=%.1f°C",
                 z, type, temp != INT_MIN ? temp / 1000.0 : 0.0);
        info(msg);

        if (strcmp(type, "x-fan40-aux") == 0) found_aux = 1;
        if (strcmp(type, "cpu-thermal")  == 0) found_cpu = 1;
    }

    if (found_aux)
        ok("x-fan40-aux thermal zone registered");
    else
        bad("x-fan40-aux thermal zone not found — is the module loaded?");

    if (found_cpu)
        ok("cpu-thermal zone registered");
    else
        note("cpu-thermal zone not in sysfs — CM5 may manage CPU fan via firmware");
}

/* ── Cooling device discovery (mirrors discover_fan_cdev) ────────────────── */

static void check_cooling_devices(void)
{
    char path[128], type[32];
    int found = 0;

    printf("\n--- Cooling Devices ---\n");

    for (int n = 0; n <= 9; n++) {
        snprintf(path, sizeof(path),
                 "/sys/class/thermal/cooling_device%d/type", n);
        if (read_str(path, type, sizeof(type)) < 0)
            break;

        snprintf(path, sizeof(path),
                 "/sys/class/thermal/cooling_device%d/max_state", n);
        int max = read_int(path);

        snprintf(path, sizeof(path),
                 "/sys/class/thermal/cooling_device%d/cur_state", n);
        int cur = read_int(path);

        char msg[80];
        snprintf(msg, sizeof(msg),
                 "cooling_device%d: type=%s max_state=%d cur_state=%d",
                 n, type, max, cur);
        info(msg);

        if (strcmp(type, "pwm-fan") == 0 && max == AUX_TRIP_HI_MAX_STATE && !found) {
            snprintf(g_cdev_state_path, sizeof(g_cdev_state_path),
                     "/sys/class/thermal/cooling_device%d/cur_state", n);
            snprintf(msg, sizeof(msg),
                     "x-fan40 cooling_device%d identified (max_state=5)", n);
            ok(msg);
            found = 1;
        }
    }

    if (!found)
        bad("x-fan40 cooling device (pwm-fan, max_state=5) not found");
}

/* ── hwmon discovery (mirrors discover_fan_hwmon) ────────────────────────── */

static void check_hwmon(void)
{
    char path[160], name[32];

    printf("\n--- x-fan hwmon ---\n");

    for (int n = 0; n <= 15; n++) {
        snprintf(path, sizeof(path),
                 "/sys/devices/platform/x-fan/hwmon/hwmon%d/name", n);
        if (read_str(path, name, sizeof(name)) < 0)
            continue;

        snprintf(g_hwmon_dir, sizeof(g_hwmon_dir),
                 "/sys/devices/platform/x-fan/hwmon/hwmon%d", n);

        snprintf(path, sizeof(path), "%s/pwm1_enable", g_hwmon_dir);
        int enable = read_int(path);
        snprintf(path, sizeof(path), "%s/pwm1", g_hwmon_dir);
        int pwm = read_int(path);
        int cur_state = g_cdev_state_path[0] ? read_int(g_cdev_state_path) : INT_MIN;

        char msg[128];
        const char *enable_label = enable == 2 ? "auto" :
                                   enable == 1 ? "manual" : "disabled";
        if (cur_state != INT_MIN)
            snprintf(msg, sizeof(msg),
                     "x-fan hwmon%d found (state=%d pwm1_enable=%d (%s) pwm1=%d / %.0f%%)",
                     n, cur_state, enable, enable_label, pwm, pwm / 255.0 * 100);
        else
            snprintf(msg, sizeof(msg),
                     "x-fan hwmon%d found (pwm1_enable=%d (%s) pwm1=%d / %.0f%%)",
                     n, enable, enable_label, pwm, pwm / 255.0 * 100);
        ok(msg);

        snprintf(path, sizeof(path), "%s/fan1_input", g_hwmon_dir);
        int rpm = read_int(path);
        if (rpm != INT_MIN) {
            snprintf(msg, sizeof(msg), "tachometer present: %d RPM", rpm);
            ok(msg);
        } else {
            note("fan1_input not present — no tachometer configured");
        }
        return;
    }

    bad("x-fan hwmon not found under /sys/devices/platform/x-fan/");
}

/* ── Temperature source discovery (mirrors discover_apex/nvme_paths) ─────── */

static void check_temp_sources(void)
{
    char path[128];
    int found = 0;

    printf("\n--- Temperature Sources ---\n");

    for (int n = 0; n < MAX_APEX_DEVS; n++) {
        snprintf(path, sizeof(path), "/sys/class/apex/apex_%d/temp", n);
        int t = read_int(path);
        if (t != INT_MIN) {
            char msg[64];
            snprintf(msg, sizeof(msg), "apex_%d: %.1f°C", n, t / 1000.0);
            ok(msg);
            found++;
        }
    }

    for (int n = 0; n < MAX_NVME_DEVS; n++) {
        for (int h = 0; h <= 15; h++) {
            snprintf(path, sizeof(path),
                     "/sys/class/nvme/nvme%d/hwmon%d/temp1_input", n, h);
            int t = read_int(path);
            if (t != INT_MIN) {
                char msg[64];
                snprintf(msg, sizeof(msg), "nvme%d: %.1f°C", n, t / 1000.0);
                ok(msg);
                found++;
                break;
            }
        }
    }

    if (!found)
        note("no Apex or NVMe temperature sources found");
}

/* ── Aux sensor status (reads module sysfs attributes) ──────────────────── */

static void check_aux_sensor(void)
{
    char path[128], buf[32];
    char msg[128];

    printf("\n--- Aux Sensor Status ---\n");

    snprintf(path, sizeof(path), "%s/aux_temp", AUX_SENSOR_PATH);
    int temp = read_int(path);
    if (temp == INT_MIN) {
        bad("aux_temp not readable — is x-fan40-aux-thermal loaded?");
        return;
    }
    snprintf(msg, sizeof(msg), "aux_temp: %.1f°C", temp / 1000.0);
    ok(msg);

    snprintf(path, sizeof(path), "%s/source", AUX_SENSOR_PATH);
    if (read_str(path, buf, sizeof(buf)) > 0) {
        snprintf(msg, sizeof(msg), "source (hottest device): %s", buf);
        ok(msg);
    }

    snprintf(path, sizeof(path), "%s/fan_state", AUX_SENSOR_PATH);
    int state = read_int(path);
    if (state != INT_MIN) {
        snprintf(msg, sizeof(msg), "fan_state: %d", state);
        ok(msg);
    }

    snprintf(path, sizeof(path), "%s/fan_driver", AUX_SENSOR_PATH);
    if (read_str(path, buf, sizeof(buf)) > 0) {
        snprintf(msg, sizeof(msg), "fan_driver (zone driving fan): %s", buf);
        ok(msg);
    }
}

/* ── Fan spin-up test (mirrors fan detection in aux_poll_fn) ─────────────── */

static void test_spin(void)
{
    char path[160];

    printf("\n--- Fan Spin-up Test ---\n");

    if (g_hwmon_dir[0] == '\0') {
        bad("hwmon not found; skipping spin-up test");
        return;
    }
    if (geteuid() != 0) {
        note("not root; skipping spin-up test — re-run with sudo ./test-fan40 --spin");
        return;
    }

    /* Switch to manual PWM control */
    snprintf(path, sizeof(path), "%s/pwm1_enable", g_hwmon_dir);
    if (write_int(path, 1) < 0) {
        bad("failed to set manual PWM mode (pwm1_enable=1)");
        return;
    }

    /* Spin at ~80% duty — same as kernel module detection */
    snprintf(path, sizeof(path), "%s/pwm1", g_hwmon_dir);
    if (write_int(path, 200) < 0) {
        bad("failed to set PWM duty (pwm1=200)");
        write_int(path, 2);
        return;
    }

    ok("fan commanded to spin (pwm1=200 / ~80% duty)");
    info("waiting 3 s for tachometer measurement window...");
    sleep(3);

    /* Read RPM */
    snprintf(path, sizeof(path), "%s/fan1_input", g_hwmon_dir);
    int rpm = read_int(path);

    /* Restore thermal control */
    snprintf(path, sizeof(path), "%s/pwm1_enable", g_hwmon_dir);
    write_int(path, 2);
    ok("restored thermal control (pwm1_enable=2)");

    if (rpm == INT_MIN) {
        note("fan1_input not readable — no tachometer; fan assumed present");
    } else if (rpm > 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "fan spinning at %d RPM", rpm);
        ok(msg);
    } else {
        bad("RPM reads 0 — fan may not be spinning or tachometer not wired");
    }
}

/* ── Manual fan speed control ────────────────────────────────────────────── */

static void set_fan_pwm(int pwm)
{
    char path[160], msg[80];

    printf("\n--- Set Fan PWM ---\n");

    if (g_hwmon_dir[0] == '\0') {
        bad("hwmon not found; cannot set PWM");
        return;
    }
    if (geteuid() != 0) {
        bad("root required — re-run with sudo ./test-fan40 --pwm <value>");
        return;
    }

    snprintf(path, sizeof(path), "%s/pwm1_enable", g_hwmon_dir);
    if (write_int(path, 1) < 0) { bad("failed to set manual mode (pwm1_enable=1)"); return; }

    snprintf(path, sizeof(path), "%s/pwm1", g_hwmon_dir);
    if (write_int(path, pwm) < 0) {
        bad("failed to write pwm1");
        write_int(path, 2);
        return;
    }

    snprintf(msg, sizeof(msg), "fan set to manual mode, pwm1=%d (%.0f%%)",
             pwm, pwm / 255.0 * 100);
    ok(msg);
    info("waiting 2 s for tachometer measurement window...");
    sleep(2);

    snprintf(path, sizeof(path), "%s/fan1_input", g_hwmon_dir);
    int rpm = read_int(path);
    if (rpm == INT_MIN)
        note("fan1_input not readable — no tachometer configured");
    else {
        snprintf(msg, sizeof(msg), "fan speed: %d RPM", rpm);
        ok(msg);
    }

    info("fan remains in manual mode; run --auto to restore thermal control");
}

static void set_fan_auto(void)
{
    char path[160];

    printf("\n--- Restore Auto Control ---\n");

    if (g_hwmon_dir[0] == '\0') {
        bad("hwmon not found; cannot restore auto mode");
        return;
    }
    if (geteuid() != 0) {
        bad("root required — re-run with sudo ./test-fan40 --auto");
        return;
    }

    snprintf(path, sizeof(path), "%s/pwm1_enable", g_hwmon_dir);
    if (write_int(path, 2) < 0)
        bad("failed to restore auto mode (pwm1_enable=2)");
    else
        ok("restored thermal control (pwm1_enable=2 / auto)");
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    int do_spin = 0, do_auto = 0, pwm_val = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--spin") == 0) {
            do_spin = 1;
        } else if (strcmp(argv[i], "--auto") == 0) {
            do_auto = 1;
        } else if (strcmp(argv[i], "--pwm") == 0 && i + 1 < argc) {
            pwm_val = atoi(argv[++i]);
            if (pwm_val < 0 || pwm_val > 255) {
                fprintf(stderr, "error: --pwm value must be 0-255\n");
                return 1;
            }
        }
    }

    printf("x-fan40 system validation\n");
    printf("=========================\n");

    check_thermal_zones();
    check_cooling_devices();
    check_hwmon();
    check_temp_sources();
    check_aux_sensor();

    if (do_spin)
        test_spin();
    if (pwm_val >= 0)
        set_fan_pwm(pwm_val);
    if (do_auto)
        set_fan_auto();

    printf("\n=========================\n");
    printf("Results: %d passed, %d failed, %d warnings\n",
           g_pass, g_fail, g_warn);

    return g_fail > 0 ? 1 : 0;
}
