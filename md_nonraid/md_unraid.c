/*
 * md.c : (modified) Multiple Devices driver for Linux
 *         Copyright (C) 2006-2020, Tom Mortensen <tomm@lime-technology.com>
 *         Copyright (C) 2016, Eric Schultz <erics@lime-technology.com>
 *
 * Greatly revised to support UnRaid in a particular manner.
 * 
 * Derived from:

   md.c : Multiple Devices driver for Linux
	  Copyright (C) 1998, 1999, 2000 Ingo Molnar

     completely rewritten, based on the MD driver code from Marc Zyngier

   Changes:

   - RAID-1/RAID-5 extensions by Miguel de Icaza, Gadi Oxman, Ingo Molnar
   - boot support for linear and striped mode by Harald Hoyer <HarryH@Royal.Net>
   - kerneld support by Boris Tobotras <boris@xtalk.msk.su>
   - kmod support by: Cyrus Durgin
   - RAID0 bugfixes: Mark Anthony Lisher <markal@iname.com>
   - Devfs support by Richard Gooch <rgooch@atnf.csiro.au>

   - lots of fixes and improvements to the RAID1/RAID5 and generic
     RAID code (such as request based resynchronization):

     Neil Brown <neilb@cse.unsw.edu.au>.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   You should have received a copy of the GNU General Public License
   (for example /usr/src/linux/COPYING); if not, write to the Free
   Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/
#include "md_unraid.h"
#include <linux/seq_file.h>
#include <linux/sched/signal.h>

#define MAJOR_NR 127
#define MD_DRIVER

/* MD_TRACE level
 *  0 = no command tracing
 *  1 = printk all commands
 *  2 = printk all commands plus md debug info
 *  3 = printk all commands plus md debug info plus unraid read/write commands
 *  4 = printk all commands plus md debug info plus unraid read/write commands plus unraid debug info
 */
#define MD_TRACE          1
int md_trace              = MD_TRACE;          /* command/debug tracing */
#define dprintk(x...) ((void)((md_trace >= 2) && printk(x)))

/****************************************************************************/
/* Module parameters, and other global data */

static char *super;                            /* superblock file */
module_param(super, charp, 0);

/* tunables */
#define MD_NUM_STRIPES   1280
#define MD_QUEUE_LIMIT     80
#define MD_SYNC_LIMIT       5

int md_num_stripes        = MD_NUM_STRIPES;    /* total number of stripes possible */
int md_queue_limit        = MD_QUEUE_LIMIT;    /* percentage from 1..100 */
int md_sync_limit         = MD_SYNC_LIMIT;     /* percentage from 1..100 */
int md_write_method       = READ_MODIFY_WRITE; /* write algorithm */
int md_restrict           = 1; /* temp hack */

/* These are for start_array() NEW_ARRAY case to tell it which slots start out 'invalid'.
 * Normally these identify the P and Q disk slots, but can be set to other slot numbers
 * making them start out 'invalid' instead. These are for cases where, eg, super block is
 * lost and user knows the correct array config and wants to prevent parity rebuild, or
 * wants to initiate data reconstruct on a specific disk(s) upon startup.
 */
static int invalidslota = MD_SB_P_IDX;
static int invalidslotb = MD_SB_Q_IDX;

/* This is for md_do_recovery to override the start and end of parity check/sync.
 * Value of 'md_resync_start' (in 512-byte units) specifies where to start sync (normally 0).
 * Value of 'md_resync_end' specifies where to end sync, where 0 means use the size of the
 * largest disk in the array. (Value is actually end sector + 1.)
 * Caution: no bounds checking is done if these values are changed off their defaults.
 */
static unsigned long long md_resync_start = 0;
static unsigned long long md_resync_end = 0;

/* Workqueue - used for flush
 */
struct workqueue_struct *md_wq;

/****************************************************************************/
/* Kernel thread utilities. */

/* If awakened by a signal, signal just gets flushed and we go back to waiting.
 * Also, if we're executing the 'run' function, and one or more md_wakeup_thread()'s
 * are called, it will get 'run' again as soon as it exits.
 */
static int md_thread(void * arg)
{
	mdk_thread_t *thread = arg;

	allow_signal(SIGKILL);
	while (!kthread_should_stop()) {
		if (signal_pending(current))
			flush_signals(current);

		wait_event_interruptible(thread->wqueue,
                                         test_bit(THREAD_WAKEUP, &thread->flags) ||
                                         kthread_should_park() ||
                                         kthread_should_stop());

                if (kthread_should_park())
                        kthread_parkme();
                if (test_and_clear_bit(THREAD_WAKEUP, &thread->flags))
			thread->run(thread->mddev, thread->arg);
	}

	return 0;
}

mdk_thread_t *md_register_thread(void (*run)(mddev_t *, unsigned long),
                                 mddev_t *mddev, unsigned long arg,  const char *name)
{
	mdk_thread_t *thread;

	dprintk("md: registering MD-thread %s\n", name);

	thread = kzalloc(sizeof(mdk_thread_t), GFP_KERNEL);
	if (!thread)
		return NULL;

	init_waitqueue_head(&thread->wqueue);

	thread->run = run;
	thread->mddev = mddev;
	thread->arg = arg;
	thread->tsk = kthread_run(md_thread, thread, name);
	thread->timeout = MAX_SCHEDULE_TIMEOUT; /* not used */
	if (IS_ERR(thread->tsk)) {
		kfree(thread);
		return NULL;
	}
	return thread;
}

void md_unregister_thread(mdk_thread_t *thread)
{
	dprintk("md: unregistering MD-thread pid %d\n", task_pid_nr(thread->tsk));

	/* kthread_stop() sets kthread_should_stop() for task to return true,
	 * wakes it, and waits for it to exit.
	 */
	kthread_stop(thread->tsk);
	kfree(thread);
}

void md_wakeup_thread(mdk_thread_t *thread)
{
	if (thread) {
//		dprintk("md: waking up MD-thread %s\n", thread->tsk->comm);
		set_bit(THREAD_WAKEUP, &thread->flags);
		wake_up(&thread->wqueue);
	}
}

void md_interrupt_thread(mdk_thread_t *thread)
{
	BUG_ON(!thread->tsk);

	dprintk("md: interrupting MD-thread pid %d\n", task_pid_nr(thread->tsk));
	send_sig(SIGKILL, thread->tsk, 1);
}

/****************************************************************************/
/* Superblock handling */

/* more: should merge ctime/utime in superblock and keep a single 64-bit utime */
static inline unsigned long get_seconds(void)
{
        return ktime_get_real_seconds();
}

/* Open the indicated file and read size bytes into the buffer; then close the file.
 * Returns count of bytes read, which should equal size, upon success.
 */
static int read_file(char *filename, void *buffer, int size)
{
	int retval = 0;

	if (filename && *filename) {
		struct file *fp = filp_open(filename, O_RDONLY, 0);
		if (IS_ERR(fp))
			printk("read_file: error %ld opening %s\n", -PTR_ERR(fp), filename);
		else {
                        loff_t offset = 0;
                        retval = kernel_read(fp, buffer, size, &offset);
                        if (retval < 0)
                                printk("read_file: read error %d\n", -retval);
			if (filp_close(fp, NULL))
				printk("read_file: error closing %s\n", filename);
		}
	}

	return retval;
}

/* Open the indicated file and write size bytes from the buffer; then close the file.
 * Returns count of bytes written, which should equal size, upon success.
 */
static int write_file(char *filename, void *buffer, int size)
{
	int retval = 0;

	if (filename && *filename) {
		struct file *fp = filp_open(filename, O_SYNC|O_WRONLY|O_CREAT, 0644);
		if (IS_ERR(fp)) {
			printk("write_file: error %ld opening %s\n", -PTR_ERR(fp), filename);
		}
		else {
                        loff_t offset = 0;
                        retval = kernel_write(fp, buffer, size, &offset);
                        if (retval < 0)
                                printk("write_file: write error %d\n", -retval);
			if (filp_close(fp, NULL))
				printk("write_file: error closing %s\n", filename);
		}
	}
	
	return retval;
}

/* initialize superblock */
static void init_sb(mdp_super_t *sb)
{
	memset(sb, 0, sizeof(mdp_super_t));

	sb->md_magic = MD_SB_MAGIC;
	sb->major_version = MD_MAJOR_VERSION;
	sb->minor_version = MD_MINOR_VERSION;
	sb->patch_version = MD_PATCHLEVEL_VERSION;

	sb->ctime = get_seconds();
}
	
/* calculate the superblock checksum */
static unsigned int calc_sb_csum(mdp_super_t * sb)
{
	unsigned int disk_csum, csum;

	disk_csum = sb->sb_csum;
	sb->sb_csum = 0;
	csum = csum_partial((void *)sb, MD_SB_BYTES, 0);
	sb->sb_csum = disk_csum;
	return csum;
}

/* Superblock conversion */
/* So far only conversion is pre-2.0.0 to 2.0.0 where we:
 * - increased stored disk size from 32-bit to 64-bit
 * - changed explicit model/serial to udev-style id string
 */

