#!/usr/bin/env python3
"""
SupTronics X-FAN40 auxiliary temperature bridge for Raspberry Pi 5
Compatible with Debian 13 (trixie)

Reads temperatures from Coral TPU (apex0/apex1) and NVMe — sources the
kernel has no standard thermal zone for — and feeds the maximum to the
x-fan40-aux-thermal kernel module's virtual sensor.  The kernel thermal
framework then drives the fan from that zone the same way it drives it
from the CPU zone, using max-wins arbitration across all bound zones.

Requires:
  - x-fan40 DT overlay (fragments 2, 3, 4 active)
  - x-fan40-aux-thermal.ko loaded

Fallback (module not loaded): boost-only mode writes directly to
cur_state when aux temperatures exceed what the CPU zone has set, and
releases the boost when aux sources cool.  This can cause brief
oscillation if the kernel governor runs between daemon polls.
"""

import glob
import logging
import os
import signal
import time

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(message)s",
)
logger = logging.getLogger(__name__)


# ── Configuration ──────────────────────────────────────────────────────────
#
# Cooling states 0-5 are mapped to PWM duty cycles by the DT overlay:
#   state : 0    1    2    3    4    5
#   duty  : 0%  30%  40%  60%  80% 100%   (cooling-levels = <0 77 102 153 204 255>)
#
# CPU is intentionally absent — the kernel thermal governor owns it via the
# DT overlay (fragment@2).  Add "cpu" here only if you disable fragment@2.
#
# Each source entry:
#   name        : unique key used by read_source_temp()
#   enabled     : set False to exclude a source without deleting its config
#   thresholds  : [(min_temp_°C, fan_state), ...] sorted ascending by temp
#                 The highest threshold whose min_temp ≤ current temp wins.
#                 Below the lowest threshold the contribution is 0.

SOURCES = [
    {
        "name":       "apex0",
        "enabled":    True,
        "thresholds": [
            (50, 1),
            (65, 2),
            (75, 3),
            (85, 4),
            (90, 5),
        ],
    },
    {
        "name":       "apex1",
        "enabled":    True,
        "thresholds": [
            (50, 1),
            (65, 2),
            (75, 3),
            (85, 4),
            (90, 5),
        ],
    },
    {
        "name":       "nvme",
        "enabled":    True,
        "thresholds": [
            (40, 1),
            (50, 2),
            (55, 3),
            (60, 4),
            (65, 5),
        ],
    },
]

POLL_INTERVAL = 3  # seconds between temperature checks

# Path written by this daemon when x-fan40-aux-thermal.ko is loaded.
# The kernel thermal framework reads it and drives the fan automatically.
AUX_SENSOR_PATH = "/sys/devices/platform/x-fan40-aux-sensor/temp_mc"

# Coral TPU sysfs paths (stable, determined by PCIe topology)
APEX_PATHS = [
    "/sys/devices/platform/axi/1000110000.pcie/pci0001:00"
    "/0001:00:00.0/0001:01:00.0/0001:02:07.0/0001:04:00.0"
    "/0001:05:03.0/0001:06:00.0/apex/apex_0",

    "/sys/devices/platform/axi/1000110000.pcie/pci0001:00"
    "/0001:00:00.0/0001:01:00.0/0001:02:07.0/0001:04:00.0"
    "/0001:05:07.0/0001:07:00.0/apex/apex_1",
]

PWM_PIN_FALLBACK = 13   # GPIO pin used when neither sysfs backend is found


# ── Temperature readers ────────────────────────────────────────────────────

def _read_millideg(path: str) -> float:
    with open(path) as f:
        return int(f.read().strip()) / 1000.0


def _find_hwmon(name: str) -> str | None:
    for d in glob.glob("/sys/class/hwmon/hwmon*"):
        try:
            with open(os.path.join(d, "name")) as f:
                if f.read().strip() == name:
                    return d
        except OSError:
            pass
    return None


