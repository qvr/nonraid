/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Simple header to map raid6 symbols to nonraid symbols
 * This allows the nonraid module to use the renamed symbols
 * while keeping all the internal raid6 code unchanged
 */

#ifndef NONRAID_PQ_H
#define NONRAID_PQ_H

/* Include the original raid6 header for all the types and constants */
#include <linux/raid/pq.h>

/* Map the original symbols to the nonraid-exported versions */
#define raid6_empty_zero_page    nonraid_empty_zero_page
#define raid6_call               nonraid_call
#define raid6_gen_syndrome       nonraid_gen_syndrome
#define raid6_xor_syndrome       nonraid_xor_syndrome
#define raid6_2data_recov        nonraid_2data_recov
#define raid6_datap_recov        nonraid_datap_recov
#define raid6_gfmul              nonraid_gfmul
#define raid6_vgfmul             nonraid_vgfmul
#define raid6_gfexp              nonraid_gfexp
#define raid6_gflog              nonraid_gflog
#define raid6_gfinv              nonraid_gfinv
#define raid6_gfexi              nonraid_gfexi

/* Declare the nonraid symbols that will be exported */
extern const char nonraid_empty_zero_page[PAGE_SIZE];
extern struct raid6_calls nonraid_call;
extern void (*nonraid_gen_syndrome)(int, size_t, void **);
extern void (*nonraid_xor_syndrome)(int, int, int, size_t, void **);
extern void (*nonraid_2data_recov)(int, size_t, int, int, void **);
extern void (*nonraid_datap_recov)(int, size_t, int, void **);
extern u8 const nonraid_gfmul[256][256];
extern u8 const nonraid_gfexp[256];
extern u8 const nonraid_gflog[256];
extern u8 const nonraid_gfinv[256];
extern u8 const nonraid_gfexi[256];
#ifdef CONFIG_SMP
extern const char nonraid_vgfmul[256][32];
#endif

#endif /* NONRAID_PQ_H */
