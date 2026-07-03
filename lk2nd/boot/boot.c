// SPDX-License-Identifier: BSD-3-Clause
/* Copyright (c) 2023 Nikita Travkin <nikita@trvn.ru> */

#include <debug.h>
#include <lib/bio.h>
#include <lib/fs.h>
#include <list.h>
#include <stdlib.h>
#include <target.h>

#include <lk2nd/boot.h>
#include <lk2nd/hw/bdev.h>

#include "ab.h"
#include "boot.h"

#ifndef LK2ND_BOOT_MIN_SIZE
#define LK2ND_BOOT_MIN_SIZE (16 * 1024 * 1024)
#endif

/**
 * lk2nd_mount() - Mount a device on mountpoint, trying all supported
 * filesystems (ext2/3/4 and FAT).
 */
static int lk2nd_mount(const char *mountpoint, const char *device)
{
	static const char * const filesystems[] = { "ext2", "fat" };
	unsigned int i;
	int ret = -1;

	for (i = 0; i < ARRAY_SIZE(filesystems); i++) {
		ret = fs_mount(mountpoint, filesystems[i], device);
		if (ret >= 0)
			return ret;
	}

	return ret;
}

/**
 * lk2nd_try_boot_bdev() - Mount a partition and try to boot from it.
 *
 * Returns only if no kernel was launched.
 */
static void lk2nd_try_boot_bdev(bdev_t *bdev)
{
	char mountpoint[128];
	int ret;

	/*
	 * Skip partitions that are too small to have a boot fs on.
	 *
	 * 'boot' partition is explicitly allowed to have a small fs on it
	 * in case one installs next stage bootloader package (i.e. u-boot)
	 * there but still wants to make use of lk2nd's device database.
	 */
	if (bdev->size < LK2ND_BOOT_MIN_SIZE &&
	    !(bdev->label && !strncmp(bdev->label, "boot", strlen("boot"))))
		return;

	snprintf(mountpoint, sizeof(mountpoint), "/%s", bdev->name);
	ret = lk2nd_mount(mountpoint, bdev->name);
	if (ret < 0)
		return;

	if (DEBUGLEVEL >= SPEW) {
		dprintf(SPEW, "Scanning %s ...\n", bdev->name);
		dprintf(SPEW, "%s\n", mountpoint);
		lk2nd_print_file_tree(mountpoint, " ");
	}

	lk2nd_try_extlinux(mountpoint);
}

/**
 * lk2nd_scan_devices() - Scan filesystems and try to boot
 */
