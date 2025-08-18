/*
 * md_private.h : (modified) Multiple Devices driver for linux
 *        Copyright (C) 2006-2019 Tom Mortensen
 * 
 * Derived from:

   md.h : Multiple Devices driver for Linux
          Copyright (C) 1996-98 Ingo Molnar, Gadi Oxman
          Copyright (C) 1994-96 Marc ZYNGIER
	  <zyngier@ufr-info-p7.ibp.fr> or
	  <maz@gloups.fdn.fr>
	  
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.
   
   You should have received a copy of the GNU General Public License
   (for example /usr/src/linux/COPYING); if not, write to the Free
   Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.  
*/

#ifndef _MD_PRIVATE_H
#define _MD_PRIVATE_H

#include <linux/version.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/linkage.h>
#include <linux/blkdev.h>
#include <linux/major.h>
#include <linux/reboot.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>
#include <linux/sysctl.h>
#include <linux/buffer_head.h> /* for invalidate_bdev */
#include <linux/poll.h>
#include <linux/mutex.h>
#include <linux/ctype.h>
#include <linux/string.h>
#include <linux/freezer.h>
#include <linux/init.h>
#include <linux/file.h>
#ifdef CONFIG_KMOD
#include <linux/kmod.h>
#endif
#include <linux/hdreg.h>
#include <linux/ata.h>
#include <net/checksum.h>
#include <linux/raid/xor.h>

/*
 * Here are the raid6 p/q functions. Note: we patched lib/raid6/algos.c to ensure xor_syndrome() function
 * is always available.
 */
#include "../../raid6/nonraid_pq.h"
extern void (*raid6_gen_syndrome)(int, size_t, void **);
extern void (*raid6_xor_syndrome)(int, int, int, size_t, void **);

/*
 * Different major versions are not compatible.
 * Different minor versions are only downward compatible.
 * Different patchlevel versions are downward and upward compatible.
 */
#define MD_MAJOR_VERSION                2
#define MD_MINOR_VERSION                9
#define MD_PATCHLEVEL_VERSION           35

/****************************************************************************/
/* 'md_p.h' holds the 'physical' layout of RAID devices */

/* The maximum number of disks per array that we support.
 */
#define MD_SB_DISKS			30
#define MD_SB_P_IDX                     0
#define MD_SB_Q_IDX                     29

/*
 * unRAID superblock.
 * The 4096-byte superblock is a set of 32 "descriptors" of 32 4-byte words (128 bytes) each.
 * The first descriptor holds "common information".
 * The next 31 descriptors are "disk discriptors":
 *  0 => P
 *  1 => disk1
 *  :
 * 28 => disk28
 * 29 => Q
 * 30 => R (not implemented)
 */
#define MD_SB_BYTES			4096
#define MD_SB_COMMON_WORDS	        32
#define MD_SB_DESCRIPTOR_WORDS	        32

/* id strings are null-terminated and this length includes the terminating null byte */
#define MD_ID_SIZE                      80  /* byte count */

/* Device "Operational state bits"
 * Important: bits 8-15 are reserved for unraid use.
 */
#define MD_DISK_VALID		        0 /* disk is valid */
#define MD_DISK_ENABLED		        1 /* disk is enabled */
#define MD_DISK_ACTIVE                  2 /* disk is active */

/* These macros expect the argument to be a pointer-to a structure with
 * a 'state' member.
 */
#define disk_active(d)                  ((d)->state &   (1 << MD_DISK_ACTIVE))
#define mark_disk_active(d)             ((d)->state |=  (1 << MD_DISK_ACTIVE))
#define mark_disk_inactive(d)           ((d)->state &= ~(1 << MD_DISK_ACTIVE))

#define disk_enabled(d)                 ((d)->state &   (1 << MD_DISK_ENABLED))
#define mark_disk_enabled(d)            ((d)->state |=  (1 << MD_DISK_ENABLED))
#define mark_disk_disabled(d)           ((d)->state &= ~(1 << MD_DISK_ENABLED))

#define disk_valid(d)                   ((d)->state &   (1 << MD_DISK_VALID))
#define mark_disk_valid(d)              ((d)->state |=  (1 << MD_DISK_VALID))
#define mark_disk_invalid(d)            ((d)->state &= ~(1 << MD_DISK_VALID))
/*
 * Device descriptor structure.
 */
