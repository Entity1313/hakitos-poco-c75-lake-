# HakiOS device build for the POCO C75 4G / Redmi 14C (lake, MediaTek MT6768 / Helio G81 Ultra).
#
#   make lake_defconfig   read configs/lake_defconfig into .config and include/generated/
#   make kernel           build XNU when HAKIOS_KERNEL_TREE is a prepared common tree, otherwise
#                         stage this device's drivers and configuration for it (build/kernel/)
#   make loader           mtkloader/mtkloader.bin (mtkloader/build.sh)
#   make bootimg          pack mtkloader + Android's boot image + XNU payload into build/boot.img
#   make clean
#
# The shared kernel (XNU sources, the common drivers, the iOS SDK glue) deliberately does not live
# in this repository. Point HAKIOS_KERNEL_TREE at a prepared checkout of
# https://github.com/oosexgang/hakios-xnu-kernel (branch mtk) for a real `make kernel`; without it
# the kernel target stages devices/lake/ instead of pretending a kernel was built.
#
#   HAKIOS_KERNEL_TREE=../hakios make lake_defconfig kernel
#
# Variables: DEVICE (lake), JOBS, HAKIOS_CONFIG_DIR, ANDROID_BOOT, XNU_PAYLOAD, BOOT_PARTITION.

DEVICE ?= lake
JOBS ?= 8
PYTHON ?= python3
HAKIOS_KERNEL_TREE ?=
HAKIOS_CONFIG_DIR ?=
ANDROID_BOOT ?= backup/boot_a_magisk.img
XNU_PAYLOAD ?= build-payload.bin
BOOT_OUT ?= build/boot.img
BUILD ?= build
# boot_a's size, from the resolved configuration: 64 MB on the POCO C75, 32 on the Infinix.
BOOT_PARTITION ?= $(shell sed -n 's/^CONFIG_BOOT_PARTITION=//p' .config 2>/dev/null)

# Where the common fragments may live, most specific first. tools/mkconfig.py skips
# common_defconfig/mt6768.config with a warning when none of these has them.
CONFIG_INCLUDE_DIRS := $(HAKIOS_CONFIG_DIR)
ifneq ($(HAKIOS_KERNEL_TREE),)
CONFIG_INCLUDE_DIRS := $(CONFIG_INCLUDE_DIRS):$(HAKIOS_KERNEL_TREE)/configs
CONFIG_INCLUDE_DIRS := $(CONFIG_INCLUDE_DIRS):$(HAKIOS_KERNEL_TREE)/mtk/mt6768/configs
CONFIG_INCLUDE_DIRS := $(CONFIG_INCLUDE_DIRS):$(HAKIOS_KERNEL_TREE)/mtk/configs
endif
CONFIG_INCLUDE_DIRS := $(CONFIG_INCLUDE_DIRS):configs

BOARD_FLAG := $(shell grep -q '^CONFIG_DEVICE="x6826b"' .config 2>/dev/null && echo -DBOARD_X6826B=1 || echo -DBOARD_LAKE=1)

