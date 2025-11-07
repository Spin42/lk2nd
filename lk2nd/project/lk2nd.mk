# SPDX-License-Identifier: BSD-3-Clause
LK2ND_PROJECT := lk2nd
LK2ND_DISPLAY ?= cont-splash
LK2ND_SKIP_GDSC_CHECK ?= 0
# When set to 1, render the fastboot/lk2nd menu on the serial console instead of the framebuffer
LK2ND_SERIAL_MENU ?= 0
UMS_ENABLE ?= 0
UMS_COUNTDOWN_SECONDS ?= 3
UMS_PARTITION ?= userdata
include lk2nd/project/base.mk

MODULES += \
	lk2nd/device \
	lk2nd/device/2nd \

# Conditional GDSC check bypass for headless operation
ifeq ($(LK2ND_SKIP_GDSC_CHECK),1)
DEFINES += LK2ND_SKIP_GDSC_CHECK=1
endif

# USB Mass Storage support
ifeq ($(UMS_ENABLE),1)
DEFINES += UMS_ENABLE=1
DEFINES += UMS_COUNTDOWN_SECONDS=$(UMS_COUNTDOWN_SECONDS)
DEFINES += UMS_PARTITION="$(UMS_PARTITION)"
endif

# Serial menu support
ifeq ($(LK2ND_SERIAL_MENU),1)
DEFINES += LK2ND_SERIAL_MENU=1
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
