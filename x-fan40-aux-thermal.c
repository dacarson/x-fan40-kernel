// SPDX-License-Identifier: GPL-2.0
// x-fan40-aux-thermal.c
//
// Platform driver that exposes a virtual thermal sensor for auxiliary heat
// sources (Coral TPU, NVMe) that have no standard kernel thermal zone.
//
// The driver polls all discovered temperature sources and reports the maximum
// to the kernel thermal framework, which then drives the pwm-fan cooling device
// using max-wins arbitration across all bound zones (CPU zone from DT, aux zone
// from this driver).
//
// Sources are discovered at probe and retried on every poll cycle while the
// path list is empty, so devices that appear after this module loads are
// picked up automatically:
//   Apex: /sys/class/apex/apex_{0..MAX_APEX_DEVS-1}/temp
//   NVMe: /sys/class/nvme/nvme{0..MAX_NVME_DEVS-1}/hwmon{N}/temp1_input
//
// Adding a new source type requires only a new discover_*_paths() function
// that appends to the shared temp_paths[] array.
//
// Sysfs attributes (read-only):
//   aux_temp_mc  — max Apex/NVMe temperature in milli-Celsius (drives aux zone)
//   source       — name of hottest aux device (e.g. "apex_0", "nvme1")
//   cpu_temp_mc  — CPU temperature in milli-Celsius (observability only)
//   fan_state    — current pwm-fan cooling state (0-5)
//   fan_driver   — zone driving the fan: "cpu", source name, or "none"
//
// cpu_temp_mc is polled for visibility but is NOT fed into the aux thermal zone.
// The DT overlay (fragment 2) owns CPU cooling via the existing cpu_thermal zone.
//
// fan_driver is determined by comparing the actual fan_state with the maximum
// state the aux zone could demand at the current aux_temp_mc.  If fan_state
// exceeds that maximum, the CPU zone must be responsible.
//
// Module parameters:
//   poll_ms  - poll interval in milliseconds (default: 1000)
//
// Requires the x-fan40 DT overlay (fragments 3 & 4) to be active.
//
// Build:        make module
// Install:      sudo make install-module
// Load at boot: echo x-fan40-aux-thermal | sudo tee /etc/modules-load.d/x-fan40-aux.conf

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/thermal.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/fs.h>

#define POLL_MS_DEFAULT      1000
#define SAFE_TEMP_MC         25000  /* 25 °C — below all trip points */
#define MAX_APEX_DEVS        2
#define MAX_NVME_DEVS        4
#define MAX_TEMP_SOURCES     (MAX_APEX_DEVS + MAX_NVME_DEVS)
#define MAX_PATH_LEN         48     /* "/sys/class/nvme/nvmeX/hwmonXX/temp1_input" = 41 */
#define MAX_NAME_LEN         8      /* "apex_X" = 6, "nvmeX" = 5 */
#define MAX_CDEV_PATH_LEN    64     /* "/sys/class/thermal/cooling_deviceN/cur_state" */

/*
 * Cooling map upper bounds from x-fan40-overlay.dts fragment 4:
 *   aux_trip_lo (55 °C) → cooling-device <&x_fan40 1 3>  upper = 3
 *   aux_trip_hi (80 °C) → cooling-device <&x_fan40 4 5>  upper = 5
 * Update these if the DT overlay cooling maps change.
 */
#define AUX_TRIP_LO_MAX_STATE  3
#define AUX_TRIP_HI_MAX_STATE  5

static unsigned int poll_ms = POLL_MS_DEFAULT;
module_param(poll_ms, uint, 0644);
MODULE_PARM_DESC(poll_ms, "Temperature poll interval in milliseconds");

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Virtual thermal sensor for x-fan40 auxiliary heat sources");
MODULE_AUTHOR("x-fan40");

struct aux_sensor_data {
    /* Aux zone temperatures (Apex + NVMe) — drive the thermal zone */
    int  aux_temp_mc;
    char source_name[MAX_NAME_LEN];

    /* CPU temperature — observability only, does not drive aux zone */
    int  cpu_temp_mc;

    /* Fan state */
    int  fan_state;                       /* actual pwm-fan cur_state */
    char fan_driver_name[MAX_NAME_LEN];   /* "cpu", source name, or "none" */
    char fan_cdev_state_path[MAX_CDEV_PATH_LEN];

    /* Aux zone trip temperatures, read from sysfs after zone registration */
    int  aux_trip_lo_temp;   /* milli-Celsius; default 55000 */
    int  aux_trip_hi_temp;   /* milli-Celsius; default 80000 */

    /* Source path table */
    char temp_paths[MAX_TEMP_SOURCES][MAX_PATH_LEN];
    char temp_names[MAX_TEMP_SOURCES][MAX_NAME_LEN];
    int  temp_path_count;