/* remove all embedded spaces, replacing with single underscore */
static char *strcat_sb_old_id(char *tar, char *src, int len)
{
	int space = 0;
	int i;
	tar = tar + strlen(tar);
	for (i = 0; i < len; i++) {
		if (*src == '\0')
			break;
		if (*src == ' ') {
			space = 1;
			src++;
		}
		else {
			if (space) {
				*tar++ = '_';
				space = 0;
			}
			*tar++ = *src++;
		}
	}
	return tar;
}

/* convert version 1 superblock to version 2 */
static int convert_sb(mdp_super_t * sb)
{
	mdp_super_v1_t *sb_old = (mdp_super_v1_t *)kzalloc(sizeof(mdp_super_v1_t), GFP_KERNEL);
	int i;

	if (!sb_old)
		return -1;

	memcpy(sb_old, sb, sizeof(mdp_super_v1_t));

	init_sb(sb);

	/* same */
	sb->ctime = sb_old->ctime;
	sb->utime = sb_old->utime;
	sb->events = sb_old->events;
	sb->md_minor = sb_old->md_minor;
	sb->state = sb_old->state;
        sb->num_disks = sb_old->num_disks;
	sb->stime = sb_old->stime;
	sb->sync_errs = sb_old->sync_errs;
	
	for (i = 0; i < MD_SB_DISKS; i++) {
		/* same */
		sb->disks[i].major = sb_old->disks[i].major;
		sb->disks[i].minor = sb_old->disks[i].minor;
		sb->disks[i].number = sb_old->disks[i].number;
		sb->disks[i].state = sb_old->disks[i].state;

		/* new size is u64, old size is u32 */
		sb->disks[i].size = sb_old->disks[i].size;
		
		/* new id is a udev-style model_serial string */
		/* old model/serial strings were fixed length and padded with spaces */
		strcat_sb_old_id(sb->disks[i].id, sb_old->disks[i].model, 40);
		strcat(sb->disks[i].id, "_");
		strcat_sb_old_id(sb->disks[i].id, sb_old->disks[i].serial_no, 20);
	}

	kfree(sb_old);
	return 0;
}

/* read the superblock */
static int md_read_sb(mddev_t *mddev)
{
	dprintk("md: reading superblock\n");
	if (read_file(super, &mddev->sb, MD_SB_BYTES) != MD_SB_BYTES) {
		printk("md: could not read superblock from %s\n", super);
		return -EINVAL;
	}

	/* check for validity */
	if (mddev->sb.md_magic != MD_SB_MAGIC) {
		printk("md: invalid superblock magic\n");
		return -EINVAL;
	}
	if (calc_sb_csum(&mddev->sb) != mddev->sb.sb_csum) {
		printk("md: invalid superblock checksum\n");
		return -EINVAL;
	}

	/* check for old version */
	if (mddev->sb.major_version != MD_MAJOR_VERSION) {
		printk("md: converting superblock version %d to version %d\n",
			mddev->sb.major_version, MD_MAJOR_VERSION);
		if (convert_sb(&mddev->sb) != 0)
			return -EINVAL;
	}

        /* check for pre-P/Q version where P is active but Q is not active */
        if (disk_active(&mddev->sb.disks[MD_SB_P_IDX]) &&
            !disk_active(&mddev->sb.disks[MD_SB_Q_IDX])) {
                mark_disk_active(&mddev->sb.disks[MD_SB_Q_IDX]);
	}

        dprintk("md: superblock events: %d\n", mddev->sb.events);
	return 0;
}

/* write the superblock */
int md_update_sb(mddev_t *mddev)
{
        int retval = 0;

	mutex_lock(&mddev->sb_sem);

	/* record generation info */
	mddev->sb.utime = get_seconds();
	mddev->sb.events++;

	/* calculate checksum */
	mddev->sb.sb_csum = calc_sb_csum(&mddev->sb);

	/* write the superblock */
 	dprintk("md: writing superblock to %s\n", super);
	if (write_file(super, &mddev->sb, MD_SB_BYTES) != MD_SB_BYTES) {
		printk("md: could not write superblock file: %s\n", super);
		retval = -EINVAL;
	}

	mutex_unlock(&mddev->sb_sem);
	return retval;
}

/****************************************************************************/
/* Disk status */

static char DISK_NP[] =         "DISK_NP";                /* no disk present, no disk configured */
static char DISK_OK[] =         "DISK_OK";                /* enabled, disk present, correct, valid */
static char DISK_NP_MISSING[] = "DISK_NP_MISSING";        /* enabled, but missing */
static char DISK_INVALID[] =    "DISK_INVALID";           /* enabled, disk present, but not valid */
static char DISK_WRONG[] =      "DISK_WRONG";             /* enablled, disk present, but not correct disk */
static char DISK_DSBL[] =       "DISK_DSBL";              /* disabled, old disk still present */
static char DISK_NP_DSBL[] =    "DISK_NP_DSBL";           /* disabled, no disk present */
static char DISK_DSBL_NEW[] =   "DISK_DSBL_NEW";          /* disabled, new disk present */
static char DISK_NEW[] =        "DISK_NEW";               /* new disk */

static int lock_bdev(char *name, mdk_rdev_t *rdev)
{
        char path[BDEVNAME_SIZE+6];
	
        snprintf(path, sizeof(path), "/dev/%s", name);
        
	rdev->bdev_handle = bdev_open_by_path(path, FMODE_READ|FMODE_WRITE, NULL, NULL);
	if (IS_ERR(rdev->bdev_handle)) {
                int err = PTR_ERR(rdev->bdev_handle);
                return err;
        }
	
	rdev->bdev = rdev->bdev_handle->bdev;
	return 0;
}

static void unlock_bdev(mdk_rdev_t *rdev)
{
	if (rdev->bdev) {
		bdev_release(rdev->bdev_handle);
                rdev->bdev = NULL;
        }
}

/* Import a device.
 */
static int import_device(mdk_rdev_t *rdev, char *name,
                         unsigned long offset, unsigned long long size,
                         int erased, char *id, mddev_t *mddev, int unit)
{
	int err = 0;
	
	memset(rdev, 0, sizeof(mdk_rdev_t));
	rdev->status = DISK_NP; /* assume not present */

	/* check if device assigned to slot */
        if (size == 0) {
		dprintk("md: import disk%d: no device\n", unit);
		return -ENODEV;
	}

	/* open the disk device */
	err = lock_bdev(name, rdev);
	if (err) {
		printk("md: import disk%d: lock_bdev error: %d\n", unit, err);
		return err; /* device probably not present */
	}

	/* record device name, eg, "hda" */
        strcpy( rdev->name, name);

	/* record geometry */
        rdev->offset = offset; /* in 512-byte sectors */
	rdev->size = size;     /* in 1024-byte blocks */
        rdev->erased = erased;

	/* get id string */
	strncpy(rdev->id, id, MD_ID_SIZE-1);

	/* disk is present */
	rdev->status = DISK_OK;
	printk("md: import disk%d: (%s) %s size: %llu %s\n",
	       unit, rdev->name, rdev->id, rdev->size, erased ? "erased" : "");

	unlock_bdev(rdev);
	return err;
}

/* Check if rdev id & size matches disk id & size.
 * Input strict determines comparison between recorded size/id (disk) vs. actual size/id (rdev)
 * as follows:
 *   0 => not strict, only "serial number" part of id string is compared, if equal, then this
 * function will return true (1), and the actual id will be copied into the recorded id. If no
 * match (or no serial numbers), function reverts to strict mode.
 *   1 => strict, then entire id strings must match.
 * The non-strict mode was added to handle "ChangeDeviceAll" utility that forces new id string
 * to be written over old id string (user has confirmed) - this to handle format changes in the
 * id string due to udev changes, etc (but serial number part is the same).
 * We define the serial number to be the string following the last underscore char in the id.
 */
static int same_disk_info(mdp_disk_t *disk, mdk_rdev_t *rdev, int strict)
{
        char *disk_sn, *rdev_sn, *ptr;

	if (disk->size != rdev->size)
		return 0;

        if (!strict && ((ptr = strrchr(disk->id, '_')) != NULL))
                disk_sn = ptr + 1;
        else
                disk_sn = disk->id;
        
        if (!strict && ((ptr = strrchr(rdev->id, '_')) != NULL))
                rdev_sn = ptr + 1;
        else
                rdev_sn = rdev->id;
        
	return (strcmp(disk_sn, rdev_sn) == 0);
}

/* record disk id to config data - note size is NOT recorded by this function */
static void record_disk_info(mdp_disk_t *disk, mdk_rdev_t *rdev)
{
	strcpy(disk->id, rdev->id);
}

/****************************************************************************/
/* Array device allocation */

