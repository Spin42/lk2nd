/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef LK2ND_BOOT_AB_H
#define LK2ND_BOOT_AB_H

#include <stdint.h>
#include <sys/types.h>

/*
 * RAUC-compatible A/B partition boot support
 *
 * Provides A/B boot slot management using U-Boot environment variables
 * compatible with RAUC's boot logic. Supports both partition-based and
 * offset-based A/B configurations.
 */

/* Initialize A/B boot system with U-Boot environment location */
void lk2nd_boot_ab_init(const char *partition, uint64_t offset, size_t size);

/* Get current active boot slot (A or B) based on BOOT_ORDER and counters */
char lk2nd_boot_ab_get_slot(void);

/* Pre-boot handler: decrement boot counter before boot attempt */
void lk2nd_boot_ab_pre_boot(void);

/* Get the base device name to read boot data from (same as U-Boot env partition) */
const char *lk2nd_boot_ab_get_base_device(void);

/* Set boot partition offsets for A/B slots (for sub-partitions within a main partition) */
void lk2nd_boot_ab_set_offsets(uint64_t offset_a, uint64_t offset_b);

/* Get the boot partition offset for the current slot */
uint64_t lk2nd_boot_ab_get_offset(void);

#endif /* LK2ND_BOOT_AB_H */