def read_source_temp(name: str) -> float | None:
    """Return temperature in °C for the named source, or None on read error."""
    try:
        if name == "cpu":
            return _read_millideg("/sys/class/thermal/thermal_zone0/temp")

        if name in ("apex0", "apex1"):
            idx = int(name[-1])
            return _read_millideg(os.path.join(APEX_PATHS[idx], "temp"))

        if name == "nvme":
            hwmon = _find_hwmon("nvme")
            if hwmon:
                return _read_millideg(os.path.join(hwmon, "temp1_input"))
            return None

    except Exception as exc:
        logger.debug("read_source_temp(%s): %s", name, exc)
    return None


# ── State calculation ──────────────────────────────────────────────────────

def temp_to_state(temp: float, thresholds: list) -> int:
    """Map temperature to cooling state using sorted threshold list."""
    state = 0
    for min_temp, s in thresholds:
        if temp >= min_temp:
            state = s
    return state


def compute_aux_temps() -> tuple[float, str]:
    """
    Read every enabled aux source and return (max_temp_°C, driving_source).
    Does not include CPU — the kernel thermal governor owns that via DT.
    """
    max_temp = 0.0
    driver = "none"
    for src in SOURCES:
        if not src["enabled"]:
            continue
        temp = read_source_temp(src["name"])
        if temp is None:
            continue
        logger.debug("  %-8s  %5.1f °C", src["name"], temp)
        if temp > max_temp:
            max_temp = temp
            driver = f"{src['name']} ({temp:.1f} °C)"
    return max_temp, driver


def compute_aux_state() -> tuple[int, str]:
    """
    Return (highest_cooling_state, driving_source) for boost-only fallback mode.
    Used only when x-fan40-aux-thermal.ko is not loaded.
    """
    desired = 0
    driver = "none"
    for src in SOURCES:
        if not src["enabled"]:
            continue
        temp = read_source_temp(src["name"])
        if temp is None:
            continue
        state = temp_to_state(temp, src["thresholds"])
        if state > desired:
            desired = state
            driver = f"{src['name']} ({temp:.1f} °C)"
    return desired, driver


# ── Fan backends ───────────────────────────────────────────────────────────

def _find_cooling_device() -> str | None:
    """Return /sys/class/thermal/cooling_deviceN path for the pwm-fan, or None."""
    for cd_dir in glob.glob("/sys/class/thermal/cooling_device*"):
        try:
            with open(os.path.join(cd_dir, "type")) as f:
                if f.read().strip() == "pwm-fan":
                    return cd_dir
        except OSError:
            pass
    return None


class CoolingDeviceFan:
    """Writes to /sys/class/thermal/cooling_deviceN/cur_state (preferred)."""

    def __init__(self, cd_path: str):
        self._cur_state = os.path.join(cd_path, "cur_state")
        with open(os.path.join(cd_path, "max_state")) as f:
            self.max_state = int(f.read().strip())
        logger.info("Fan backend: cooling_device %s (max_state=%d)", cd_path, self.max_state)

    def get_state(self) -> int:
        with open(self._cur_state) as f:
            return int(f.read().strip())

    def set_state(self, state: int):
        value = max(0, min(self.max_state, state))
        with open(self._cur_state, "w") as f:
            f.write(f"{value}\n")

    def close(self):
        pass   # leave cur_state as-is; kernel governor takes back control


class SysfsPWMFan:
    """Writes 0-255 directly to hwmon pwm1 when no cooling_device is present."""

    MAX_STATE = 5
    # Must mirror the cooling-levels array in the DT overlay
    _LEVELS = [0, 77, 102, 153, 204, 255]

    def __init__(self, pwm1_path: str):
        self._pwm1 = pwm1_path
        # Switch to manual PWM mode if the driver supports it
        enable_path = pwm1_path + "_enable"
        if os.path.exists(enable_path):
            try:
                with open(enable_path, "w") as f:
                    f.write("1\n")
            except OSError:
                pass
        logger.info("Fan backend: hwmon pwm1 %s", pwm1_path)

    def set_state(self, state: int):
        value = self._LEVELS[max(0, min(self.MAX_STATE, state))]
        with open(self._pwm1, "w") as f:
            f.write(f"{value}\n")

    def close(self):
        self.set_state(0)


