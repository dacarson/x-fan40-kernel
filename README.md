# x-fan40-kernel

Kernel driver and device tree overlay for the
[SupTronics X-FAN40](https://suptronics.com/Raspberrypi/Storage/x-fan40-v1.0.html)
on Raspberry Pi 5.

## Overview

The [X-FAN40](https://suptronics.com/Raspberrypi/Storage/x-fan40-v1.0.html)
is a 4-pin PWM fan HAT designed for active cooling of M.2 NVMe drives on the
Raspberry Pi 5. This project integrates it into the Linux thermal framework so
the fan responds to all relevant heat sources without any user-space daemon:

- **CPU** — handled by the device tree overlay (fragment 2), which adds
  cooling maps to the Pi's existing `cpu_thermal` zone. No kernel module
  needed; the thermal framework drives the fan automatically from DT alone.
- **M.2 NVMe drives and Coral TPU accelerators** — handled by the
  `x-fan40-aux-thermal` kernel module, which polls their temperatures
  directly from `/sys/class/nvme` and `/sys/class/apex`.

| GPIO | Function |
|------|----------|
| GPIO13 | PWM output to fan (25 kHz, RP1 PWM1) |
| GPIO16 | Tachometer input (2 pulses / revolution) |

### Cooling states

| State | Duty | Speed |
|-------|------|-------|
| 0 | 0 % | Off |
| 1 | 30 % | Low |
| 2 | 40 % | Medium |
| 3 | 60 % | Medium-high |
| 4 | 80 % | High |
| 5 | 100 % | Full |

### Architecture

```
CPU thermal zone  ──────────────────────────────────────────────────┐
                                                                     ▼
Coral TPU  ──► /sys/class/apex/apex_N/temp ──┐
                                              ├──► x-fan40-aux-thermal.ko
NVMe       ──► /sys/class/nvme/nvmeN/...  ──┘     (aux thermal zone)
                                                           │
                                                           ▼
                                                  kernel max-wins arbitration
                                                           │
                                                           ▼
                                                    pwm-fan (GPIO13)
```

The CPU zone is configured entirely in the device tree overlay (fragment 2)
— the kernel thermal framework wires it to the fan with no module or daemon
involved. The aux zone is owned by `x-fan40-aux-thermal.ko`, which polls
Coral TPU and NVMe temperatures and reports the maximum. The framework then
applies max-wins arbitration: whichever zone demands more cooling wins.
No user-space daemon is required.

## Files

| File | Purpose |
|------|---------|
| `x-fan40-overlay.dts` | Device tree overlay: PWM, tachometer, CPU cooling maps, aux thermal zone |
| `x-fan40-aux-thermal.c` | Kernel module: polls Coral TPU and NVMe temperatures, drives aux thermal zone |
| `Kbuild` | Kernel build descriptor for the module |
| `Makefile` | Build and install rules |

## Requirements

- Raspberry Pi 5
- Raspberry Pi OS (Debian 12 Bookworm or Debian 13 Trixie)
- Kernel headers: `sudo apt install raspberrypi-kernel-headers`
- Device tree compiler: `sudo apt install device-tree-compiler`

## Installation

```bash
# Clone the repository
git clone git@github.com:dacarson/x-fan40-kernel.git
cd x-fan40-kernel

# Build the device tree overlay and kernel module
make

# Install both (requires sudo)
sudo make install

# Reboot to activate the device tree overlay
sudo reboot
```

After reboot, load the auxiliary thermal module:

```bash
sudo modprobe x-fan40-aux-thermal
```

To load it automatically at boot:

```bash
echo "x-fan40-aux-thermal" | sudo tee /etc/modules-load.d/x-fan40-aux.conf
```

## Kernel module: x-fan40-aux-thermal

`x-fan40-aux-thermal.ko` polls Coral TPU and NVMe temperatures directly from
their kernel sysfs class entries and feeds the maximum into the kernel thermal
framework as an auxiliary zone.

**Coral TPU** — discovered via `/sys/class/apex/apex_N/temp` (up to 2 devices).

**NVMe** — discovered via `/sys/class/nvme/nvmeN/hwmonM/temp1_input` (up to 4
drives). The hwmon index `M` is assigned dynamically by the kernel and is
found automatically.

Both source types are discovered at module load and re-tried on every poll
cycle, so devices that appear after the module loads are picked up
automatically.

### Module parameter

| Parameter | Default | Description |
|-----------|---------|-------------|
| `poll_ms` | `1000` | Temperature poll interval in milliseconds |

Override at load time:

```bash
sudo modprobe x-fan40-aux-thermal poll_ms=500
```

Or persist in `/etc/modprobe.d/x-fan40-aux.conf`:

```
options x-fan40-aux-thermal poll_ms=500
```

### Sysfs attributes

| Attribute | Access | Description |
|-----------|--------|-------------|
| `aux_temp_mc` | read | Hottest Apex/NVMe temperature in milli-Celsius — this is what drives the aux thermal zone |
| `source` | read | Name of the hottest Apex/NVMe device (`apex_0`, `nvme1`, etc.) |
| `cpu_temp_mc` | read | CPU temperature in milli-Celsius — observability only, does not drive the aux zone |

`cpu_temp_mc` is polled purely for visibility. CPU fan control is handled
independently by the device tree overlay via the kernel's `cpu_thermal` zone.

```bash
cat /sys/devices/platform/x-fan40-aux-sensor/aux_temp_mc
# → 65000   (65 °C, from nvme0 — drives aux zone)

cat /sys/devices/platform/x-fan40-aux-sensor/source
# → nvme0

cat /sys/devices/platform/x-fan40-aux-sensor/cpu_temp_mc
# → 78000   (78 °C — fan driven by DT overlay, not aux zone)
```

## sysfs layout after install

The `pwm-fan` driver registers an hwmon device, so the fan appears alongside
the Pi's built-in sensors under `/sys/class/hwmon/`:

```
/sys/class/hwmon/hwmonX/
  name        → "pwmfan"
  fan1_input  → current RPM (read, from GPIO16 tachometer)
  pwm1        → duty cycle 0-255 (read/write)
  pwm1_enable → control mode (read/write)
```

The fan also appears as a thermal cooling device:

```
/sys/class/thermal/cooling_deviceN/
  type        → "pwm-fan"
  cur_state   → current cooling state 0-5 (read/write)
  max_state   → 5
```

## Uninstall

```bash
sudo make uninstall
sudo reboot
```

This removes the `.dtbo` file, the `dtoverlay=x-fan40` line from
`/boot/firmware/config.txt`, and unloads the kernel module. Remove the
module auto-load conf manually if added:

```bash
sudo rm /etc/modules-load.d/x-fan40-aux.conf
```

## Troubleshooting

**`make` fails with `syntax error` on the `.dts` file**

The device tree compiler needs the C preprocessor to handle `#include`
directives. Ensure `raspberrypi-kernel-headers` is installed — the Makefile
runs `cpp` with the kernel include path automatically.

**Module loads but `aux_temp_mc` sysfs attribute is missing**

The DT overlay must be active before the module is loaded. Confirm with:

```bash
dtoverlay -l | grep x-fan40
```

If not listed, check `/boot/firmware/config.txt` for `dtoverlay=x-fan40`
and reboot.

**`source` reads `none` after module load**

No Apex or NVMe devices were found at probe time. Check that the Apex and
NVMe drivers are loaded:

```bash
ls /sys/class/apex/
ls /sys/class/nvme/
```

The module retries discovery on every poll cycle, so `source` will update
automatically once the devices appear.

**Fan does not spin after install**

Verify the overlay created a pwm-fan cooling device:

```bash
cat /sys/class/thermal/cooling_device*/type
```

One entry should read `pwm-fan`. If absent, the overlay did not load — check
`dmesg` for device tree errors.