/* Array states */
static char STARTED[] =                  "STARTED";
static char STOPPED[] =                  "STOPPED";
static char NEW_ARRAY[] =                "NEW_ARRAY";
static char RECON_DISK[] =               "RECON_DISK";
static char DISABLE_DISK[] =             "DISABLE_DISK";
static char SWAP_DSBL[] =                "SWAP_DSBL";
static char INVALID_EXPANSION[] =        "ERROR:INVALID_EXPANSION";
static char PARITY_NOT_BIGGEST[] =       "ERROR:PARITY_NOT_BIGGEST";
static char TOO_MANY_MISSING_DISKS[] =   "ERROR:TOO_MANY_MISSING_DISKS";
static char NEW_DISK_TOO_SMALL[] =       "ERROR:NEW_DISK_TOO_SMALL";
static char NO_DATA_DISKS[] =            "ERROR:NO_DATA_DISKS";

/* only supports one array now -tmm */
static mddev_t *mddev_map[1];

static inline mddev_t *dev_to_mddev(dev_t dev)
{
	BUG_ON((MAJOR(dev) != MAJOR_NR) || (MINOR(dev) >= MD_SB_DISKS));
	BUG_ON(mddev_map[0] == NULL);

	return mddev_map[0];
}

static void add_mddev_mapping(mddev_t *mddev)
{
	BUG_ON(mddev_map[0] != NULL);

	mddev_map[0] = mddev;
}

static void del_mddev_mapping(mddev_t *mddev)
{
	BUG_ON(mddev_map[0] != mddev);

	mddev_map[0] = NULL;
}

/* Deallocate a mddev control block.
 */
static void free_mddev(mddev_t *mddev)
{
	/* unregister the recovery thread */
	if (mddev->recovery_thread)
		md_unregister_thread(mddev->recovery_thread);

	/* remove array from mapping table */
	del_mddev_mapping(mddev);

	/* free memory */
	kfree(mddev);
}

static void md_do_recovery(mddev_t *mddev, unsigned long unused);  /* fwd ref */

/* Allocate memory and initialize a mddev control block.
 * Called when driver starts up.
 */
static mddev_t *alloc_mddev(dev_t dev)
{
	mddev_t *mddev;
	mdp_super_t *sb;
	int i;

	/* allocate memory for the mddev structure */
	mddev = (mddev_t *)kzalloc(sizeof(*mddev), GFP_KERNEL);
	if (!mddev)
		return NULL;
	add_mddev_mapping(mddev);

	/* setup */
	mddev->dev = dev;

	/* init recovery synchronization semaphore */
	mutex_init(&mddev->recovery_sem);

	/* create our recovery thread */
	mddev->recovery_thread = md_register_thread(md_do_recovery, mddev, 0, "mdrecoveryd");
	if (!mddev->recovery_thread) {
		printk("md: bug: couldn't allocate mdrecoveryd\n");
		free_mddev(mddev);
		return NULL;
	}

	/* init superblock update synchronization semaphore */
	mutex_init(&mddev->sb_sem);

	/* read the superblock */
	if (md_read_sb(mddev)) {
		printk("md: initializing superblock\n");
		init_sb(&mddev->sb);
		mark_sb_clean(&mddev->sb);
	}
	sb = &mddev->sb;
        sb->num_disks = 2;

	/* initialize */
	mddev->state = STOPPED;
        mddev->num_disks = 0;
	mddev->num_disabled = 0;
	mddev->num_replaced = 0;
	mddev->num_invalid = 0;
	mddev->num_missing = 0;
	mddev->num_wrong = 0;
	mddev->num_new = 0;;
        mddev->swap_p_idx = 0;
        mddev->swap_q_idx = 0;

	for (i = 0; i < MD_SB_DISKS; i++) {
		mdp_disk_t *disk = &sb->disks[i];
		mdk_rdev_t *rdev = &mddev->rdev[i];

		disk->number = i;
		rdev->status = DISK_NP;
	}

	return mddev;
}

static int is_parity_idx(int idx)
{
        return (idx == MD_SB_P_IDX || idx == MD_SB_Q_IDX);
}

/* Check that physical size of parity disk(s) is as large or larger than logical
 * size of all data disks.
 */
static int valid_parity_size(mddev_t *mddev)
{
	mdp_super_t *sb = &mddev->sb;
        unsigned long long smallest_parity = 0;
        unsigned long long largest_data = 0;
        int i;

        for (i = 0; i < MD_SB_DISKS; i++) {
                mdp_disk_t *disk = &sb->disks[i];
                mdk_rdev_t *rdev = &mddev->rdev[i];
                
                if (is_parity_idx(i))
                        smallest_parity = min_not_zero(smallest_parity, rdev->size);
                else
                        largest_data = max3(largest_data, rdev->size, disk->size);
        }

        return (smallest_parity == 0 || smallest_parity >= largest_data);
}

/* Check that physical size of any data disk marked DISK_DSBL_NEW or DISK_WRONG is as large
 * or larger than the recorded logical size.
 */
static int valid_replacement(mddev_t *mddev)
{
	mdp_super_t *sb = &mddev->sb;
        int i;

        for (i = 0; i < MD_SB_DISKS; i++) {
                mdp_disk_t *disk = &sb->disks[i];
                mdk_rdev_t *rdev = &mddev->rdev[i];
                
                if (!rdev->size || is_parity_idx(i))
                        continue;
                
                if ((rdev->status == DISK_DSBL_NEW || rdev->status == DISK_WRONG) && (rdev->size < disk->size))
                        return 0;
        }

        return 1;
}

static int find_disk_info(mddev_t *mddev, mdp_disk_t *disk)
{
        int i;

        for (i = 0; i < MD_SB_DISKS; i++) {
                mdk_rdev_t *rdev = &mddev->rdev[i];

                if (!is_parity_idx(i) && same_disk_info(disk, rdev, 0))
                        return i;
        }

        return 0;
}

static int import_slot(dev_t array_dev, int slot, char *name,
                       unsigned long offset, unsigned long long size,
                       int erased, char *id)
{
	mddev_t *mddev = dev_to_mddev(array_dev);
	mdp_super_t *sb = &mddev->sb;

	mdp_disk_t *disk = &sb->disks[slot];
	mdk_rdev_t *rdev = &mddev->rdev[slot];

	if (mddev->private) {
		printk("md: import_slot: already started\n");
		return -EINVAL;
	}

	/*** establish disk status ***/

	/* import the disk device */
	import_device(rdev, name,offset,size,erased,id,  mddev,slot);
        if (rdev->status == DISK_OK)
                mddev->num_disks++;

	if (disk_active(disk)) {
		if (disk_enabled(disk)) {
			if (rdev->status == DISK_NP) {
                                if (disk_valid(disk)) {
                                        printk("md: import_slot: %d missing\n", disk->number);
                                        rdev->status = DISK_NP_MISSING;
                                        mddev->num_missing++;
                                }
                                else {
                                        rdev->status = DISK_NP_DSBL;
                                        mark_disk_disabled(disk);
                                        mddev->num_disabled++;
                                }
			}
			else if (!same_disk_info(disk, rdev, 0)) {
                                if (disk_valid(disk)) {
                                        printk("md: import_slot: %d wrong\n", disk->number);
                                        rdev->status = DISK_WRONG;
                                        mddev->num_wrong++;
                                }
                                else {
                                        rdev->status = DISK_INVALID;
                                }
			}
			else if (!disk_valid(disk)) {
				rdev->status = DISK_INVALID;
			}
		}
		else {
			if (rdev->status == DISK_NP) {
				printk("md: import_slot: %d empty\n", disk->number);
				rdev->status = DISK_NP_DSBL;
			}
			else if (!same_disk_info(disk, rdev, 0)) {
				printk("md: import_slot: %d replaced\n", disk->number);
				rdev->status = DISK_DSBL_NEW;
				mddev->num_replaced++;
			}
			else
				rdev->status = DISK_DSBL;
			
			mddev->num_disabled++;
		}

		if (!disk_valid(disk))
			mddev->num_invalid++;

                if (!is_parity_idx(slot))
                        sb->num_disks = slot+2;
	}
	else {
                memset(disk->id, 0, sizeof(disk->id));
                disk->size = 0;

                disk->state = 0;
                if (rdev->status == DISK_OK) {
                        printk("md: disk%d new disk\n", disk->number);

                        rdev->status = DISK_NEW;
                        mddev->num_new++;

                        if (!is_parity_idx(slot))
                                sb->num_disks = slot+2;
                }
        }

	/*** now establish array state ***/

        /* assume we're just stopped */
        mddev->state = STOPPED;

        /* maybe parity assigned but no data disks */
        if (mddev->num_disks && (sb->num_disks == 2)) {
                mddev->state = NO_DATA_DISKS;
        }
        else
        /* verify parity disk(s) large enough */
        if (!valid_parity_size(mddev)) {
                mddev->state = PARITY_NOT_BIGGEST;
        }
        else
        /* check cases where new disks are detected */
        if (mddev->num_new) {
                /* check for new array special case */
                if (mddev->num_new == mddev->num_disks) {
                        mddev->state = NEW_ARRAY;
                }
                else
                /* cannot add new disks if any other config change */
                if (mddev->num_missing || mddev->num_wrong || mddev->num_replaced ||
                    (mddev->num_invalid != mddev->num_disabled)) {
                        mddev->state = INVALID_EXPANSION;
                }
        }
        else
        /* maybe we yanked some disks */
        if (mddev->num_missing) {
                if ((mddev->num_missing + mddev->num_invalid) <= 2 && !mddev->num_replaced && !mddev->num_wrong) {
                        mddev->state = DISABLE_DISK;
                }
                else {
                        mddev->state = TOO_MANY_MISSING_DISKS;
                }
        }
        else
        /* maybe we replaced one or two data disks */
        if (mddev->num_wrong || mddev->num_replaced) {
                int num_wrong = mddev->num_wrong;
                int swap_idx;

                mddev->state = RECON_DISK;

                if (mddev->rdev[MD_SB_P_IDX].status == DISK_WRONG) {
                        swap_idx = find_disk_info(mddev, &sb->disks[MD_SB_P_IDX]);
                        if (swap_idx && (mddev->rdev[swap_idx].status == DISK_DSBL_NEW)) {
                                mddev->swap_p_idx = swap_idx;
                                mddev->state = SWAP_DSBL;
                                num_wrong--;
                        }
                }
                if (mddev->rdev[MD_SB_Q_IDX].status == DISK_WRONG) {
                        swap_idx = find_disk_info(mddev, &sb->disks[MD_SB_Q_IDX]);
                        if (swap_idx && (mddev->rdev[swap_idx].status == DISK_DSBL_NEW)) {
                                mddev->swap_q_idx = swap_idx;
                                mddev->state = SWAP_DSBL;
                                num_wrong--;
                        }
                }

                if ((num_wrong + mddev->num_invalid) <= 2) {
                        if (!valid_replacement(mddev)) {
                                mddev->state = NEW_DISK_TOO_SMALL;
                        }
                }
                else {
                        mddev->state = TOO_MANY_MISSING_DISKS;
                }
        }

	return 0;
}

