// SPDX-License-Identifier: BSD-3-Clause
/* Copyright (c) 2024 lk2nd boot menu */

#ifndef __LK2ND_DEVICE_MENU_H
#define __LK2ND_DEVICE_MENU_H

/**
 * boot_menu_countdown_check() - Display boot menu countdown and wait for keypress
 *
 * Displays a countdown timer on the serial console and waits for user input.
 * If any key is pressed during the countdown, returns 1 to indicate the user
 * wants to enter the fastboot menu. Otherwise returns 0 to continue normal boot.
 *
 * Return: 1 if key pressed (enter menu), 0 if timeout (normal boot)
 */
int boot_menu_countdown_check(void);

#endif /* __LK2ND_DEVICE_MENU_H */
