# x-fan40-kernel

[![License: MIT](https://img.shields.io/github/license/dacarson/x-fan40-kernel)](LICENSE)
[![Version](https://img.shields.io/github/v/release/dacarson/x-fan40-kernel)](https://github.com/dacarson/x-fan40-kernel/releases)
[![Downloads](https://img.shields.io/github/downloads/dacarson/x-fan40-kernel/total)](https://github.com/dacarson/x-fan40-kernel/releases)

Kernel driver and device tree overlay for the
[SupTronics X-FAN40](https://suptronics.com/Raspberrypi/Storage/x-fan40-v1.0.html)
on Raspberry Pi 5.

## Overview

The [X-FAN40](https://suptronics.com/Raspberrypi/Storage/x-fan40-v1.0.html)
is a 4-pin PWM fan HAT designed for active cooling of M.2 NVMe drives on the
Raspberry Pi 5. This project integrates it into the Linux thermal framework so
the fan responds to all relevant heat sources without any user-space daemon:

- **CPU** — handled by the device tree overlay (fragment 3), which adds
  cooling maps to the Pi's existing `cpu_thermal` zone. No kernel module
  needed; the thermal framework drives the fan automatically from DT alone.
- **M.2 NVMe drives and Coral TPU accelerators** — handled by the
  `x-fan40-aux-thermal` kernel module, which polls their temperatures
  directly from `/sys/class/nvme` and `/sys/class/apex`.

| GPIO | Function |
|------|----------|
| GPIO13 | PWM output to fan (25 kHz, RP1 PWM0 channel 1) |
| GPIO16 | Tachometer input (2 pulses / revolution, open-drain with pull-up) |

### Cooling states

| State | Duty | Speed |
|-------|------|-------|
| 0 | 0 % | Off |
| 1 | 30 % | Low |
| 2 | 40 % | Medium |
| 3 | 60 % | Medium-high |
| 4 | 80 % | High |
| 5 | 100 % | Full |

### How the thermal governor drives fan states

The kernel `step_wise` governor steps through cooling states one at a time
based on trip points. Each cooling map in the DT overlay specifies a trip
temperature and a `<min max>` state range the governor is allowed to use
once that trip fires:

| Zone | Trip | State range | Meaning |
|------|------|-------------|---------|
| aux (Apex/NVMe) | 55 °C | 1–3 | Fan starts at low and steps up to medium-high |
| aux (Apex/NVMe) | 80 °C | 4–5 | Fan steps up to high and full |
| cpu | 50 °C (`cpu_tepid`) | 1–2 | Fan starts at low |
| cpu | 67.5 °C | 3–4 | Fan steps up to medium-high |
| cpu | 75 °C | 5–5 | Fan goes to full |

- **Below the trip temperature** — the zone requests state 0 (off).
- **At or above the trip temperature** — the governor starts at the minimum
  state for that range and steps up one state per poll cycle while the
  temperature is still rising, or steps back down while it is falling. It
  settles at whichever state within the range stabilises the temperature.
- **Multiple trip points** — when the temperature crosses a second trip, the
  higher range becomes available and the governor can step into it.

The kernel applies **max-wins arbitration** across all zones: whichever zone
demands a higher state wins. For example, if the CPU zone wants state 4 but the
aux zone only wants state 2, the fan runs at state 4. The `fan_driver` sysfs
attribute reports which zone is responsible.

### Architecture

```
CPU thermal zone   ──────────────────────────────────────────────────┐
                                                                     ▼
Coral TPU  ──► /sys/class/apex/apex_N/temp  ──┐
                                              ├──► x-fan40-aux-thermal.ko
NVMe       ──► /sys/class/nvme/nvmeN/...    ──┘     (aux thermal zone)
                                                           │
                                                           ▼
                                                  kernel max-wins arbitration
                                                           │
                                                           ▼
                                                    x-fan (GPIO13)
```

The CPU zone is configured entirely in the device tree overlay (fragment 3)
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

## Installation

### Option 1 — CPU cooling only (pre-built overlay, no compilation)

If you only need CPU-driven fan control and do not have an NVMe or Coral TPU,
you can drop in the pre-built overlay without installing any kernel module.

1. Download `x-fan40.dtbo` from the [latest release](https://github.com/dacarson/x-fan40-kernel/releases/latest).
2. Copy it to the overlays directory:

```bash
sudo cp x-fan40.dtbo /boot/firmware/overlays/
```

3. Add the overlay to `/boot/firmware/config.txt`:

```bash
echo "dtoverlay=x-fan40" | sudo tee -a /boot/firmware/config.txt
```

4. Reboot.

### Option 2 — Full install with NVMe / Coral TPU support (DKMS)

DKMS rebuilds the kernel module automatically after kernel updates.

```bash
# Install prerequisites
sudo apt install dkms raspberrypi-kernel-headers device-tree-compiler

# Clone and install
git clone https://github.com/dacarson/x-fan40-kernel.git
cd x-fan40-kernel
sudo make install-overlay   # installs pre-built overlay and edits config.txt
sudo make install-dkms      # registers and builds the module via DKMS

# Reboot to activate the overlay
sudo reboot
```

After reboot, load the module and configure it to load automatically:

```bash
sudo modprobe x-fan40-aux-thermal
echo "x-fan40-aux-thermal" | sudo tee /etc/modules-load.d/x-fan40-aux.conf
```

To remove:

```bash
sudo make uninstall-dkms
sudo make uninstall
sudo reboot
```

### Option 3 — Build from source

```bash
# Prerequisites
sudo apt install raspberrypi-kernel-headers device-tree-compiler

# Clone the repository
git clone https://github.com/dacarson/x-fan40-kernel.git
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

## Requirements

- Raspberry Pi 5
- Raspberry Pi OS (Debian 12 Bookworm or Debian 13 Trixie)
- For build/DKMS: `sudo apt install raspberrypi-kernel-headers device-tree-compiler`

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
| `aux_temp` | read | Hottest Apex/NVMe temperature in milli-Celsius — this is what drives the aux thermal zone |
| `source` | read | Name of the hottest Apex/NVMe device (`apex_0`, `nvme1`, etc.) |
| `fan_state` | read | Current pwm-fan cooling state (0–5) |
| `fan_driver` | read | Zone responsible for the current fan speed: `cpu`, a source name (e.g. `nvme0`), or `none` |

`fan_driver` is inferred by comparing the actual fan state against the maximum
state the aux zone could demand at the current aux temperature — if the fan is
running faster than the aux zone requires, the CPU zone must be responsible.

```bash
cat /sys/devices/platform/x-fan40-aux-sensor/aux_temp
# → 65000   (65 °C, from nvme0 — drives aux zone)

cat /sys/devices/platform/x-fan40-aux-sensor/source
# → nvme0

cat /sys/devices/platform/x-fan40-aux-sensor/fan_state
# → 4

cat /sys/devices/platform/x-fan40-aux-sensor/fan_driver
# → cpu
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

### Reading fan RPM

The X-FAN40 and the Pi 5's built-in active cooler both appear as `pwmfan`
hwmon devices. To see the RPM of both at once:

```bash
for d in /sys/class/hwmon/hwmon*/; do
  name=$(cat "$d/name" 2>/dev/null)
  rpm=$(cat "$d/fan1_input" 2>/dev/null || echo "no tach")
  echo "$name: $rpm RPM  ($d)"
done
```

The X-FAN40 is specifically at `/sys/devices/platform/x-fan/hwmon/hwmon*/fan1_input`.
The `x-fan40-aux-thermal` module finds it by that path (the `x-fan` DT node
name is unique). The thermal cooling device is identified by `max_state` 5,
which differs from the Pi 5 built-in fan (max_state 4).

## Adding new temperature sources

See [ADDING-SOURCES.md](ADDING-SOURCES.md) for a step-by-step guide.

## Uninstall

For DKMS installs:

```bash
sudo make uninstall-dkms
sudo make uninstall
sudo reboot
```

For source/manual installs:

```bash
sudo make uninstall
sudo reboot
```

`make uninstall` removes the `.dtbo` file and the `dtoverlay=x-fan40` line
from `/boot/firmware/config.txt`. Remove the module auto-load conf manually
if added:

```bash
sudo rm /etc/modules-load.d/x-fan40-aux.conf
```

## Troubleshooting

**`make` fails with `syntax error` on the `.dts` file**

The device tree compiler needs the C preprocessor to handle `#include`
directives. Ensure `raspberrypi-kernel-headers` is installed — the Makefile
runs `cpp` with the kernel include path automatically.

**Module loads but `aux_temp` sysfs attribute is missing**

The DT overlay must be active before the module is loaded. Confirm with:

```bash
dtoverlay -l | grep x-fan40
```

On Pi 5 with kernel 6.12, `dtoverlay -l` does not list overlays applied
at boot time via `config.txt`, even when they are working correctly.
Verify the overlay is active by checking for its sysfs devices instead:

```bash
ls /sys/devices/platform/x-fan/hwmon/
ls /sys/devices/platform/x-fan40-aux-sensor/
```

If those paths are missing, check `/boot/firmware/config.txt` for
`dtoverlay=x-fan40` and reboot.

**`source` reads `none` after module load**

No Apex or NVMe devices were found at probe time. Check that the Apex and
NVMe drivers are loaded:

```bash
ls /sys/class/apex/
ls /sys/class/nvme/
```

The module retries discovery on every poll cycle, so `source` will update
automatically once the devices appear.

**Overlay not working after reboot**

On Pi 5 with kernel 6.12, `dtoverlay -l` does not list boot-time overlays,
so its output cannot confirm whether the overlay applied. Use sysfs instead
(see above). If the sysfs paths are missing, the overlay failed to apply.
Run `sudo dtoverlay -v x-fan40` to attempt a manual load and check `dmesg`
for the error. Common causes on Pi 5 kernel 6.12:

- `node label 'rp1_pwm1_gpio13' not found` — the GPIO13 pinctrl node is
  defined inline in the overlay (fragment 0) for this reason; if you see this
  error the overlay version pre-dates the fix.
- `node label 'cpu_trip1' not found` — the BCM2712 base DT only exports
  `cpu_tepid` as a labeled CPU trip point; fragment 3 now uses that label and
  defines its own nodes for the higher trip temperatures.

Both are fixed in current source; rebuild and reinstall the overlay.

**`fan1_input` is missing from the hwmon directory**

The tachometer entry only appears when the `pwm-fan` driver successfully
registers an interrupt on GPIO16. Check that the overlay is current:

```bash
strings /boot/firmware/overlays/x-fan40.dtbo | grep -E 'interrupts-extended|rp1_tach'
```

You should see `interrupts-extended` and `rp1_tach_gpio16`. If you see
`fan-tach-gpios` instead, rebuild with `make clean && make overlay &&
sudo make install-overlay` and reboot.

> **Note for kernel maintainers:** The Raspberry Pi 6.12 `pwm-fan` driver
> diverges from mainline. It discovers tachometer inputs via
> `platform_irq_count()`, which reads the `interrupts`/`interrupts-extended`
> DT property. The mainline `fan-tach-gpios` property is silently ignored.
> Use `interrupts-extended = <&gpio 16 IRQ_TYPE_EDGE_FALLING>` to wire the
> tachometer.

**Fan does not spin after install**

Verify the overlay created a pwm-fan cooling device:

```bash
cat /sys/class/thermal/cooling_device*/type
```

One entry should read `pwm-fan`. If absent, the overlay did not load — check
`dmesg` for device tree errors.