/* called on read error, with device_lock held */
void md_read_error(mddev_t *mddev, int disk_number, sector_t sector)
{
        printk("md: disk%d read error, sector=%llu\n", disk_number, (unsigned long long)sector);
        mddev->rdev[disk_number].errors++;
}

/* called on write error, with device_lock held */
int md_write_error(mddev_t *mddev, int disk_number, sector_t sector)
{
        mdp_disk_t *disk = &mddev->sb.disks[disk_number];
        mdk_rdev_t *rdev = &mddev->rdev[disk_number];
        int update_sb = 0;

        printk("md: disk%d write error, sector=%llu\n", disk_number, (unsigned long long)sector);
        rdev->errors++;

	if (disk_active(disk)) {
                /* an active array disk failed */
                if (disk_enabled(disk) && (mddev->num_disabled < 2)) {
                        /* if a replacement disk that failed */
                        if (!disk_valid(disk)) {
                                /* mark the disk disabled again */
                                rdev->status = DISK_DSBL;
                                mark_disk_disabled(disk);
                                mddev->num_disabled++;
                                /* stop recovery if it's running */
                                md_interrupt_thread(mddev->recovery_thread);
                                /* config changed */
                                update_sb++;
                        }
                        else if (mddev->num_invalid < 2) {
                                /* mark the failing disk disabled and "not valid" */
                                rdev->status = DISK_DSBL;
                                mark_disk_disabled(disk);
                                mddev->num_disabled++;
                                mark_disk_invalid(disk);
                                mddev->num_invalid++;
                                if (mddev->num_disabled == 2) {
                                        /* stop recovery if it's running */
                                        md_interrupt_thread(mddev->recovery_thread);
                                }
                                /* config changed */
                                update_sb++;
                        }
                }
        }
        else {
                if (disk_enabled(disk)) {
                        /* must be a disk we're clearing */
                        mark_disk_disabled(disk);
                        mddev->num_new--;
                        if (mddev->num_new == 0) {
                                /* stop recovery (clearing) if it's running */
                                md_interrupt_thread(mddev->recovery_thread);
                        }

                        /* config changed */
                        update_sb++;
                }
        }

        return update_sb;
}

static void md_submit_bio(struct bio *bi)
{
        mddev_t *mddev = bi->bi_bdev->bd_disk->private_data;
        int unit = bi->bi_bdev->bd_disk->first_minor;
        mdp_disk_t *disk = &mddev->sb.disks[unit];

        /* verify this unit is active */
        if (!disk_active(disk)) {
                bio_io_error(bi);
                return;
        }
        
        bi = bio_split_to_limits(bi);
        bi->bi_opf &= ~REQ_NOMERGE;

        unraid_make_request(mddev, unit, bi);
}

static int md_open(struct gendisk *gd, fmode_t mode)
{
	mddev_t *mddev = gd->private_data;

	if (!mddev) {
		printk("md_open: no mddev\n");
		return -ENODEV;
	}

	/* increment the usage count */
	atomic_inc(&mddev->active);
	return 0;
}

static void md_release(struct gendisk *gd)
{
	mddev_t *mddev = gd->private_data;

	if (!mddev) {
		printk("md_release: no mddev\n");
		return;
	}

	/* decrement the usage count */
	atomic_dec(&mddev->active);
}

static struct block_device_operations md_fops =
{
	.owner          = THIS_MODULE,
	.submit_bio 	= md_submit_bio,
	.open           = md_open,
	.release        = md_release,
//	.ioctl          = md_ioctl,
//	.getgeo         = md_getgeo,
//	.media_changed  = md_media_changed,
//	.revalidate_disk= md_revalidate,
};

static int do_run(mddev_t *mddev)
{
	mdp_super_t *sb = &mddev->sb;
	int i, err;

	/* lock the devices */
	for (i = 0; i < MD_SB_DISKS; i++) {
		mdk_rdev_t *rdev = &mddev->rdev[i];

		/* only lock present devices */
                if (!strstr(rdev->status, "DISK_NP")) {
			err = lock_bdev(rdev->name, rdev);
			if (err) {
				printk("md: do_run: lock_bdev error: %d\n", err);
				return err; /* partition not present */
			}
			/* sync device & invalidate cache buffers */
			sync_blockdev(rdev->bdev);
			invalidate_bdev(rdev->bdev);
		}
	}

	/* alloc transfer resources */
	err = unraid_run(mddev);
	if (err) {
		printk("md: unraid_run: failed: %d\n", err);
		return -EINVAL;
	}

        /* create the md devices */
	for (i = 1; i <= 28; i++) {
		mdp_disk_t *disk = &sb->disks[i];

		if (disk_active(disk) || disk_enabled(disk)) {
			int unit = disk->number;
			struct gendisk *gd = blk_alloc_disk(NUMA_NO_NODE);
                        int ret;

			mddev->gendisk[unit] = gd;

			gd->major = MAJOR(mddev->dev);
			gd->first_minor = unit;
                        gd->minors = 1;
			sprintf(gd->disk_name, "nmd%dp1", unit);
			gd->fops = &md_fops;
			gd->private_data = mddev;
			gd->queue->queuedata = mddev;

                        /* capacity in 512-byte sectors */
                        set_capacity(gd, disk->size*2);

                        blk_set_stacking_limits(&gd->queue->limits);
			blk_queue_write_cache(gd->queue, true, true);

                        if (md_restrict & 1)
                                blk_queue_max_hw_sectors(gd->queue, 256);  /* 256 sectors => 128K */

                        blk_queue_max_write_zeroes_sectors(gd->queue, 0);
                        blk_queue_max_discard_sectors(gd->queue, 0);
                        blk_queue_flag_clear(QUEUE_FLAG_NONROT, gd->queue);

			ret = add_disk(gd);
			printk("nmd%dp1: running, size: %llu blocks\n", unit, disk->size);
		}
	}
	
	return 0;
}

static int do_stop(mddev_t *mddev)
{
	mdp_super_t *sb = &mddev->sb;
	int i;

	/* remove md devices */
	for (i = 1; i <= 28; i++) {
		mdp_disk_t *disk = &sb->disks[i];
                int unit = disk->number;

                if (mddev->gendisk[unit]) {
			printk("nmd%dp1: stopping\n", unit);

			del_gendisk(mddev->gendisk[unit]);
			put_disk(mddev->gendisk[unit]);

			mddev->gendisk[unit] = NULL;
		}
	}

	/* free transfer resources  */
	unraid_stop(mddev);

	/* unlock the disk partitions */
	for (i = 0; i < MD_SB_DISKS; i++) {
		mdk_rdev_t *rdev = &mddev->rdev[i];

		unlock_bdev(rdev);
	}

	return 0;
}

/****************************************************************************/
/* Sync */

/* to limit messages to system log */
#define SYNC_ERROR_LIMIT 100

/* called on a sync error */
void md_sync_error(mddev_t *mddev, sector_t sector, char *message)
{
        mdp_super_t *sb = &mddev->sb;
        
        sb->sync_errs++;
        
        /* limit number of messages generated */
        if (sb->sync_errs <= SYNC_ERROR_LIMIT)
                printk("md: recovery thread: %s, sector=%llu\n", message, (unsigned long long)sector);
        if (sb->sync_errs == SYNC_ERROR_LIMIT+1)
                printk("md: recovery thread: stopped logging\n");
}

