/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef LK2ND_RAMOOPS_H
#define LK2ND_RAMOOPS_H

/* Serial-console access to the ramoops region (see lk2nd/ramoops) */
void lk2nd_ramoops_print_summary(void);
void lk2nd_ramoops_print_console(void);
void lk2nd_ramoops_print_dumps(void);
void lk2nd_ramoops_zap(void);

#endif /* LK2ND_RAMOOPS_H */
