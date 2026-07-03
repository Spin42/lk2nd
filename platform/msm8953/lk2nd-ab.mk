# SPDX-License-Identifier: BSD-3-Clause
# A/B boot defaults for msm8953/sdm632 devices (Fairphone 3).
# Initial values mirror the msm8974/FP2 disk image layout; adjust once the
# FP3 image layout is finalized. Overridable on the make command line and,
# at runtime, via the BOOT_A_OFFSET/BOOT_B_OFFSET U-Boot env variables.
LK2ND_AB_ENV_PART ?= userdata
LK2ND_AB_ENV_OFFSET ?= 0x10000
LK2ND_AB_ENV_SIZE ?= 0x20000
LK2ND_AB_SLOT_OFFSET_A ?= 0x00100000
LK2ND_AB_SLOT_OFFSET_B ?= 0x04100000