/* called when a stripe sync completes */
void md_sync_done(mddev_t *mddev, sector_t sector, int count)
{
        /* another "count" sectors have been sync'ed */
        atomic_sub(count, &mddev->recovery_active);
        wake_up(&mddev->recovery_wait);
}

/* SYNC_MARKS * SYNC_MARK_STEP is the number of past seconds that rate is calcuated over */
#define SYNC_MARKS	10
#define	SYNC_MARK_STEP	(3*HZ)

static int md_do_sync(mddev_t *mddev)
{
	unsigned long start_time, last_mark;
	unsigned long mark[SYNC_MARKS];
	unsigned long long mark_cnt[SYNC_MARKS];
	int err = 0;
	int i;

	start_time = jiffies;
	for (i = 0; i < SYNC_MARKS; i++) {
		mark[i] = start_time;
		mark_cnt[i] = mddev->curr_resync;
	}
	last_mark = 0;
	mddev->resync_mark = mark[last_mark];
	mddev->resync_mark_cnt = mark_cnt[last_mark];

	init_waitqueue_head(&mddev->recovery_wait);
	atomic_set(&mddev->recovery_active, 0);

	while (mddev->curr_resync < mddev->recovery_running) {
		int sectors = unraid_sync(mddev, mddev->curr_resync);

		mddev->curr_resync += sectors;
		atomic_add(sectors, &mddev->recovery_active);

		if (jiffies >= mark[last_mark] + SYNC_MARK_STEP) {
                        unsigned long long prev_cnt = mark_cnt[last_mark];

			/* step marks */
			last_mark = (last_mark+1) % SYNC_MARKS;

			mddev->resync_mark = mark[last_mark];
			mddev->resync_mark_cnt = mark_cnt[last_mark];

			mark[last_mark] = jiffies;
			mark_cnt[last_mark] = mddev->curr_resync - atomic_read(&mddev->recovery_active);

			dprintk("md: curr_resync=%llu delta=%llu\n",
                                mddev->curr_resync, mark_cnt[last_mark] - prev_cnt);
		}

		if (signal_pending(current)) {
			/* got a signal, exit */
			dprintk("md: md_do_sync: got signal, exit...\n");
			flush_signals(current);
			err = -EINTR;
			break;
		}
	}

	wait_event(mddev->recovery_wait, (atomic_read(&mddev->recovery_active) == 0));
	return err;
}

/*
 * This is a kernel thread which syncs a spare disk with the active array. If no disk is
 * invalid, then we execute a "check".
 */
static void md_do_recovery(mddev_t *mddev, unsigned long unused)
{
	mdp_super_t *sb = &mddev->sb;

	/* if nothing to resync, get out now */
	if (!mddev->recovery_size) {
		printk("md: recovery thread: nothing to resync\n");
		return;
	}

	/* mutex_lock_interruptible() returns 0 if you got the lock,
	 * or -EINTR if the process was interrupted with a signal.
	 */
	if (mutex_lock_interruptible(&mddev->recovery_sem)) {
		printk("md: recovery thread: signal pending?\n");
		flush_signals(current);
		return;
	}
        printk("md: recovery thread: %s ...\n", mddev->recovery_action);
	mddev->recovery_running = mddev->recovery_size*2; /* count of sectors */

        /* record start of resync */
        sb->stime = get_seconds();
        sb->stime2 = 0;
        sb->sync_exit = 0;
	md_update_sb(mddev);

        /* execute resync */
	sb->sync_exit = md_do_sync(mddev);
        sb->stime2 = get_seconds();

	if (sb->sync_exit == 0) {
                int i;
                        
                /* After successful rebuild, invalid enabled disk(s) now valid and
                 * new disks are now active.
                 */
                for (i = 0; i < MD_SB_DISKS; i++) {
                        mdp_disk_t *disk = &sb->disks[i];
                        mdk_rdev_t *rdev = &mddev->rdev[i];
                                
                        if (disk_enabled(disk) && !disk_valid(disk)) {
                                if (rdev->status == DISK_NEW) {
                                        mark_disk_active(disk);
                                        mddev->num_new--;
                                }
                                else {
                                        mddev->num_invalid--;
                                }
                                mark_disk_valid(disk);
                                rdev->status = DISK_OK;
                        }
                }

                mddev->curr_resync = 0;
                printk("md: sync done. time=%usec\n", sb->stime2 - sb->stime);
	}

	/* record sync result */
	md_update_sb(mddev);

	mddev->recovery_running = 0;
	mutex_unlock(&mddev->recovery_sem);

	printk("md: recovery thread: exit status: %d\n", sb->sync_exit);
}

/****************************************************************************/
/* Array operations */

static int start_array(dev_t array_dev, char *state)
{
	mddev_t *mddev = dev_to_mddev(array_dev);
	mdp_super_t *sb;
	int err;

        int update_sb = 0;

	if (mddev->private) {
		printk("md: start_array: already started\n");
		return -EINVAL;
	}

	/* ensure valid state */
	if (strstr(mddev->state, "ERROR:")) {
		printk("md: start_array: %s\n", mddev->state);
		return -EINVAL;
	}
	
	/* ensure no state change */
	if (strcmp(state, mddev->state)) {
		printk("md: start_array: state %s does't match %s\n", state, mddev->state);
		return -EINVAL;
	}
	
	sb = &mddev->sb;

        /* new array (both P and Q are invalid) */
	if (mddev->state == NEW_ARRAY) {
		int i;

                if (invalidslota != MD_SB_P_IDX)
                        printk("md: invalidslota=%d\n", invalidslota);
                if (invalidslotb != MD_SB_Q_IDX)
                        printk("md: invalidslotb=%d\n", invalidslotb);

                sb->num_disks = 2;
                mddev->num_disabled = 0;
                mddev->num_replaced = 0;
                mddev->num_invalid = 0;
                mddev->num_missing = 0;
                mddev->num_wrong = 0;
                mddev->num_new = 0;;
                mddev->swap_p_idx = 0;
                mddev->swap_q_idx = 0;

                for (i = 0; i < MD_SB_DISKS; i++) {
                        mdp_disk_t *disk = &sb->disks[i];
                        mdk_rdev_t *rdev = &mddev->rdev[i];

                        disk->state = 0;

                        /* special handling for parity disks */
                        if (i == invalidslota || i == invalidslotb) {
                                /* P/Q always active */
                                mark_disk_active(disk);

                                /* if device present, mark enabled */
                                if (rdev->size) {
                                        rdev->status = DISK_INVALID;
                                        mark_disk_enabled(disk);
                                }
                                else {
                                        rdev->status = DISK_NP_DSBL;
                                        mark_disk_disabled(disk);
                                        mddev->num_disabled++;
                                }

                                /* parity disks start out invalid */
                                mark_disk_invalid(disk);
                                mddev->num_invalid++;
                        }
                        else if (rdev->size) {
                                /* data disks start out valid */
                                rdev->status = DISK_OK;
                                mark_disk_active(disk);
                                mark_disk_enabled(disk);
                                mark_disk_valid(disk);
                        }
                        else {
                                /* empty slot */
                                rdev->status = DISK_NP;
                        }

                        /* record disk information */
                        record_disk_info(disk, rdev);
                        disk->size = rdev->size;

                        /* array width for Q calculation */
                        if (disk_active(disk) && !is_parity_idx(i))
                                sb->num_disks = i+2;
                }

                update_sb++;
        }
	else
	if (mddev->state == DISABLE_DISK) {
		int i;

		for (i = 0; i < MD_SB_DISKS; i++) {
			mdp_disk_t *disk = &sb->disks[i];
			mdk_rdev_t *rdev = &mddev->rdev[i];

                        if (rdev->status == DISK_NP_MISSING) {
                                /* disable the disk */
                                rdev->status = DISK_NP_DSBL;

                                mark_disk_disabled(disk);
                                mddev->num_disabled++;

                                mark_disk_invalid(disk);
                                mddev->num_invalid++;

                                /* record (cleared) disk information */
                                record_disk_info(disk, rdev);

                                mddev->num_missing--;

                                update_sb++;
                        }
                }
	}
        else
	if (mddev->state == RECON_DISK || mddev->state == SWAP_DSBL) {
		int i;

		for (i = 0; i < MD_SB_DISKS; i++) {
			mdp_disk_t *disk = &sb->disks[i];
			mdk_rdev_t *rdev = &mddev->rdev[i];

                        if (rdev->status == DISK_WRONG) {
                                if (i == MD_SB_P_IDX && mddev->swap_p_idx) {
                                        mddev->swap_p_idx = 0;

                                        /* status is 'ok' */
                                        rdev->status = DISK_OK;
                                }
                                else
                                if (i == MD_SB_Q_IDX && mddev->swap_q_idx) {
                                        mddev->swap_q_idx = 0;

                                        /* status is 'ok' */
                                        rdev->status = DISK_OK;
                                }
                                else {
                                        /* if not already invalid, mark disk invalid */
                                        if (disk_valid(disk)) {
                                                mark_disk_invalid(disk);
                                                mddev->num_invalid++;
                                        }

                                        /* status is 'invalid' */
                                        rdev->status = DISK_INVALID;
                                }

                                /* record disk information */
                                record_disk_info(disk, rdev);
                                disk->size = rdev->size;

                                mddev->num_wrong--;

                                update_sb++;
                        }
                        else
                        if (rdev->status == DISK_DSBL_NEW) {
                                /* enable the disk */
                                mark_disk_enabled(disk);
                                mddev->num_disabled--;

                                /* status is 'invalid' */
                                rdev->status = DISK_INVALID;

                                /* record disk information */
                                record_disk_info(disk, rdev);
                                disk->size = rdev->size;

                                mddev->num_replaced--;

                                update_sb++;
                        }
                }
	}

	/* check if a disabled disk has been removed */
	if (mddev->num_disabled) {
		int i;

		for (i = 0; i < MD_SB_DISKS; i++) {
			mdp_disk_t *disk = &sb->disks[i];
			mdk_rdev_t *rdev = &mddev->rdev[i];

                        if ((rdev->status == DISK_NP_DSBL) && !same_disk_info(disk, rdev, 1)) {
                                /* record (clear) disk information */
                                record_disk_info(disk, rdev);

                                update_sb++;
                        }
                }
        }

	/* check if new data disks added */
	if (mddev->num_new) {
                int parity_valid = (disk_valid(&sb->disks[MD_SB_P_IDX]) || disk_valid(&sb->disks[MD_SB_Q_IDX]));
		int i;

		for (i = 0; i < MD_SB_DISKS; i++) {
			mdp_disk_t *disk = &sb->disks[i];
			mdk_rdev_t *rdev = &mddev->rdev[i];

                        if (rdev->status == DISK_NEW) {
                                mark_disk_enabled(disk);

                                if (rdev->erased || !parity_valid) {
                                        rdev->status = DISK_OK;
                                        mark_disk_active(disk);
                                        mark_disk_valid(disk);
                                        mddev->num_new--;
                                }

                                /* record disk information */
                                record_disk_info(disk, rdev);
                                disk->size = rdev->size;

                                update_sb++;
                        }
                }
        }

	/* gitty up */
        if (sb->num_disks > 2) {
                err = do_run(mddev);
                if (err) {
                        do_stop(mddev);
                        return err;
                }
	}

	mddev->state = STARTED;

	/* commit the superblock */
        if (update_sb)
                md_update_sb(mddev);

	return 0;
}