class GpiozeroFan:
    """Last-resort fallback: direct gpiozero PWM (no sysfs presence)."""

    MAX_STATE = 5
    # Fractional duty cycles matching cooling-levels intent
    _DUTIES = [0.0, 0.30, 0.40, 0.60, 0.80, 1.0]

    def __init__(self, pin: int):
        from gpiozero import PWMOutputDevice
        self._dev = PWMOutputDevice(pin)
        logger.info("Fan backend: gpiozero GPIO%d (no DT overlay detected)", pin)

    def set_state(self, state: int):
        self._dev.value = self._DUTIES[max(0, min(self.MAX_STATE, state))]

    def close(self):
        self._dev.value = 0.0
        self._dev.close()


def _make_fan():
    cd = _find_cooling_device()
    if cd:
        return CoolingDeviceFan(cd)

    hwmon = _find_hwmon("pwmfan")
    if hwmon:
        pwm1 = os.path.join(hwmon, "pwm1")
        if os.path.exists(pwm1):
            return SysfsPWMFan(pwm1)

    return GpiozeroFan(PWM_PIN_FALLBACK)


# ── Controller ─────────────────────────────────────────────────────────────

class FanController:
    def __init__(self):
        self._shutdown = False
        self._fan = _make_fan()
        self._boosting = False
        self._module_mode = os.path.exists(AUX_SENSOR_PATH)
        if self._module_mode:
            logger.info("Module mode: feeding temperatures to %s", AUX_SENSOR_PATH)
        else:
            logger.info("Fallback mode: boost-only cur_state writes "
                        "(load x-fan40-aux-thermal.ko for clean kernel integration)")
        signal.signal(signal.SIGTERM, self._on_signal)
        signal.signal(signal.SIGINT, self._on_signal)

    def _on_signal(self, signum, frame):
        self._shutdown = True
        self._fan.close()

    def _run_module_mode(self):
        """Feed max aux temperature to the kernel module's virtual sensor."""
        max_temp, driver = compute_aux_temps()
        temp_mc = int(max_temp * 1000)
        try:
            with open(AUX_SENSOR_PATH, "w") as f:
                f.write(f"{temp_mc}\n")
            if max_temp > 0:
                logger.debug("Aux sensor: %d m°C  [%s]", temp_mc, driver)
        except OSError as exc:
            logger.warning("Cannot write to aux sensor (%s); switching to fallback", exc)
            self._module_mode = False

    def _run_boost_mode(self):
        """
        Boost-only fallback: write cur_state only if aux sources demand
        more cooling than the kernel has already set for CPU.
        The kernel's next governor cycle will reduce the state once aux
        sources cool down.
        """
        aux_state, driver = compute_aux_state()

        kernel_state = 0
        if hasattr(self._fan, "get_state"):
            try:
                kernel_state = self._fan.get_state()
            except OSError:
                pass

        if aux_state > kernel_state:
            if not self._boosting:
                logger.info("Boost: state %d → %d  [%s]",
                            kernel_state, aux_state, driver)
                self._boosting = True
            self._fan.set_state(aux_state)
        elif self._boosting:
            logger.info("Aux cool; releasing boost (kernel at state %d)", kernel_state)
            self._boosting = False

    def run(self):
        while not self._shutdown:
            if self._module_mode:
                self._run_module_mode()
            else:
                self._run_boost_mode()
            time.sleep(POLL_INTERVAL)


if __name__ == "__main__":
    FanController().run()
