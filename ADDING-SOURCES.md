# Adding new temperature sources

The module is designed so that adding a new device type requires changes in
only one place: `x-fan40-aux-thermal.c`.

## How it works

All temperature sources share a single array (`temp_paths[]` /
`temp_names[]`). The poll loop reads every entry and takes the maximum,
so it is completely agnostic about source type. Each source type needs
only a `discover_X_paths()` function that probes for the device and
appends readable entries to that array.

A source is eligible if its temperature is exposed as a plain integer
(milli-Celsius) in a sysfs file — the same format used by hwmon
(`temp1_input`) and the Apex driver (`temp`).

## Step-by-step

**1. Verify the sysfs path and format**

```bash
cat /sys/path/to/your/device/temp_file
# must print a single integer in milli-Celsius, e.g. 47000
```

**2. Write a `discover_X_paths()` function**

Follow the same pattern as `discover_apex_paths()` or
`discover_nvme_paths()`.  Simple case — one file per device, no hwmon
indirection (like Apex):

```c
#define MAX_MYDEV_DEVS  2

static void discover_mydev_paths(struct aux_sensor_data *d)
{
    char path[MAX_PATH_LEN];
    int n;

    for (n = 0; n < MAX_MYDEV_DEVS && d->temp_path_count < MAX_TEMP_SOURCES; n++) {
        struct file *f;

        snprintf(path, sizeof(path), "/sys/class/mydev/mydev%d/temp", n);
        f = filp_open(path, O_RDONLY, 0);
        if (!IS_ERR(f)) {
            filp_close(f, NULL);
            strscpy(d->temp_paths[d->temp_path_count], path, MAX_PATH_LEN);
            snprintf(d->temp_names[d->temp_path_count], MAX_NAME_LEN,
                     "mydev%d", n);
            d->temp_path_count++;
        }
    }
}
```

If the device exposes temperature through a dynamic hwmon child (like NVMe),
model the function on `discover_nvme_paths()` and add an inner loop over
`hwmon0..15` that breaks once a readable `temp1_input` is found.

**3. Call it from `discover_all_paths()`**

```c
static void discover_all_paths(struct aux_sensor_data *d)
{
    int i;

    d->temp_path_count = 0;
    discover_apex_paths(d);
    discover_nvme_paths(d);
    discover_mydev_paths(d);   /* ← add this line */

    for (i = 0; i < d->temp_path_count; i++)
        dev_info(d->dev, "discovered %s temperature source\n",
                 d->temp_names[i]);
}
```

**4. Check the constants**

| Constant | Current value | What it controls |
|----------|--------------|-----------------|
| `MAX_TEMP_SOURCES` | `MAX_APEX_DEVS + MAX_NVME_DEVS` | Total slots in the path array — increase if needed |
| `MAX_PATH_LEN` | `48` | Maximum sysfs path length — increase if your path is longer |
| `MAX_NAME_LEN` | `8` | Maximum source name length (e.g. `"mydev0"`) |

`MAX_TEMP_SOURCES` must be at least the sum of all `MAX_*_DEVS` values.
If your path (including the longest index) exceeds 47 characters, increase
`MAX_PATH_LEN` to the next appropriate value and update the comment.

**5. Rebuild and reinstall**

```bash
make
sudo make install
sudo modprobe -r x-fan40-aux-thermal
sudo modprobe x-fan40-aux-thermal
```

Confirm the new device appears:

```bash
cat /sys/devices/platform/x-fan40-aux-sensor/source
cat /sys/devices/platform/x-fan40-aux-sensor/aux_temp_mc
```