    struct thermal_zone_device *tz;
    struct delayed_work        poll_work;
    struct device              *dev;
};

/* ── Thermal zone callback ───────────────────────────────────────────────── */

static int aux_get_temp(struct thermal_zone_device *tz, int *temp)
{
    struct aux_sensor_data *d = thermal_zone_device_priv(tz);
    *temp = d->aux_temp_mc;
    return 0;
}

static const struct thermal_zone_device_ops aux_tz_ops = {
    .get_temp = aux_get_temp,
};

/* ── sysfs helpers ───────────────────────────────────────────────────────── */

/*
 * Read a millidegree (or any small integer) from a sysfs path.
 * Returns the value on success, INT_MIN on any error.
 */
static int read_int_from_sysfs(const char *path)
{
    struct file *f;
    char buf[32];
    ssize_t n;
    loff_t pos = 0;
    int val;

    f = filp_open(path, O_RDONLY, 0);
    if (IS_ERR(f))
        return INT_MIN;

    n = kernel_read(f, buf, sizeof(buf) - 1, &pos);
    filp_close(f, NULL);

    if (n <= 0 || kstrtoint(strim(buf), 10, &val))
        return INT_MIN;

    return val;
}

/*
 * Read a short string from a sysfs path into buf (NUL-terminated, trimmed).
 * Returns number of bytes read on success, negative on error.
 */
static ssize_t read_str_from_sysfs(const char *path, char *buf, size_t len)
{
    struct file *f;
    ssize_t n;
    loff_t pos = 0;

    f = filp_open(path, O_RDONLY, 0);
    if (IS_ERR(f))
        return PTR_ERR(f);

    n = kernel_read(f, buf, len - 1, &pos);
    filp_close(f, NULL);

    if (n > 0) {
        buf[n] = '\0';
        strim(buf);
    }
    return n;
}

/* ── Source path discovery ───────────────────────────────────────────────── */

static void discover_apex_paths(struct aux_sensor_data *d)
{
    char path[MAX_PATH_LEN];
    int n;

    for (n = 0; n < MAX_APEX_DEVS && d->temp_path_count < MAX_TEMP_SOURCES; n++) {
        struct file *f;

        snprintf(path, sizeof(path), "/sys/class/apex/apex_%d/temp", n);
        f = filp_open(path, O_RDONLY, 0);
        if (!IS_ERR(f)) {
            filp_close(f, NULL);
            strscpy(d->temp_paths[d->temp_path_count], path, MAX_PATH_LEN);
            snprintf(d->temp_names[d->temp_path_count], MAX_NAME_LEN, "apex_%d", n);
            d->temp_path_count++;
        }
    }
}

static void discover_nvme_paths(struct aux_sensor_data *d)
{
    char path[MAX_PATH_LEN];
    int n, h;

    for (n = 0; n < MAX_NVME_DEVS && d->temp_path_count < MAX_TEMP_SOURCES; n++) {
        for (h = 0; h <= 15; h++) {
            struct file *f;

            snprintf(path, sizeof(path),
                     "/sys/class/nvme/nvme%d/hwmon%d/temp1_input", n, h);
            f = filp_open(path, O_RDONLY, 0);
            if (!IS_ERR(f)) {
                filp_close(f, NULL);
                strscpy(d->temp_paths[d->temp_path_count], path, MAX_PATH_LEN);
                snprintf(d->temp_names[d->temp_path_count], MAX_NAME_LEN, "nvme%d", n);
                d->temp_path_count++;
                break;   /* found hwmon for this drive; move to next nvme */
            }
        }
    }
}

static void discover_all_paths(struct aux_sensor_data *d)
{
    d->temp_path_count = 0;
    discover_apex_paths(d);
    discover_nvme_paths(d);

    if (d->temp_path_count)
        dev_info(d->dev, "discovered %d temperature source(s)\n",
                 d->temp_path_count);
}

/* ── Fan cooling device discovery ────────────────────────────────────────── */

/*
 * Find the pwm-fan cooling device by scanning /sys/class/thermal/cooling_deviceN
 * for type "pwm-fan" and store the path to its cur_state file.
 */
static void discover_fan_cdev(struct aux_sensor_data *d)
{
    char path[MAX_CDEV_PATH_LEN];
    char type_buf[16];
    int n;

    for (n = 0; n <= 9; n++) {
        snprintf(path, sizeof(path),
                 "/sys/class/thermal/cooling_device%d/type", n);
        if (read_str_from_sysfs(path, type_buf, sizeof(type_buf)) <= 0)
            continue;
        if (strcmp(type_buf, "pwm-fan") != 0)
            continue;
        snprintf(d->fan_cdev_state_path, sizeof(d->fan_cdev_state_path),
                 "/sys/class/thermal/cooling_device%d/cur_state", n);
        dev_info(d->dev, "fan cooling device: cooling_device%d\n", n);
        return;
    }
    dev_warn(d->dev, "pwm-fan cooling device not found\n");
}