typedef struct mdp_device_descriptor_s {
	__u32 major;		/* 0 Device major number (not used)	      */
	__u32 minor;		/* 1 Device minor number (not used)	      */
	__u32 number;		/* 2 Device slot number                       */
	__u32 state;		/* 3 Operational state bits		      */
	__u64 size;             /* 4-5 Size in 1024-byte blocks               */
        __u8  id[MD_ID_SIZE];   /* 6-25 ID string (including null terminator) */
	__u32 reserved[MD_SB_DESCRIPTOR_WORDS - 26];
} mdp_disk_t;

/*
 * Superblock structure.
 */
#define MD_SB_MAGIC		0xb92b4efc

/* Array "operational" state bits
 */
#define MD_SB_CLEAN             0 /* array is clean */

#define sb_clean(sb)            ((sb)->state &   (1 << MD_SB_CLEAN))
#define mark_sb_clean(sb)       ((sb)->state |=  (1 << MD_SB_CLEAN))
#define mark_sb_unclean(sb)     ((sb)->state &= ~(1 << MD_SB_CLEAN))

typedef struct mdp_superblock_s {
	/*
	 * Common information
	 */
	__u32 md_magic;		/*  0 MD identifier 			      */
	__u32 major_version;	/*  1 major version to which the set conforms */
	__u32 minor_version;	/*  2 minor version ...			      */
	__u32 patch_version;	/*  3 patchlevel version ...		      */
	__u32 sb_csum;		/*  4 checksum of the whole superblock        */
	__u32 ctime;		/*  5 Array Creation time		      */
	__u32 utime;		/*  6 Superblock update time		      */
	__u32 events;	        /*  7 Superblock update count                 */
	__u32 md_minor;		/*  8 preferred MD minor device number	      */
	__u32 state;	        /*  9 array state                             */
	__u32 num_disks;	/* 10 number of disk slots in the array       */
	__u32 stime;		/* 11 Last sync start time        	      */
	__u32 sync_errs;	/* 12 Last sync error count                   */
	__u32 stime2;		/* 13 Last sync end time        	      */
	__u32 sync_exit;	/* 14 Last sync exit code                     */
	__u32 spare;	        /* 15 spare (not used)                        */
        __u8  label[32];        /* 16-23 Label                                */
	__u32 common_reserved[MD_SB_COMMON_WORDS - 24];
	/*
	 * Disks information
	 */
	mdp_disk_t disks[MD_SB_DISKS];
        /*
         * Last descriptor reserved
         */
        mdp_disk_t reserved_desc;
} mdp_super_t;

/* Previous version of the superblock */
/* ********************************** */

typedef struct mdp_device_descriptor_v1_s {
	__u32 major;		/* 0 Device major number (not used)	      */
	__u32 minor;		/* 1 Device minor number (not used)	      */
	__u32 number;		/* 2 Device slot number                       */
	__u32 state;		/* 3 Operational state bits		      */
	__u32 size;             /* 4 Size in 1024-byte blocks                 */
        __u8  model[40];        /* 5-14 Model reported by device              */
	__u8  serial_no[20];    /* 15-19 Serial number reported by device     */
	__u32 reserved[MD_SB_DESCRIPTOR_WORDS - 20];
} mdp_disk_v1_t;

typedef struct mdp_superblock_v1_s {
	/*
	 * Common information
	 */
	__u32 md_magic;		/*  0 MD identifier 			      */
	__u32 major_version;	/*  1 major version to which the set conforms */
	__u32 minor_version;	/*  2 minor version ...			      */
	__u32 patch_version;	/*  3 patchlevel version ...		      */
	__u32 sb_csum;		/*  4 checksum of the whole superblock        */
	__u32 ctime;		/*  5 Array Creation time		      */
	__u32 utime;		/*  6 Superblock update time		      */
	__u32 events;	        /*  7 Superblock update count                 */
	__u32 md_minor;		/*  8 preferred MD minor device number	      */
	__u32 state;	        /*  9 array state                             */
	__u32 num_disks;	/* 10 number of disks in the array            */
	__u32 stime;		/* 11 Last sync time        		      */
	__u32 sync_errs;	/* 12 Last sync error count                   */
	__u32 common_reserved[MD_SB_COMMON_WORDS - 13];
	/*
	 * Disks information
	 */
	mdp_disk_v1_t disks[MD_SB_DISKS];
} mdp_super_v1_t;


