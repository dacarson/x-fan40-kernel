OVERLAY     := x-fan40
DTS         := $(OVERLAY)-overlay.dts
DTBO        := $(OVERLAY).dtbo
OVERLAY_DIR := /boot/firmware/overlays
CONFIG      := /boot/firmware/config.txt
KDIR        := /lib/modules/$(shell uname -r)/build

.PHONY: all overlay module install install-module uninstall clean

all: overlay module

# ── Device Tree overlay ────────────────────────────────────────────────────

overlay: $(DTBO)

$(DTBO): $(DTS)
	dtc -I dts -O dtb -o $@ $<

# ── Kernel module (aux thermal zones for apex / nvme) ─────────────────────

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# ── Install ────────────────────────────────────────────────────────────────

install: install-overlay install-module

install-overlay: $(DTBO)
	sudo cp $(DTBO) $(OVERLAY_DIR)/
	@if grep -q "dtoverlay=$(OVERLAY)" $(CONFIG); then \
		echo "dtoverlay=$(OVERLAY) already in $(CONFIG)"; \
	else \
		echo "dtoverlay=$(OVERLAY)" | sudo tee -a $(CONFIG); \
		echo "Added dtoverlay=$(OVERLAY) to $(CONFIG)"; \
	fi
	@echo "Reboot to activate the overlay."

install-module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	sudo depmod -a
	@echo "Module installed. Load with: sudo modprobe x-fan40-aux-thermal"

# ── Uninstall ──────────────────────────────────────────────────────────────

uninstall:
	sudo rmmod x-fan40-aux-thermal 2>/dev/null || true
	sudo rm -f $(OVERLAY_DIR)/$(DTBO)
	sudo sed -i '/dtoverlay=$(OVERLAY)/d' $(CONFIG)
	@echo "Removed $(OVERLAY). Reboot to fully deactivate overlay."

# ── Clean ──────────────────────────────────────────────────────────────────

clean:
	rm -f $(DTBO)
	$(MAKE) -C $(KDIR) M=$(PWD) clean