/* ── Aux zone trip point discovery ───────────────────────────────────────── */

/*
 * Find our aux thermal zone ("x-fan40-aux") and cache its trip point
 * temperatures so the poll function can determine the max cooling state
 * the aux zone can demand at any given temperature.
 *
 * Called after devm_thermal_of_zone_register() so the zone is in sysfs.
 * Falls back to the DT overlay defaults (55/80 °C) if the zone is not
 * found — this should not happen in normal operation.
 */
static void discover_aux_zone_trips(struct aux_sensor_data *d)
{
    char path[64];
    char type_buf[32];
    int z;

    for (z = 0; z <= 9; z++) {
        snprintf(path, sizeof(path),
                 "/sys/class/thermal/thermal_zone%d/type", z);
        if (read_str_from_sysfs(path, type_buf, sizeof(type_buf)) <= 0)
            continue;
        if (strcmp(type_buf, "x-fan40-aux") != 0)
            continue;

        snprintf(path, sizeof(path),
                 "/sys/class/thermal/thermal_zone%d/trip_point_0_temp", z);
        d->aux_trip_lo_temp = read_int_from_sysfs(path);

        snprintf(path, sizeof(path),
                 "/sys/class/thermal/thermal_zone%d/trip_point_1_temp", z);
        d->aux_trip_hi_temp = read_int_from_sysfs(path);

        dev_info(d->dev, "aux zone trips: lo=%d hi=%d mC\n",
                 d->aux_trip_lo_temp, d->aux_trip_hi_temp);
        return;
    }

    /* Fallback — should not occur if DT overlay is loaded */
    d->aux_trip_lo_temp = 55000;
    d->aux_trip_hi_temp = 80000;
    dev_warn(d->dev,
             "aux zone not found in sysfs; using default trips 55/80 °C\n");
}

/* ── Poll workqueue function ─────────────────────────────────────────────── */

static void aux_poll_fn(struct work_struct *work)
{
    struct aux_sensor_data *d = container_of(work, struct aux_sensor_data,
                                             poll_work.work);
    int max_temp = INT_MIN;
    int max_idx = -1;
    int i;

    /* ── Aux sources (Apex + NVMe) ── */
    if (!d->temp_path_count)
        discover_all_paths(d);

    for (i = 0; i < d->temp_path_count; i++) {
        int v = read_int_from_sysfs(d->temp_paths[i]);
        if (v != INT_MIN && v > max_temp) {
            max_temp = v;
            max_idx = i;
        }
    }

    if (max_idx >= 0 && (max_temp != d->aux_temp_mc ||
        strncmp(d->source_name, d->temp_names[max_idx], MAX_NAME_LEN) != 0)) {
        d->aux_temp_mc = max_temp;
        strscpy(d->source_name, d->temp_names[max_idx], MAX_NAME_LEN);
        thermal_zone_device_update(d->tz, THERMAL_EVENT_UNSPECIFIED);
    }

    /* ── CPU (observability only) ── */
    {
        int v = read_int_from_sysfs("/sys/class/thermal/thermal_zone0/temp");
        if (v != INT_MIN)
            d->cpu_temp_mc = v;
    }

    /* ── Fan state and driver ── */
    if (d->fan_cdev_state_path[0]) {
        int cur = read_int_from_sysfs(d->fan_cdev_state_path);
        if (cur != INT_MIN) {
            /*
             * Determine the maximum cooling state the aux zone can demand
             * at the current aux_temp_mc, based on the DT cooling maps:
             *   aux_trip_lo → states 1..AUX_TRIP_LO_MAX_STATE
             *   aux_trip_hi → states 1..AUX_TRIP_HI_MAX_STATE
             * If the actual fan state exceeds this maximum, the CPU zone
             * must be the reason.
             */
            int aux_max;

            if (d->aux_temp_mc >= d->aux_trip_hi_temp)
                aux_max = AUX_TRIP_HI_MAX_STATE;
            else if (d->aux_temp_mc >= d->aux_trip_lo_temp)
                aux_max = AUX_TRIP_LO_MAX_STATE;
            else
                aux_max = 0;

            d->fan_state = cur;

            if (cur == 0)
                strscpy(d->fan_driver_name, "none", MAX_NAME_LEN);
            else if (cur > aux_max)
                strscpy(d->fan_driver_name, "cpu", MAX_NAME_LEN);
            else
                strscpy(d->fan_driver_name, d->source_name, MAX_NAME_LEN);
        }
    }

    schedule_delayed_work(&d->poll_work, msecs_to_jiffies(poll_ms));
}

