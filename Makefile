OVERLAY     := x-fan40
DTS         := $(OVERLAY)-overlay.dts
DTBO        := $(OVERLAY).dtbo
OVERLAY_DIR := /boot/firmware/overlays
CONFIG      := /boot/firmware/config.txt
KDIR        := /lib/modules/$(shell uname -r)/build

.PHONY: all overlay module test install install-module uninstall clean

all: overlay module

# ── Device Tree overlay ────────────────────────────────────────────────────

overlay: $(DTBO)

$(DTBO): $(DTS) Makefile
	cpp -nostdinc -I $(KDIR)/include -undef -x assembler-with-cpp $< | dtc -I dts -O dtb -@ -o $@ -

# ── Userspace validation tool ─────────────────────────────────────────────

test: test-fan40

test-fan40: test-fan40.c
	$(CC) -O2 -o $@ $<

# ── Kernel module (aux thermal zones for apex / nvme) ─────────────────────

module:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

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
	sudo $(MAKE) -C $(KDIR) M=$(CURDIR) modules_install
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
	rm -f $(DTBO) test-fan40
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