/* Stop a running array.
 * int notifier is true if called from notify_reboot, in this case we don't
 * write the super.dat file because the filesystem is likely unmounted.
 */
static int stop_array(dev_t array_dev, int notifier)
{
	mddev_t *mddev = dev_to_mddev(array_dev);
	int active;

	/* check if still in use */
	active = atomic_read(&mddev->active);
	if (active) {
		printk("md: %d devices still in use.\n", active);
		return -EBUSY;
	}

	/* stop the recovery thread */
	md_interrupt_thread(mddev->recovery_thread);
	mutex_lock(&mddev->recovery_sem);
	mutex_unlock(&mddev->recovery_sem);
        mddev->curr_resync = 0;

	if (mddev->private) {
                do_stop(mddev);
        }
	mddev->state = STOPPED;

	return 0;
}

/* Kick the recovery thread for the array.  If one disk is invalid, this will start a
 * reconstruct of that disk.  If all disks are valid, then this will start a parity check, and
 * in this case, input 'option' indicates whether to correct bad parity or not:
 * "CORRECT", or
 * "NOCORRECT".
 */
static int check_array(dev_t array_dev, char *option, unsigned long long offset)
{
	mddev_t *mddev = dev_to_mddev(array_dev);
	mdp_super_t *sb = &mddev->sb;
        int recovery_option, recovery_resume;

	if (!mddev->private) {
		printk("md: check_array: not started\n");
		return -EINVAL;
	}
	if (sb->num_disks <= 2) {
		printk("md: check_array: no devices\n");
		return -EINVAL;
	}

	/* process option */
	if (strcasecmp(option, "NOCORRECT") == 0) {
                recovery_option = 0;
                recovery_resume = 0;
        }
	else if (strcasecmp(option, "CORRECT") == 0) {
                recovery_option = 1;
                recovery_resume = 0;
        }
	else if (strcasecmp(option, "RESUME") == 0) {
                recovery_resume = 1;
	}
	else {
		printk("md: check_array: invalid option: %s\n", option);
		return -EINVAL;
	}
        /* validate offset */
        if (offset >= (mddev->recovery_size*2) || (offset%8)) {
		printk("md: check_array: invalid offset %llu\n", offset);
		return -EINVAL;
	}

        /* if recovery already running, just exit */
        if (mddev->recovery_running)
                return 0;

        /* if resume indicated but not paused, just exit */
        if (recovery_resume) {
                if (mddev->curr_resync == 0)
                        return 0;
        }
        else {
		mddev->recovery_option = recovery_option;
                mddev->curr_resync = offset;
                mddev->sb.sync_errs = 0;
        }

        /* kick the thread */
        md_wakeup_thread(mddev->recovery_thread);
	return 0;
}

/* Stop a running parity check operation.
 */
static int nocheck_array(dev_t array_dev, char *option)
{
	mddev_t *mddev = dev_to_mddev(array_dev);
        int recovery_pause;

	if (!mddev->private) {
		printk("md: nocheck_array: not started\n");
		return -EINVAL;
	}

	/* process the option */
	if (strcasecmp(option, "CANCEL") == 0)
		recovery_pause = 0;
	else if (strcasecmp(option, "PAUSE") == 0)
		recovery_pause = 1;
	else {
		printk("md: nocheck_array: invalid option: %s\n", option);
		return -EINVAL;
	}

        /* stop the recovery thread */
        md_interrupt_thread(mddev->recovery_thread);
        mutex_lock(&mddev->recovery_sem);
        mutex_unlock(&mddev->recovery_sem);

        if (!recovery_pause) {
                mddev->curr_resync = 0;
        }
	return 0;
}

/* Set mddev label.
 */
static int label_array(dev_t array_dev, char *label)
{
	mddev_t *mddev = dev_to_mddev(array_dev);

	if (mddev->private) {
		printk("md: label_array: already started\n");
		return -EINVAL;
	}

        if (strlen(label) >= sizeof(mddev->sb.label)) {
		printk("md: label_array: invalid label\n");
		return -EINVAL;
        }

        memset(mddev->sb.label, 0, sizeof(mddev->sb.label));
        strcpy(mddev->sb.label, label);
	md_update_sb(mddev);
	return 0;
}

/* Dump debug info.
 */
static int dump_array(dev_t array_dev)
{
	mddev_t *mddev = dev_to_mddev(array_dev);

        printk("md_num_stripes=%d\n", md_num_stripes);
        printk("md_write_method=%d\n", md_write_method);
        printk("md_queue_limit=%d\n", md_queue_limit);
        printk("md_sync_limit=%d\n", md_sync_limit);
        printk("md_restrict=%d\n", md_restrict);
        printk("recovery_active=%d\n", atomic_read(&mddev->recovery_active)/(int)(PAGE_SIZE/512));
        
	if (mddev->private)
		unraid_dump(mddev);

	return 0;
}

/* Get status
 */
static void status_sb(struct seq_file *seq, mdp_super_t *sb)
{
	seq_printf(seq, "sbName=%s\n", super);
	seq_printf(seq, "sbVersion=%d.%d.%d\n", sb->major_version, sb->minor_version, sb->patch_version);
	seq_printf(seq, "sbCreated=%u\n", sb->ctime);
	seq_printf(seq, "sbUpdated=%u\n", sb->utime);
	seq_printf(seq, "sbEvents=%d\n", sb->events);
	seq_printf(seq, "sbState=%d\n", sb->state);
        seq_printf(seq, "sbNumDisks=%d\n", sb->num_disks);
	seq_printf(seq, "sbLabel=%s\n", sb->label);

	seq_printf(seq, "sbSynced=%u\n", sb->stime);
	seq_printf(seq, "sbSynced2=%u\n", sb->stime2);
	seq_printf(seq, "sbSyncErrs=%u\n", sb->sync_errs);
	seq_printf(seq, "sbSyncExit=%d\n", sb->sync_exit);
}