static void lk2nd_scan_devices(void)
{
	struct bdev_struct *bdevs = bio_get_bdevs();
	char mountpoint[128];
	bdev_t *bdev;
	int ret;
	const char *base_device = NULL;
	uint64_t target_offset = 0;
	char subdev_name[64];
	bool sd_tried = false;

	dprintf(INFO, "boot: Trying to boot from the file system...\n");

	/*
	 * SD card pass: external SD partitions are registered as mmcXpN
	 * (eMMC partitions use the wrp0pN wrapper). Try them before the
	 * eMMC so an inserted bootable card always takes precedence.
	 */
	list_for_every_entry(&bdevs->list, bdev, bdev_t, node) {
		if (!bdev->is_leaf || strncmp(bdev->name, "mmc", 3))
			continue;

		if (!sd_tried) {
			dprintf(INFO, "boot: SD card detected, trying it first\n");
			sd_tried = true;
		}

		lk2nd_try_boot_bdev(bdev);
	}

	if (sd_tried)
		dprintf(INFO, "boot: SD card did not boot, falling back to eMMC\n");

	/* Early A/B bootstrap from the LK2ND_AB_* build defaults, unless
	 * A/B was already initialized by extlinux. The BOOT_A_OFFSET and
	 * BOOT_B_OFFSET env variables override the default slot offsets.
	 */
	lk2nd_boot_ab_ensure_init();

	/* Check if A/B boot is configured: base device is the U-Boot env partition */
	base_device = lk2nd_boot_ab_get_base_device();
	target_offset = lk2nd_boot_ab_get_offset();

	if (base_device) {
		dprintf(INFO, "boot: A/B mode - base device '%s'", base_device);
		if (target_offset > 0)
			dprintf(INFO, " at offset 0x%llx\n", target_offset);
		else
			dprintf(INFO, " (no offset)\n");
	}

	/* A/B configured: boot straight from the env partition, skip the scan */
	if (base_device) {
		if (target_offset > 0) {
			/*
			 * Try each slot until one boots. lk2nd_try_extlinux()
			 * only returns if no kernel was launched, so on return we
			 * unmount and move on to the next slot.
			 */
			do {
				char slot = lk2nd_boot_ab_get_slot();
				target_offset = lk2nd_boot_ab_get_offset();

				/* A slot without a configured offset would map to
				 * block 0 (the env area), not a boot filesystem. */
				if (target_offset == 0) {
					dprintf(CRITICAL, "boot: No offset configured for slot %c, skipping\n", slot);
					continue;
				}

				bdev_t *parent_bdev = bio_open(base_device);
				if (!parent_bdev) {
					dprintf(CRITICAL, "boot: Failed to open base device '%s'\n", base_device);
					break;
				}

				size_t block_size = parent_bdev->block_size;
				bnum_t start_block = target_offset / block_size;
				size_t subdev_len = parent_bdev->block_count - start_block;
				bio_close(parent_bdev);

				/* Per-slot name so retries don't clash */
				snprintf(subdev_name, sizeof(subdev_name), "ab-slot-%c", slot);

				ret = bio_publish_subdevice(base_device, subdev_name, start_block, subdev_len);
				if (ret < 0) {
					dprintf(CRITICAL, "boot: Failed to create subdevice for slot %c: %d\n", slot, ret);
					continue;
				}

				dprintf(INFO, "boot: Trying slot %c: subdevice '%s' at block %u\n",
					slot, subdev_name, (unsigned)start_block);

				snprintf(mountpoint, sizeof(mountpoint), "/%s", subdev_name);
				ret = lk2nd_mount(mountpoint, subdev_name);
				if (ret < 0) {
					dprintf(CRITICAL, "boot: Failed to mount slot %c subdevice '%s'\n", slot, subdev_name);
					continue;
				}

				if (DEBUGLEVEL >= SPEW) {
					dprintf(SPEW, "Scanning %s ...\n", subdev_name);
					lk2nd_print_file_tree(mountpoint, " ");
				}

				lk2nd_try_extlinux(mountpoint);

				dprintf(CRITICAL, "boot: Slot %c did not boot\n", slot);
				fs_unmount(mountpoint);

			} while (lk2nd_boot_ab_advance_slot());

			dprintf(CRITICAL, "boot: No A/B slot booted. Reverting to android boot.\n");

		} else {
			/* No offset: just mount the base device directly */
			snprintf(mountpoint, sizeof(mountpoint), "/%s", base_device);
			ret = lk2nd_mount(mountpoint, base_device);
			if (ret >= 0) {
				if (DEBUGLEVEL >= SPEW) {
					dprintf(SPEW, "Scanning %s ...\n", base_device);
					dprintf(SPEW, "%s\n", mountpoint);
					lk2nd_print_file_tree(mountpoint, " ");
				}
				lk2nd_try_extlinux(mountpoint);
			} else {
				dprintf(CRITICAL, "boot: Failed to mount base device '%s'\n", base_device);
			}
		}
		return;
	}

	/* Fallback: scan all devices as before (non-A/B mode) */
	list_for_every_entry(&bdevs->list, bdev, bdev_t, node) {

		/* Skip top level block devices. */
		if (!bdev->is_leaf)
			continue;

		/* SD partitions were already tried in the first pass. */
		if (!strncmp(bdev->name, "mmc", 3))
			continue;

		lk2nd_try_boot_bdev(bdev);
	}

	dprintf(INFO, "boot: Bootable file system not found. Reverting to android boot.\n");
}

/**
 * lk2nd_boot() - Try to boot the OS.
 *
 * This method is supposed to be called from aboot.
 * If appropriate OS is found, it will be booted, and this
 * method will never return.
 */
void lk2nd_boot(void)
{
	static bool init_done = false;

	if (!init_done) {
		lk2nd_bdev_init();
		init_done = true;
	}

	lk2nd_scan_devices();
}
