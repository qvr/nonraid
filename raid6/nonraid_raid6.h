/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Minimal raid6 definitions for nonraid module
 * This avoids dependency on the system raid6_pq module
 */

#ifndef NONRAID_RAID6_H
#define NONRAID_RAID6_H

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <asm/page.h>

/* Include the external interface which has symbol mappings and basic declarations */
#include "nonraid_pq.h"

/* CPU feature detection for X86 */
#ifdef CONFIG_X86
#include <asm/cpufeatures.h>
#include <asm/fpu/api.h>
#endif

/* Preemption control */
#include <linux/preempt.h>

/* Time functions */
#include <linux/jiffies.h>

/* RAID-6 algorithm structures - copied from linux/raid/pq.h */
struct raid6_calls {
	void (*gen_syndrome)(int, size_t, void **);
	void (*xor_syndrome)(int, int, int, size_t, void **);
	int (*valid)(void);
	const char *name;
	int priority;
};

struct raid6_recov_calls {
	void (*data2)(int, size_t, int, int, void **);
	void (*datap)(int, size_t, int, void **);
	int (*valid)(void);
	const char *name;
	int priority;
};

/* Function prototypes that would normally come from other raid6 files */
#ifdef CONFIG_X86
extern const struct raid6_calls raid6_mmxx1, raid6_mmxx2;
extern const struct raid6_calls raid6_sse1x1, raid6_sse1x2;
extern const struct raid6_calls raid6_sse2x1, raid6_sse2x2, raid6_sse2x4;
extern const struct raid6_calls raid6_avx2x1, raid6_avx2x2, raid6_avx2x4;
extern const struct raid6_calls raid6_avx512x1, raid6_avx512x2, raid6_avx512x4;
extern const struct raid6_recov_calls raid6_recov_ssse3, raid6_recov_avx2, raid6_recov_avx512;
#endif

#ifdef CONFIG_ALTIVEC
extern const struct raid6_calls raid6_altivec1, raid6_altivec2, raid6_altivec4, raid6_altivec8;
extern const struct raid6_calls raid6_vpermxor1, raid6_vpermxor2, raid6_vpermxor4, raid6_vpermxor8;
#endif

#ifdef CONFIG_KERNEL_MODE_NEON
extern const struct raid6_calls raid6_neonx1, raid6_neonx2, raid6_neonx4, raid6_neonx8;
extern const struct raid6_recov_calls raid6_recov_neon;
#endif

#ifdef CONFIG_S390
extern const struct raid6_calls raid6_s390vx8;
extern const struct raid6_recov_calls raid6_recov_s390xc;
#endif

#ifdef CONFIG_LOONGARCH
extern const struct raid6_calls raid6_lasx, raid6_lsx;
extern const struct raid6_recov_calls raid6_recov_lasx, raid6_recov_lsx;
#endif

extern const struct raid6_calls raid6_intx1, raid6_intx2, raid6_intx4, raid6_intx8;
extern const struct raid6_recov_calls raid6_recov_intx1;

/* Function prototypes */
int __init raid6_select_algo(void);

#endif /* NONRAID_RAID6_H */