static void status_resync(mddev_t *mddev)
{
	mdp_super_t *sb = &mddev->sb;
        int i;

        *mddev->recovery_action = '\0';
        mddev->recovery_size = 0;

        if (mddev->state == NEW_ARRAY) {
                /* sync P and/or Q if present
                 */
                if (mddev->rdev[MD_SB_P_IDX].size || mddev->rdev[MD_SB_Q_IDX].size) {
                        strcpy(mddev->recovery_action, "recon");

                        if (mddev->rdev[MD_SB_P_IDX].size)
                                strcat(mddev->recovery_action, " P");
                        if (mddev->rdev[MD_SB_Q_IDX].size)
                                strcat(mddev->recovery_action, " Q");
                }
                else
                        strcpy(mddev->recovery_action, "check");
        }
        else
        if (mddev->num_invalid != mddev->num_disabled) {
                /* reconstruct
                 */
                strcpy(mddev->recovery_action, "recon");

                /* find the target disk(s) */
                for (i = 0; i < MD_SB_DISKS; i++) {
                        mdp_disk_t *disk = &sb->disks[i];
                                
                        if (disk_enabled(disk) && !disk_valid(disk)) {
                                if (i == MD_SB_P_IDX)
                                        strcat(mddev->recovery_action, " P");
                                else if (i == MD_SB_Q_IDX)
                                        strcat(mddev->recovery_action, " Q");
                                else
                                        sprintf(mddev->recovery_action+strlen(mddev->recovery_action), " D%i", i);

                                mddev->recovery_size = max(mddev->recovery_size, disk->size);
                        }
                }
        }
        else
        if (mddev->num_new && (disk_valid(&sb->disks[MD_SB_P_IDX]) || disk_valid(&sb->disks[MD_SB_Q_IDX]))) {
                /* clear new data disks if array parity is valid
                 */
                strcpy(mddev->recovery_action, "clear");

                for (i = 0; i < MD_SB_DISKS; i++) {
                        mdk_rdev_t *rdev = &mddev->rdev[i];
                
                        if (rdev->status == DISK_NEW) {
                                mddev->recovery_size = max(mddev->recovery_size, rdev->size);
                        }
                }
        }
        else
        if (sb->num_disks > 2) {
                /* read all data disks and check P and/or Q if present */
                /* note: if it's a single disabled data disk we are really just checking Q
                 * because check_parity() will generate D from P and then check Q.
                 */
                strcpy(mddev->recovery_action, "check");

                if (mddev->num_disabled <= 1) {
                        if (disk_valid(&sb->disks[MD_SB_P_IDX]))
                                strcat(mddev->recovery_action, " P");
                        if (disk_valid(&sb->disks[MD_SB_Q_IDX]))
                                strcat(mddev->recovery_action, " Q");
                }

                for (i = 0; i < MD_SB_DISKS; i++) {
                        mdk_rdev_t *rdev = &mddev->rdev[i];
                
                        mddev->recovery_size = max(mddev->recovery_size, rdev->size);
                }
        }
}

static void status_md(struct seq_file *seq, mddev_t *mddev)
{
	seq_printf(seq, "mdVersion=%d.%d.%d\n",
		   MD_MAJOR_VERSION, MD_MINOR_VERSION, MD_PATCHLEVEL_VERSION);
	seq_printf(seq, "mdState=%s\n", mddev->state);
        seq_printf(seq, "mdNumDisks=%d\n", mddev->num_disks);
	seq_printf(seq, "mdNumDisabled=%d\n", mddev->num_disabled);
	seq_printf(seq, "mdNumReplaced=%d\n", mddev->num_replaced);
	seq_printf(seq, "mdNumInvalid=%d\n", mddev->num_invalid);
	seq_printf(seq, "mdNumMissing=%d\n", mddev->num_missing);
	seq_printf(seq, "mdNumWrong=%d\n", mddev->num_wrong);
	seq_printf(seq, "mdNumNew=%d\n", mddev->num_new);
	seq_printf(seq, "mdSwapP=%d\n", mddev->swap_p_idx);
	seq_printf(seq, "mdSwapQ=%d\n", mddev->swap_q_idx);

        status_resync(mddev);

	seq_printf(seq, "mdResyncAction=%s\n", mddev->recovery_action);
	seq_printf(seq, "mdResyncSize=%llu\n", mddev->recovery_size);
	seq_printf(seq, "mdResyncCorr=%d\n", mddev->recovery_option);
	seq_printf(seq, "mdResync=%llu\n", mddev->recovery_running/2);

	if (mddev->recovery_running) {
		unsigned long long resync;
		unsigned long dt;
		unsigned long long db;

		/* compute number of 1024-byte blocks which have completed */
		resync = (mddev->curr_resync - atomic_read(&mddev->recovery_active))/2;
		seq_printf(seq, "mdResyncPos=%llu\n", resync);

		/* time delta in seconds */
		dt = ((jiffies - mddev->resync_mark) / HZ);
		if (!dt) dt++;
		seq_printf(seq, "mdResyncDt=%lu\n", dt);
		
		/* resync'ed blocks delta */
		db = resync - (mddev->resync_mark_cnt/2);
		seq_printf(seq, "mdResyncDb=%llu\n", db);
	}
	else {
		seq_printf(seq, "mdResyncPos=%llu\n", mddev->curr_resync/2);
		seq_printf(seq, "mdResyncDt=0\n");
		seq_printf(seq, "mdResyncDb=0\n");
        }
}

static void status_disk(struct seq_file *seq, mdp_disk_t *disk, mdk_rdev_t *rdev)
{
	int number = disk->number;

        seq_printf(seq, "diskNumber.%d=%d\n", number, number);
	if ((disk_active(disk) || disk_enabled(disk)) && !is_parity_idx(number))
		seq_printf(seq, "diskName.%d=nmd%dp1\n", number, number);
	else
		seq_printf(seq, "diskName.%d=\n", number);
	seq_printf(seq, "diskSize.%d=%llu\n", number, disk->size);

	seq_printf(seq, "diskState.%d=%d\n", number, disk->state);
	seq_printf(seq, "diskId.%d=%s\n", number, disk->id);

        seq_printf(seq, "rdevNumber.%d=%d\n", number, number);
	seq_printf(seq, "rdevStatus.%d=%s\n", number, rdev->status);
	seq_printf(seq, "rdevName.%d=%s\n", number, rdev->name);

	seq_printf(seq, "rdevOffset.%d=%lu\n", number, rdev->offset);
	seq_printf(seq, "rdevSize.%d=%llu\n", number, rdev->size);
	seq_printf(seq, "rdevId.%d=%s\n", number, rdev->id);

	seq_printf(seq, "rdevReads.%d=%lu\n", number, rdev->reads);
	seq_printf(seq, "rdevWrites.%d=%lu\n", number, rdev->writes);
	seq_printf(seq, "rdevNumErrors.%d=%lu\n", number, rdev->errors);
}
	
static int md_status(struct seq_file *seq, dev_t array_dev)
{
	mddev_t *mddev = dev_to_mddev(array_dev);
	mdp_super_t *sb = &mddev->sb;
	int i;

	status_sb(seq, sb);
	status_md(seq, mddev);
	for (i = 0; i < MD_SB_DISKS; i++) {
		mdp_disk_t *disk = &sb->disks[i];
		mdk_rdev_t *rdev = &mddev->rdev[i];

		status_disk(seq, disk, rdev);
	}
			
	return 0;
}

/****************************************************************************/
/* Command/staus interface.  All communication with driver is through
 * the /proc virtual file system.  We don't use ioctl().
 */

/* Caution: modifies the input buffer. */
static char *get_token(char **bufp, char *delim) {
	char *ptr = *bufp;
	char *token;
	
	/*skip leading delimeters */
	while (strchr(delim, *ptr) != NULL) {
		if (*ptr == '\0')
			break;
		ptr++;
	}
	
	if (*ptr != '\0') {
		token = ptr;
		
		/*skip over token */
		while (strchr(delim, *ptr) == NULL) {
			if (*ptr == '\0')
				break;
			ptr++;
		}
		if (*ptr != '\0')
			*ptr++ = '\0';
	}
	else
		token = NULL;
	
	/* update buffer pointer */
	*bufp = ptr;
	
	return token;
}

static int cmd_seq = 0;  /* command sequence number */