DEFCONFIGS := $(patsubst configs/%_defconfig,%,$(wildcard configs/*_defconfig))

.PHONY: help kernel kernel-stage loader bootimg clean check $(addsuffix _defconfig,$(DEFCONFIGS))

help:
	@sed -n '2,16p' Makefile | sed 's/^# \{0,1\}//'

# Any configs/<name>_defconfig, so a new device needs no Makefile edit.
$(addsuffix _defconfig,$(DEFCONFIGS)): %_defconfig: configs/%_defconfig
	$(PYTHON) tools/mkconfig.py \
		--defconfig configs/$*_defconfig \
		--out .config \
		--header include/generated/$*_config.h \
		--include-dir "$(CONFIG_INCLUDE_DIRS)"
	@echo "configuration: $$(grep '^CONFIG_DEVICE=' .config || echo 'CONFIG_DEVICE unset')"

.config:
	@echo "make: no .config: run 'make $(DEVICE)_defconfig' first" >&2
	@exit 1

check: .config
	@awk '!/^#/ && NF && !/^CONFIG_[A-Za-z0-9_]+=/ { print "bad line in .config: " $$0; bad = 1 } END { exit bad }' .config

# A real kernel is built where the kernel lives. When that tree is not here, stage what this
# repository owns: the device's drivers and its resolved configuration, ready to drop into
# devices/lake/ of the common tree. The warning is on stderr and stays in the CI log.
kernel: .config
	@if [ -n "$(HAKIOS_KERNEL_TREE)" ] && [ -x "$(HAKIOS_KERNEL_TREE)/xnu-build.sh" ]; then \
		echo "==> kernel: $(HAKIOS_KERNEL_TREE)/xnu-build.sh"; \
		"$(HAKIOS_KERNEL_TREE)/xnu-build.sh" -j$(JOBS) -k; \
	elif [ -n "$(HAKIOS_KERNEL_TREE)" ]; then \
		echo "make kernel: $(HAKIOS_KERNEL_TREE) has no executable xnu-build.sh" >&2; \
		exit 1; \
	else \
		echo "make kernel: HAKIOS_KERNEL_TREE is not set; the shared XNU tree is not in this" >&2; \
		echo "             repository. Staging this device's kernel inputs in $(BUILD)/kernel/" >&2; \
		echo "             Set HAKIOS_KERNEL_TREE=path/to/hakios-xnu-kernel for a real kernel." >&2; \
		$(MAKE) --no-print-directory kernel-stage; \
	fi

kernel-stage: .config
	@DEV=$$(sed -n 's/^CONFIG_DEVICE="\(.*\)"/\1/p' .config); \
	rm -rf "$(BUILD)/kernel"; \
	mkdir -p "$(BUILD)/kernel/drivers" "$(BUILD)/kernel/config"; \
	cp -a kernel/. "$(BUILD)/kernel/drivers/"; \
	cp -a ft8057_tcm.c ft8057_tcm.h "$(BUILD)/kernel/drivers/"; \
	cp .config "$(BUILD)/kernel/config/.config"; \
	cp "configs/$$DEV.config" "configs/$${DEV}_defconfig" "$(BUILD)/kernel/config/"; \
	{ \
		echo "HakiOS kernel staging for $$DEV"; \
		echo; \
		echo "The shared XNU tree was not available, so this is not a mach kernel."; \
		echo "Copy drivers/ into mtk/mt6768/drivers/ of hakios-xnu-kernel (branch mtk)"; \
		echo "and configs/ to its configs/, or build there with:"; \
		echo; \
		echo "  make HAKIOS_KERNEL_TREE=path/to/hakios-xnu-kernel $$DEV""_defconfig kernel"; \
		echo; \
		echo "ft8057_tcm.c is the FocalTech FT8057M backend for hakios.tcm.c."; \
	} > "$(BUILD)/kernel/README.md"; \
	echo "==> kernel staged: $(BUILD)/kernel/ (drivers, .config; not a mach kernel)"

loader: .config
	@echo "==> loader: mtkloader/build.sh ($(BOARD_FLAG))"
	@BOARD=$$(grep -q '^CONFIG_DEVICE="x6826b"' .config && echo x6826b || echo lake) \
		mtkloader/build.sh $(BOARD_FLAG)

bootimg: loader
	@test -f "$(ANDROID_BOOT)" || { \
		echo "make bootimg: no $(ANDROID_BOOT)" >&2; \
		echo "  put your stock boot_a image there (never committed) or set ANDROID_BOOT=..." >&2; \
		exit 1; }
	@test -f "$(XNU_PAYLOAD)" || { \
		echo "make bootimg: no $(XNU_PAYLOAD) (xnu/apply.sh + xnu-build.sh + mkpayload.py)" >&2; \
		exit 1; }
	@mkdir -p "$(BUILD)"
	XNU_PAYLOAD="$(XNU_PAYLOAD)" BOOT_OUT="$(BOOT_OUT)" BOOT_PARTITION="$(BOOT_PARTITION)" \
		$(PYTHON) mtkloader/build_flashed.py "$(ANDROID_BOOT)"
	@ls -la "$(BOOT_OUT)"

clean:
	rm -rf "$(BUILD)" include/generated .config
	rm -f mtkloader/*.o mtkloader/*.bin mtkloader/touchfw.h mtkloader/font8x16.h mtkloader/identity.h
	rm -f xnu-flashed-slot_a.img build-payload.bin
