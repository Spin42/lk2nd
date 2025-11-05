/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef LK2ND_BOOT_BOOT_H
#define LK2ND_BOOT_BOOT_H

#include <list.h>
#include <string.h>

#include <lk2nd/boot.h>

/* util.c */
void lk2nd_print_file_tree(char *root, char *prefix);

/* extlinux.c */
void lk2nd_try_extlinux(const char *mountpoint);

/* ab.c - RAUC-compatible A/B partition support */
#include "ubootenv.h"

/* Initialize A/B boot system with U-Boot environment location */
void lk2nd_boot_ab_init(const char *partition, uint64_t offset, size_t size);

/* Get current active boot slot (A or B) based on BOOT_ORDER and counters */
char lk2nd_boot_ab_get_slot(void);

/* Pre-boot handler: decrement boot counter before boot attempt */
void lk2nd_boot_ab_pre_boot(void);

/* Set boot partition names for A/B slots */
void lk2nd_boot_ab_set_partitions(const char *part_a, const char *part_b);

/* Get the boot partition name for the current slot */
const char *lk2nd_boot_ab_get_partition(void);

#endif /* LK2ND_BOOT_BOOT_H */
