# SPDX-License-Identifier: BSD-3-Clause
LK2ND_PROJECT := lk2nd
LK2ND_DISPLAY ?= cont-splash
LK2ND_SKIP_GDSC_CHECK ?= 0
# When set to 1, render the fastboot/lk2nd menu on the serial console instead of the framebuffer
LK2ND_SERIAL_MENU ?= 0
# Boot menu countdown duration in seconds (before auto-booting)
LK2ND_MENU_TIMEOUT ?= 10
# USB Mass Storage configuration
LK2ND_UMS ?= 0
LK2ND_UMS_PARTITION ?= userdata
# RAUC-style A/B boot bootstrap: where the U-Boot environment lives.
# The partition is resolved by name or GPT label; slot offsets are fallback
# defaults, overridable at runtime via the BOOT_A_OFFSET/BOOT_B_OFFSET env
# variables (fw_setenv). Platform-specific defaults (e.g. slot offsets for
# the device's disk image layout) live in platform/$(TARGET)/lk2nd-ab.mk.
-include platform/$(TARGET)/lk2nd-ab.mk
LK2ND_AB_BOOT ?= 1
LK2ND_AB_ENV_PART ?= userdata
LK2ND_AB_ENV_OFFSET ?= 0x10000
LK2ND_AB_ENV_SIZE ?= 0x20000
LK2ND_AB_SLOT_OFFSET_A ?= 0
LK2ND_AB_SLOT_OFFSET_B ?= 0
include lk2nd/project/base.mk

MODULES += \
	lk2nd/device \
	lk2nd/device/2nd \

# Conditional GDSC check bypass for headless operation
ifeq ($(LK2ND_SKIP_GDSC_CHECK),1)
DEFINES += LK2ND_SKIP_GDSC_CHECK=1
endif

# Boot menu countdown (applies when LK2ND_UMS=1 or other boot menu triggers)
DEFINES += LK2ND_MENU_TIMEOUT=$(LK2ND_MENU_TIMEOUT)

# USB Mass Storage support
ifeq ($(LK2ND_UMS),1)
DEFINES += LK2ND_UMS=1
DEFINES += LK2ND_UMS_PARTITION=$(LK2ND_UMS_PARTITION)
endif

# Serial menu support
ifeq ($(LK2ND_SERIAL_MENU),1)
DEFINES += LK2ND_SERIAL_MENU=1
endif

# A/B boot bootstrap (values are tokens, stringified with xstr() in C)
ifeq ($(LK2ND_AB_BOOT),1)
DEFINES += LK2ND_AB_BOOT=1
DEFINES += LK2ND_AB_ENV_PART=$(LK2ND_AB_ENV_PART)
DEFINES += LK2ND_AB_ENV_OFFSET=$(LK2ND_AB_ENV_OFFSET)
DEFINES += LK2ND_AB_ENV_SIZE=$(LK2ND_AB_ENV_SIZE)
DEFINES += LK2ND_AB_SLOT_OFFSET_A=$(LK2ND_AB_SLOT_OFFSET_A)
DEFINES += LK2ND_AB_SLOT_OFFSET_B=$(LK2ND_AB_SLOT_OFFSET_B)
endif

# Verbose HSUSB init logging (set LK2ND_DEBUG_HSUSB=1 for debug builds)
LK2ND_DEBUG_HSUSB ?= 0
ifeq ($(LK2ND_DEBUG_HSUSB),1)
DEFINES += LK2ND_DEBUG_HSUSB=1
endif

ifneq ($(ENABLE_FBCON_DISPLAY_MSG),1)
MODULES += $(if $(filter $(MODULES), lk2nd/display), lk2nd/device/menu)
endif

# Use part of the "boot" partition for the lk2nd boot image. The real Android
# boot image can be placed in the partition with 512 KiB offset.
LK2ND_PARTITION_BASE ?= boot
LK2ND_PARTITION_NAME ?= lk2nd
LK2ND_PARTITION_SIZE ?= 512*1024

# The primary bootloader will implement LONG_PRESS_POWER_ON if needed.
# If we do it again in lk2nd we might accidentally shutdown the device because
# the user needs to keep the power key pressed for *really* long.
DEFINES := $(filter-out LONG_PRESS_POWER_ON=1, $(DEFINES))

# Since lk2nd is typically used through lk.bin, having separate code/data
# segments with a fixed 1 MiB offset increases the binary size significantly,
# since a lot of padding has to be added inbetween. Disable it for now....
DEFINES := $(filter-out SECURE_CODE_MEM=1, $(DEFINES))

# Weak battery charging is handled by the primary bootloader
DEFINES := $(filter-out ENABLE_WBC=1, $(DEFINES))

# Should be already done by primary bootloader if wanted
DEFINES := $(filter-out ENABLE_XPU_VIOLATION=1, $(DEFINES))

# Build Android boot image
OUTBOOTIMG := $(BUILDDIR)/lk2nd.img
MKBOOTIMG_CMDLINE := lk2nd

SIGN_BOOTIMG ?= 0
BOOTIMG_CERT ?= lk2nd/certs/verity.x509.pem
BOOTIMG_KEY ?= lk2nd/certs/verity.pk8