/* ── sysfs attributes (read-only) ────────────────────────────────────────── */

static ssize_t aux_temp_mc_show(struct device *dev,
                                 struct device_attribute *attr, char *buf)
{
    struct aux_sensor_data *d = dev_get_drvdata(dev);
    return sysfs_emit(buf, "%d\n", d->aux_temp_mc);
}
static DEVICE_ATTR_RO(aux_temp_mc);

static ssize_t source_show(struct device *dev,
                            struct device_attribute *attr, char *buf)
{
    struct aux_sensor_data *d = dev_get_drvdata(dev);
    return sysfs_emit(buf, "%s\n", d->source_name);
}
static DEVICE_ATTR_RO(source);

static ssize_t cpu_temp_mc_show(struct device *dev,
                                 struct device_attribute *attr, char *buf)
{
    struct aux_sensor_data *d = dev_get_drvdata(dev);
    return sysfs_emit(buf, "%d\n", d->cpu_temp_mc);
}
static DEVICE_ATTR_RO(cpu_temp_mc);

static ssize_t fan_state_show(struct device *dev,
                               struct device_attribute *attr, char *buf)
{
    struct aux_sensor_data *d = dev_get_drvdata(dev);
    return sysfs_emit(buf, "%d\n", d->fan_state);
}
static DEVICE_ATTR_RO(fan_state);

static ssize_t fan_driver_show(struct device *dev,
                                struct device_attribute *attr, char *buf)
{
    struct aux_sensor_data *d = dev_get_drvdata(dev);
    return sysfs_emit(buf, "%s\n", d->fan_driver_name);
}
static DEVICE_ATTR_RO(fan_driver);

static struct attribute *aux_sensor_attrs[] = {
    &dev_attr_aux_temp_mc.attr,
    &dev_attr_source.attr,
    &dev_attr_cpu_temp_mc.attr,
    &dev_attr_fan_state.attr,
    &dev_attr_fan_driver.attr,
    NULL,
};
ATTRIBUTE_GROUPS(aux_sensor);

/* ── Platform driver ─────────────────────────────────────────────────────── */

static int aux_probe(struct platform_device *pdev)
{
    struct aux_sensor_data *d;
    struct thermal_zone_device *tz;

    d = devm_kzalloc(&pdev->dev, sizeof(*d), GFP_KERNEL);
    if (!d)
        return -ENOMEM;

    d->aux_temp_mc     = SAFE_TEMP_MC;
    d->cpu_temp_mc     = SAFE_TEMP_MC;
    d->aux_trip_lo_temp = 55000;   /* overwritten by discover_aux_zone_trips */
    d->aux_trip_hi_temp = 80000;
    strscpy(d->source_name,     "none", MAX_NAME_LEN);
    strscpy(d->fan_driver_name, "none", MAX_NAME_LEN);
    d->dev = &pdev->dev;
    dev_set_drvdata(&pdev->dev, d);

    discover_all_paths(d);
    if (!d->temp_path_count)
        dev_info(&pdev->dev, "no temperature sources at probe; will retry\n");

    discover_fan_cdev(d);

    tz = devm_thermal_of_zone_register(&pdev->dev, 0, d, &aux_tz_ops);
    if (IS_ERR(tz))
        return dev_err_probe(&pdev->dev, PTR_ERR(tz),
                             "thermal zone registration failed "
                             "(is the DT overlay loaded?)\n");
    d->tz = tz;

    /* Zone is now in sysfs — read its trip points */
    discover_aux_zone_trips(d);

    INIT_DELAYED_WORK(&d->poll_work, aux_poll_fn);
    schedule_delayed_work(&d->poll_work, msecs_to_jiffies(poll_ms));

    dev_info(&pdev->dev, "aux thermal zone registered; poll interval %u ms\n",
             poll_ms);
    return 0;
}

static void aux_remove(struct platform_device *pdev)
{
    struct aux_sensor_data *d = dev_get_drvdata(&pdev->dev);
    cancel_delayed_work_sync(&d->poll_work);
}

static const struct of_device_id aux_sensor_of_match[] = {
    { .compatible = "x-fan40,aux-temp-sensor" },
    { },
};
MODULE_DEVICE_TABLE(of, aux_sensor_of_match);

static struct platform_driver aux_sensor_driver = {
    .probe  = aux_probe,
    .remove = aux_remove,
    .driver = {
        .name           = "x-fan40-aux-thermal",
        .of_match_table = aux_sensor_of_match,
        .dev_groups     = aux_sensor_groups,
    },
};

module_platform_driver(aux_sensor_driver);
