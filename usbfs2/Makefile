SHELL := /bin/bash
# usbfs2: READ-ONLY USB mass-storage filesystems for Switch 2 homebrew (libounce): build/libusbfs2.a + the standalone
# test NRO test/usbfs2test.nro, and `make check` (checks.py + the host test). From homebrew/usbfs2:
#   test/mkimages.sh                       # once, ON THE BUILD MACHINE (needs dosfstools/mtools/exfatprogs/sfdisk/...)
#   podman run --rm --network=none -v "$PWD:/build/usbfs2:Z" -w /build/usbfs2 ounce-devkit make all check repro
#   ./mutants.sh                           # (same container) every safety rule is enforced: each mutant must fail
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment")
endif
# XFER64K=1 builds the 64 KiB-transfer variant (-DUSBFS2_XFER_64K: 0x11000 IN bind, 64 KiB READ(10)) into build-64k/
# and test/usbfs2test-x64k.nro; `make check` then pins 64 KiB instead of the default one-page cap. The reviewed 64 KiB
# NRO (4cfdd338) is kept as test/usbfs2test-64k.nro for its own later flight pair.
XFER64K ?= 0
ifeq ($(XFER64K),1)
BUILD := build-64k
NRO := test/usbfs2test-x64k.nro
XFERDEF := -DUSBFS2_XFER_64K
else
BUILD := build
NRO := test/usbfs2test.nro
XFERDEF :=
endif
PREFIX := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-
include $(DEVKITPRO)/libounce/switch2_cpu.mk
ARCH := $(SWITCH2_CPU) -mtp=soft -fPIE
INCLUDES := -Iinclude -Isource -Ithird_party/fatfs -I$(DEVKITPRO)/portlibs/switch2/include -I$(DEVKITPRO)/libounce/include
CFLAGS := $(ARCH) -std=gnu11 -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -D__SWITCH__ -D__SWITCH2__ $(XFERDEF) \
    $(INCLUDES)
LDFLAGS := $(ARCH) -specs=$(DEVKITPRO)/libounce/switch.specs -Wl,--gc-sections
LIBS := -L$(DEVKITPRO)/portlibs/switch2/lib -L$(DEVKITPRO)/libounce/lib -lnx
HOSTCC := gcc
HOSTCFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    -Iinclude -Isource -Ithird_party/fatfs -Itest $(XFERDEF)

# pure layers (also built for the host test) + console-only layers (usb:hs IPC, devoptab, orchestration)
PURE := bot blk part mountset vfs diskio ffsystem ulog
DEVICE := usbhs2 devoptab usbfs2
FATFS := ff ffunicode
LIB_OBJS := $(addprefix $(BUILD)/,$(addsuffix .o,$(PURE) $(DEVICE) $(FATFS)))
HDRS := $(wildcard source/*.h include/*.h third_party/fatfs/*.h)

all: $(BUILD)/libusbfs2.a $(NRO)

$(BUILD):
	mkdir -p $@
$(BUILD)/%.o: source/%.c $(HDRS) | $(BUILD)
	$(PREFIX)gcc $(CFLAGS) -c $< -o $@
$(BUILD)/%.o: third_party/fatfs/%.c $(HDRS) | $(BUILD)
	$(PREFIX)gcc $(CFLAGS) -c $< -o $@
$(BUILD)/usbfs2test.o: test/usbfs2test.c $(HDRS) | $(BUILD)
	$(PREFIX)gcc $(CFLAGS) -c $< -o $@
$(BUILD)/libusbfs2.a: $(LIB_OBJS)
	rm -f $@
	$(PREFIX)ar rcsD $@ $^
$(BUILD)/usbfs2test.elf: $(BUILD)/usbfs2test.o $(BUILD)/libusbfs2.a
	$(PREFIX)gcc $(LDFLAGS) $(BUILD)/usbfs2test.o $(BUILD)/libusbfs2.a $(LIBS) -o $@
$(NRO): $(BUILD)/usbfs2test.elf
	$(DEVKITPRO)/tools/bin/elf2nro $< $@

# The host test: the same pure sources (and the same read-only ffconf.h), built for the build machine.
HOST_SRC := test/host_test.c $(addprefix source/,$(addsuffix .c,$(PURE))) third_party/fatfs/ff.c third_party/fatfs/ffunicode.c
$(BUILD)/host_test: $(HOST_SRC) $(HDRS) test/sha256.h | $(BUILD)
	$(HOSTCC) $(HOSTCFLAGS) $(HOST_SRC) -o $@

static-check: all
	@USBFS2_XFER64K=$(XFER64K) USBFS2_NRO=$(NRO) python3 -I checks.py $(PREFIX) $(BUILD)

check: static-check $(BUILD)/host_test $(BUILD)/hotplug $(BUILD)/media_retry
	./$(BUILD)/media_retry test/images/fat12-whole.img test/images/gpt-fat16-exfat.img
	./$(BUILD)/hotplug test/images/fat12-whole.img test/images/fat32-mbr.img
	@out="$$(./$(BUILD)/host_test test/images)"; rc=$$?; \
	  if [ $$rc -ne 0 ] || ! grep -qE '^host_test: PASS [0-9]+ cases, 5 images$$' <<<"$$out"; then \
	    echo "check FAILED (host test):"; grep -vE '^    \|' <<<"$$out" | tail -30; exit 1; fi; \
	  echo "check H: host test (ASan+UBSan) over the pure layers + read-only FatFs: $$(tail -1 <<<"$$out")"; \
	  grep -E '^image|^  [0-9]+ files' <<<"$$out" | sed 's/^/    /'

# Two clean builds must give byte-identical outputs.
repro:
	@set -e; rm -rf $(BUILD) $(NRO); $(MAKE) -s all >/dev/null; \
	  a="$$(sha256sum $(NRO) $(BUILD)/libusbfs2.a | cut -d' ' -f1 | paste -sd' ')"; \
	  rm -rf $(BUILD) $(NRO); $(MAKE) -s all >/dev/null; \
	  b="$$(sha256sum $(NRO) $(BUILD)/libusbfs2.a | cut -d' ' -f1 | paste -sd' ')"; \
	  if [ "$$a" != "$$b" ]; then echo "repro FAILED: $$a != $$b"; exit 1; fi; \
	  echo "repro: two clean builds identical: usbfs2test.nro libusbfs2.a = $$a"

clean:
	rm -rf $(BUILD) $(NRO)
.PHONY: all static-check check repro clean

$(BUILD)/hotplug: test/hotplug.c test/shim/switch.h source/usbfs2.c $(HOST_SRC) $(HDRS) | $(BUILD)
	$(HOSTCC) $(HOSTCFLAGS) -Itest/shim test/hotplug.c source/usbfs2.c $(filter-out test/host_test.c,$(HOST_SRC)) -pthread -o $@

$(BUILD)/media_retry: test/media_retry.c test/shim/switch.h source/usbfs2.c $(HOST_SRC) $(HDRS) | $(BUILD)
	$(HOSTCC) $(HOSTCFLAGS) -Itest/shim test/media_retry.c source/usbfs2.c $(filter-out test/host_test.c,$(HOST_SRC)) -pthread -o $@