/****************************************************************************/
/* 'md_k.h' holds kernel internal definitions  */

typedef struct mdk_rdev_s {
        struct file             *bdev_file;               /* handle from open for bdev */
	struct block_device     *bdev;	                  /* block device handle */

	char *status;                                     /* disk status */
	unsigned long           offset;		          /* disk offset in sectors */
	unsigned long long      size;		          /* disk size in 1024-byte blocks */
	unsigned char           id[MD_ID_SIZE];           /* id string (including null terminator) */
	unsigned char           name[BDEVNAME_SIZE];      /* device name string */
        int                     erased;                   /* factory-erased flag */

	unsigned long           reads;                    /* reads statistic */
	unsigned long           writes;                   /* writes statistic */
	unsigned long           errors;                   /* errors statistic */
        unsigned long           simulate_rderror;         /* used for testing */
        unsigned long           simulate_wrerror;         /* used for testing */
} mdk_rdev_t;

typedef struct mddev_s {
	void			*private;

	dev_t                   dev;
	atomic_t		active;

	mdp_super_t		sb;
	struct mutex       	sb_sem;
	
	struct mdk_thread_s     *recovery_thread;
	struct mutex       	recovery_sem;
	wait_queue_head_t	recovery_wait;
	int                     recovery_option;
	unsigned long long	recovery_running;         /* total sectors, or 0 if no recovery */
	atomic_t		recovery_active;          /* sectors scheduled, but not written */
        char                    recovery_action[128];
	unsigned long long      recovery_size;
	unsigned long long	curr_resync;	          /* blocks scheduled */
	unsigned long		resync_mark;	          /* a recent timestamp */
	unsigned long long	resync_mark_cnt;          /* blocks written at resync_mark */
	
	char                    *state;
        int                     num_disks;
	int                     num_disabled;
	int                     num_replaced;
	int                     num_invalid;
	int                     num_missing;
	int                     num_wrong;
	int                     num_new;
        int                     swap_p_idx;
        int                     swap_q_idx;

	mdk_rdev_t              rdev[MD_SB_DISKS];
	struct gendisk          *gendisk[MD_SB_DISKS];
} mddev_t;

typedef struct mdk_thread_s {
	void                    (*run)(mddev_t *mddev, unsigned long arg);
	mddev_t                 *mddev;
	unsigned long           arg;
	wait_queue_head_t       wqueue;
	unsigned long           flags;
	struct task_struct      *tsk;
	unsigned long           timeout;
} mdk_thread_t;

#define THREAD_WAKEUP  0

/****************************************************************************/
/* md */

extern mdk_thread_t *md_register_thread(void (*run)(mddev_t *, unsigned long),
                                        mddev_t *mddev, unsigned long arg,  const char *name);
extern void md_unregister_thread(mdk_thread_t *thread);
extern void md_wakeup_thread(mdk_thread_t *thread);
extern void md_interrupt_thread(mdk_thread_t *thread);

extern int  md_update_sb(mddev_t *mddev);
extern void md_read_error(mddev_t *mddev, int disk_number, sector_t sector);
extern int md_write_error(mddev_t *mddev, int disk_number, sector_t sector);
extern void md_sync_error(mddev_t *mddev, sector_t sector, char *message);
extern void md_sync_done(mddev_t *mddev, sector_t sector, int count);

extern struct workqueue_struct *md_wq; 

/****************************************************************************/
/* unraid */

/*
 * Write methods
 */
#define READ_MODIFY_WRITE	0
#define RECONSTRUCT_WRITE	1

extern int unraid_run(mddev_t *mddev);
extern int unraid_stop(mddev_t *mddev);
extern int unraid_dump(mddev_t *mddev);
extern int unraid_sync(mddev_t *mddev, sector_t sector_nr);
extern int unraid_num_stripes(mddev_t *mddev, int num_stripes);
extern void unraid_make_request(mddev_t *mddev, int unit, struct bio *bi);

/****************************************************************************/
#endif 