static ssize_t md_proc_write(struct file *file, const char *buffer,
			      size_t count, loff_t *offset)
{
	char buf[256], *bufp, *token, *temp;
	char *delim = " ,\t\n";
	int result;
	
	if (count > sizeof(buf)-1)
		return -EINVAL;

	if (copy_from_user(buf, buffer, count))
                return -EFAULT;
	buf[count] = '\0';
	
	bufp = buf;
	token = get_token(&bufp, delim);
	if (token == NULL)
		return -EINVAL;
	
	/* dummy command as of 4.6 */
	if (!strcmp("status", token))
		return count;

	cmd_seq++;
	
	if (md_trace)
		printk("nmdcmd (%d): %s %s\n", cmd_seq, token, bufp);
	
	if (!strcmp("set", token)) {
		char *name = "";
		int value = 0;
		unsigned long long u64_value = 0;
		
		result = 0; /* assume ok */

		/* this is the subcommand string */
		if ((token = get_token(&bufp, delim)) != NULL)
			name = token;

		/* this is the first numeric value after the subcommand */
		if ((token = get_token(&bufp, delim)) != NULL) {
			value = simple_strtol(token, &temp, 10);
			u64_value = simple_strtoll(token, &temp, 10);
		}
		
		/* process the subcommand */

		if (!strcmp("md_trace", name))
			md_trace = token ? value : MD_TRACE;
		else
		if (!strcmp("md_num_stripes", name)) {
			dev_t array_dev = MKDEV(MAJOR_NR,0);
                        mddev_t *mddev = dev_to_mddev(array_dev);

                        int num_stripes = token ? value : MD_NUM_STRIPES;

                        if (mddev->private)
                                md_num_stripes = unraid_num_stripes(mddev, num_stripes);
                        else
                                md_num_stripes = num_stripes;
                }
		else
		if (!strcmp("md_queue_limit", name)) {
			md_queue_limit = token ? value : MD_QUEUE_LIMIT;
                        if (md_queue_limit < 1)
                                md_queue_limit = 1;
                        else if (md_queue_limit > 100)
                                md_queue_limit = 100;
                }
		else
		if (!strcmp("md_sync_limit", name)) {
			md_sync_limit = token ? value : MD_SYNC_LIMIT;
                        if (md_sync_limit < 0)
                                md_sync_limit = 0;
                        else if (md_sync_limit > 100)
                                md_sync_limit = 100;
                }
		else
                if (!strcmp("md_write_method", name)) {
                        if (token) {
                                if ((value != READ_MODIFY_WRITE) && (value != RECONSTRUCT_WRITE))
                                        result = -EINVAL; /* fail */
                                else
                                        md_write_method = value;
                        }
                        else
                                md_write_method = READ_MODIFY_WRITE;
                }
                else
                if (!strcmp("md_restrict", name)) {
			md_restrict = token ? value : 1;
                }
                else
	        if (!strcmp("invalidslot", name)) {
			/* invalidslot slota slotb */
                        if (token) {
                                invalidslota = value;
                                invalidslotb = 99; /* just a big number */
                                if ((token = get_token(&bufp, delim)) != NULL)
                                        invalidslotb = simple_strtol(token, &temp, 10);
                        }
                        else {
                                invalidslota = MD_SB_P_IDX;
                                invalidslotb = MD_SB_Q_IDX;
                        }
                }
		else
	        if (!strcmp("resync_start", name))
			md_resync_start = token ? u64_value : 0;
		else
		if (!strcmp("resync_end", name))
			md_resync_end = token ? u64_value : 0;
		else
		if (!strcmp("rderror", name)) {
			dev_t array_dev = MKDEV(MAJOR_NR,0);
                        mddev_t *mddev = dev_to_mddev(array_dev);

                        mddev->rdev[value].simulate_rderror = 1;
                }
		else
		if (!strcmp("wrerror", name)) {
			dev_t array_dev = MKDEV(MAJOR_NR,0);
                        mddev_t *mddev = dev_to_mddev(array_dev);

                        mddev->rdev[value].simulate_wrerror = 1;
                }
		else
			result = -EINVAL; /* fail */
	}
	else
	if (!strcmp("import", token)) {
		/* slot name size erased id
		 */
		dev_t array_dev = MKDEV(MAJOR_NR,0);
		long slot = 0;
                char name[32] = {'\0'};
		unsigned long offset = 0;
		unsigned long long size = 0;
		long erased = 0;
		char id[MD_ID_SIZE] = {'\0'};
		
		if ((token = get_token(&bufp, delim)) != NULL)
			slot = simple_strtol(token, &temp, 10);

		if (slot >= MD_SB_DISKS)
			return -1;

		if ((token = get_token(&bufp, delim)) != NULL)
			strncpy(name, token, sizeof(name)-1);

		if ((token = get_token(&bufp, delim)) != NULL)
			offset = simple_strtoul(token, &temp, 10);

		if ((token = get_token(&bufp, delim)) != NULL)
			size = simple_strtoull(token, &temp, 10);

		if ((token = get_token(&bufp, delim)) != NULL)
			erased = simple_strtol(token, &temp, 10);

		if ((token = get_token(&bufp, delim)) != NULL)
			strncpy(id, token, sizeof(id)-1);
		
		result = import_slot(array_dev, slot, name, offset,size, erased, id);
	}
	else
	if (!strcmp("start", token)) {
		dev_t array_dev = MKDEV(MAJOR_NR,0);
		char *state = "STOPPED";
		
		if ((token = get_token(&bufp, delim)) != NULL)
			state = token;
		
		result = start_array(array_dev, state);
	}
	else
	if (!strcmp("stop", token)) {
		dev_t array_dev = MKDEV(MAJOR_NR,0);
		
		result = stop_array(array_dev, 0);
	}
	else
	if (!strcmp("check", token)){
		dev_t array_dev = MKDEV(MAJOR_NR,0);
		char *option = "CORRECT";
		unsigned long long offset = 0;

		if ((token = get_token(&bufp, delim)) != NULL) {
			option = token;
                        if ((token = get_token(&bufp, delim)) != NULL) {
                                offset = simple_strtoull(token, &temp, 10);
                        }
                }
		
		result = check_array(array_dev, option, offset);
	}
	else
	if (!strcmp("nocheck", token)){
		dev_t array_dev = MKDEV(MAJOR_NR,0);
		char *option = "CANCEL";

		if ((token = get_token(&bufp, delim)) != NULL)
			option = token;
		
		result = nocheck_array(array_dev, option);
	}
	else
	if (!strcmp("label", token)){
		dev_t array_dev = MKDEV(MAJOR_NR,0);
		char *label = "";
		
		if ((token = get_token(&bufp, delim)) != NULL)
			label = token;
		
		result = label_array(array_dev, label);
	}
	else
	if (!strcmp("dump", token)) {
		dev_t array_dev = MKDEV(MAJOR_NR,0);
		
		result = dump_array(array_dev);
	}
	else
		result = -EINVAL; /* fail */
	
	return (result ? (ssize_t)result : count);
}

static const struct proc_ops md_proc_cmd_fops = {
	.proc_write = md_proc_write,
};

/* Basically the way seq works is this: on open, a PAGE_SIZE buffer is allocated and then our
 * md_seq_show() function is called.  As md_seq_show() calls seq_printf() to fill the buffer,
 * seq keeps track of how much data has gone into it.  If the buffer overflows, seq_printf()
 * will start returning -1.  Eventually our md_seq_show() function exits, seq sees the overflow,
 * and in this case reallocates a new buffer twice as large as before and repeats the call to
 * md_seq_show().  If no overflow this time, data is ready to be returned to user space.
 * The implication is that our md_seq_show() function can be called multiple times in one open
 * (typically is 2 times).
 */
static int md_seq_show(struct seq_file *seq, void *v)
{
	dev_t array_dev = MKDEV(MAJOR_NR,0);

        return md_status(seq, array_dev);
}

static int md_seq_open(struct inode *inode, struct file *file)
{
	return single_open(file, md_seq_show, NULL);
}

static int md_seq_release(struct inode *inode, struct file *file)
{
        return single_release(inode, file);
}

static const struct proc_ops md_proc_stat_fops = {
	.proc_open = md_seq_open,
        .proc_read = seq_read,
        .proc_lseek = seq_lseek,
	.proc_release = md_seq_release,
};

/****************************************************************************/
/* Driver install/remove */

static int md_notify_reboot(struct notifier_block *this,
			    unsigned long code, void *x)
{
        printk("md: md_notify_reboot\n");
	if ((code == SYS_DOWN) ||
	    (code == SYS_HALT) ||
	    (code == SYS_POWER_OFF)) {

		printk("md: stopping all md devices\n");
		stop_array(MKDEV(MAJOR_NR,0), 1);
	}
	return NOTIFY_DONE;
}

static struct notifier_block md_notifier = {
	.notifier_call  = md_notify_reboot,
	.next           = NULL,
	.priority       = INT_MAX, /* before any real devices */
};

static int __init md_init(void)
{
	dev_t array_dev = MKDEV(MAJOR_NR,0);

	printk("md: unRAID driver %d.%d.%d installed\n",
	       MD_MAJOR_VERSION, MD_MINOR_VERSION,
	       MD_PATCHLEVEL_VERSION);

	if (register_blkdev(MAJOR_NR, "nmd")) {
		printk("nmd: unable to get major %d for md\n", MAJOR_NR);
		return (-1);
	}

	if ((md_wq = alloc_workqueue("md", WQ_MEM_RECLAIM, 0)) == NULL) {
		printk("md: unable to alloc_workqueue for md\n");
		return -ENOMEM;
	}

	register_reboot_notifier(&md_notifier);
        proc_create("nmdcmd", S_IRUGO|S_IWUSR, NULL, &md_proc_cmd_fops);
        proc_create("nmdstat", S_IRUGO|S_IWUSR, NULL, &md_proc_stat_fops);
	return (alloc_mddev(array_dev) ? 0 : -1);
}

static __exit void md_exit(void)
{
	free_mddev(dev_to_mddev(MKDEV(MAJOR_NR,0)));
	remove_proc_entry("nmdcmd", NULL);
	remove_proc_entry("nmdstat", NULL);
	unregister_reboot_notifier(&md_notifier);
	destroy_workqueue(md_wq);
	unregister_blkdev(MAJOR_NR, "nmd");

	printk("md: unRAID driver removed\n");
}

module_init(md_init);
module_exit(md_exit);
		
/****************************************************************************/

MODULE_ALIAS("nonraid");
MODULE_ALIAS_BLOCKDEV_MAJOR(MAJOR_NR);
MODULE_LICENSE("GPL");
