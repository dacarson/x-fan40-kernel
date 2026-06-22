// SPDX-License-Identifier: GPL-2.0
// x-fan40-aux-thermal.c
//
// Platform driver that exposes a virtual thermal sensor for auxiliary heat
// sources (Coral TPU, NVMe) that have no standard kernel thermal zone.
//
// fan_controller.py reads apex/nvme temperatures and writes the maximum
// to this driver's sysfs attribute.  The kernel thermal framework then
// drives the pwm-fan cooling device for this zone with the same max-wins
// arbitration it uses for the CPU zone — no daemon-vs-governor conflict.
//
// Sysfs write path (milli-Celsius):
//   /sys/devices/platform/x-fan40-aux-sensor/temp_mc
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

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Virtual thermal sensor for x-fan40 auxiliary heat sources");
MODULE_AUTHOR("x-fan40");

struct aux_sensor_data {
    int                        temp_mc;   /* milli-Celsius; written by userspace */
    struct thermal_zone_device *tz;
};

/* ── Thermal zone callback ───────────────────────────────────────────────── */

static int aux_get_temp(struct thermal_zone_device *tz, int *temp)
{
    struct aux_sensor_data *d = thermal_zone_device_priv(tz);
    *temp = d->temp_mc;
    return 0;
}

static const struct thermal_zone_device_ops aux_tz_ops = {
    .get_temp = aux_get_temp,
};

/* ── sysfs: /sys/devices/platform/x-fan40-aux-sensor/temp_mc ────────────── */

static ssize_t temp_mc_show(struct device *dev,
                             struct device_attribute *attr, char *buf)
{
    struct aux_sensor_data *d = dev_get_drvdata(dev);
    return sysfs_emit(buf, "%d\n", d->temp_mc);
}

static ssize_t temp_mc_store(struct device *dev,
                              struct device_attribute *attr,
                              const char *buf, size_t count)
{
    struct aux_sensor_data *d = dev_get_drvdata(dev);
    int val, ret;

    ret = kstrtoint(buf, 10, &val);
    if (ret)
        return ret;

    d->temp_mc = val;
    /* Poke the thermal governor so it re-evaluates immediately. */
    thermal_zone_device_update(d->tz, THERMAL_EVENT_UNSPECIFIED);
    return count;
}

static DEVICE_ATTR_RW(temp_mc);

static struct attribute *aux_sensor_attrs[] = {
    &dev_attr_temp_mc.attr,
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

    d->temp_mc = 25000;   /* 25 °C — below all trip points, fan off */
    dev_set_drvdata(&pdev->dev, d);

    /*
     * Register with the kernel thermal framework using the OF binding.
     * devm_thermal_of_zone_register() scans thermal-zones DT nodes for one
     * that lists this device under thermal-sensors, then creates the zone
     * (trip points, cooling-maps, governor) entirely from DT.
     * Requires fragments 3 & 4 in x-fan40-overlay.dts.
     */
    tz = devm_thermal_of_zone_register(&pdev->dev, 0, d, &aux_tz_ops);
    if (IS_ERR(tz))
        return dev_err_probe(&pdev->dev, PTR_ERR(tz),
                             "thermal zone registration failed "
                             "(is the DT overlay loaded?)\n");

    d->tz = tz;
    dev_info(&pdev->dev, "aux thermal zone registered\n");
    return 0;
}

static const struct of_device_id aux_sensor_of_match[] = {
    { .compatible = "x-fan40,aux-temp-sensor" },
    { },
};
MODULE_DEVICE_TABLE(of, aux_sensor_of_match);

static struct platform_driver aux_sensor_driver = {
    .probe  = aux_probe,
    .driver = {
        .name           = "x-fan40-aux-thermal",
        .of_match_table = aux_sensor_of_match,
        .dev_groups     = aux_sensor_groups,   /* registers temp_mc sysfs attr */
    },
};

module_platform_driver(aux_sensor_driver);
