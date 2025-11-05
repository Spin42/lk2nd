// SPDX-License-Identifier: BSD-3-Clause
/* A/B partition boot integration for lk2nd (RAUC-compatible) */

#include <debug.h>
#include <stdlib.h>
#include <string.h>

#include "boot.h"
#include "ubootenv.h"

/*
 * RAUC-compatible A/B Boot Implementation for lk2nd
 *
 * This implements the RAUC bootloader interface using U-Boot environment variables.
 * The bootloader reads these variables from a U-Boot environment block typically
 * stored at a fixed offset within the userdata partition.
 *
 * Configuration via extlinux.conf (recommended):
 *   # U-Boot environment location
 *   rauc_uboot_part userdata
 *   rauc_uboot_offset 0x10000
 *   rauc_uboot_size 0x20000
 *
 *   # Boot partition names for A/B slots
 *   rauc_boot_part_a boot-a
 *   rauc_boot_part_b boot-b
 *
 *   # Boot configuration (same extlinux.conf in both boot-a and boot-b partitions)
 *   default linux
 *   label linux
 *       kernel /vmlinuz
 *       initrd /initramfs
 *       append root=/dev/mapper/image-rootfs_a
 *
 * OR using slot-specific labels (if both slots share same partition):
 *   label linux_A
 *       kernel /vmlinuz-A
 *       initrd /initramfs-A
 *       append root=/dev/mapper/image-rootfs_a
 *   label linux_B
 *       kernel /vmlinuz-B
 *       initrd /initramfs-B
 *       append root=/dev/mapper/image-rootfs_b
 *
 * Boot Flow:
 * 1. Initialize: Read U-Boot env from partition at configured offset
 * 2. Select partition: lk2nd scans only the partition for the active slot (boot-a or boot-b)
 * 3. Pre-boot: Determine active slot from BOOT_ORDER, decrement counter
 * 4. Boot: System boots selected slot using extlinux.conf from that partition
 * 5. Userspace marks boot successful by writing directly to U-Boot env
 *
 * Environment Variables (RAUC-standard):
 * - BOOT_ORDER: Space-separated slot list to try (e.g., "A B")
 * - BOOT_A_LEFT: Remaining boot attempts for slot A
 * - BOOT_B_LEFT: Remaining boot attempts for slot B
 *
 * Userspace Integration:
 * After successful boot, userspace must write directly to the U-Boot environment
 * partition to reset BOOT_<slot>_LEFT counter. This can be done using fw_setenv
 * tool or direct partition writes at the configured offset.
 */

/* Global A/B boot state */
static struct {
	struct uboot_env env;
	char partition[64];
	uint64_t offset;
	size_t size;
	bool initialized;
	char current_slot;  /* Cached current boot slot */
	char boot_part_a[64];  /* Boot partition name for slot A */
	char boot_part_b[64];  /* Boot partition name for slot B */
} ab_state = {0};

/*
 * Initialize A/B boot from U-Boot environment
 * partition: Name of partition containing uboot.env (typically "userdata")
 * offset: Byte offset within partition where uboot.env starts
 * size: Size of uboot.env in bytes (typically 0x20000 / 128KB)
 */
void lk2nd_boot_ab_init(const char *partition, uint64_t offset, size_t size)
{
	int ret;

	if (ab_state.initialized) {
		dprintf(INFO, "A/B boot already initialized\n");
		return;
	}

	if (!partition) {
		dprintf(CRITICAL, "A/B boot: partition name required\n");
		return;
	}

	if (size == 0)
		size = UBOOT_ENV_DEFAULT_SIZE;

	dprintf(INFO, "Initializing RAUC-style A/B boot from %s at offset 0x%llx (size: 0x%zx)\n",
		partition, offset, size);

	ret = uboot_env_init(&ab_state.env, partition, offset, size);
	if (ret < 0) {
		dprintf(CRITICAL, "A/B boot: Failed to initialize U-Boot environment: %d\n", ret);
		return;
	}

	strlcpy(ab_state.partition, partition, sizeof(ab_state.partition));
	ab_state.offset = offset;
	ab_state.size = size;
	ab_state.initialized = true;

	/* Determine initial boot slot based on BOOT_ORDER and counters */
	ab_state.current_slot = uboot_env_get_boot_slot(&ab_state.env);

	dprintf(INFO, "RAUC A/B boot initialized - current slot: %c\n", ab_state.current_slot);
}

/*
 * Get current boot slot (A or B)
 * Returns the slot that should be booted based on BOOT_ORDER and remaining attempts
 * Returns 'A' if A/B boot is not initialized (standard boot mode)
 */
char lk2nd_boot_ab_get_slot(void)
{
	if (!ab_state.initialized) {
		/* A/B not configured - return 'A' which won't match any _A suffix
		 * allowing standard extlinux labels to work normally */
		return 'A';
	}

	return ab_state.current_slot;
}

/*
 * Called before attempting to boot - decrements boot counter
 * This implements RAUC's pre-boot logic where we decrement BOOT_<slot>_LEFT
 * Does nothing if A/B is not initialized (standard boot mode)
 */
void lk2nd_boot_ab_pre_boot(void)
{
	char next_slot;

	if (!ab_state.initialized) {
		/* A/B not configured - skip A/B boot logic entirely */
		return;
	}

	dprintf(INFO, "A/B pre-boot: Attempting to boot slot %c\n", ab_state.current_slot);

	/* Decrement boot counter for current slot */
	if (uboot_env_decrement_boot_left(&ab_state.env, ab_state.current_slot) < 0) {
		/* Current slot exhausted, try next slot in BOOT_ORDER */
		next_slot = uboot_env_get_next_slot(&ab_state.env, ab_state.current_slot);

		if (next_slot != '\0') {
			dprintf(CRITICAL, "Slot %c exhausted, switching to slot %c\n",
				ab_state.current_slot, next_slot);
			ab_state.current_slot = next_slot;
			uboot_env_decrement_boot_left(&ab_state.env, ab_state.current_slot);
		} else {
			dprintf(CRITICAL, "All boot slots exhausted! Attempting slot %c anyway\n",
				ab_state.current_slot);
		}
	}

	/* Save state before booting (critical for boot counting) */
	uboot_env_save(&ab_state.env, ab_state.partition, ab_state.offset);
}

/*
 * Set boot partition names for A/B slots
 * This tells lk2nd which partition to scan for each slot
 */
void lk2nd_boot_ab_set_partitions(const char *part_a, const char *part_b)
{
	if (part_a)
		strlcpy(ab_state.boot_part_a, part_a, sizeof(ab_state.boot_part_a));
	if (part_b)
		strlcpy(ab_state.boot_part_b, part_b, sizeof(ab_state.boot_part_b));

	dprintf(INFO, "A/B boot partitions: A='%s', B='%s'\n",
		ab_state.boot_part_a, ab_state.boot_part_b);
}

/*
 * Get the boot partition name for the current slot
 * Returns NULL if not configured or A/B not initialized
 */
const char *lk2nd_boot_ab_get_partition(void)
{
	if (!ab_state.initialized)
		return NULL;

	if (ab_state.current_slot == 'A' && ab_state.boot_part_a[0])
		return ab_state.boot_part_a;
	else if (ab_state.current_slot == 'B' && ab_state.boot_part_b[0])
		return ab_state.boot_part_b;

	return NULL;
}
