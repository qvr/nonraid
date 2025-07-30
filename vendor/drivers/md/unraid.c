/*
 * unraid.c : UnRaid management functions.
 *         Copyright (C) 2006-2019, Tom Mortensen <tomm@lime-technology.com>
 *         Copyright (C) 2016, Eric Schultz <erics@lime-technology.com>
 *
 * Derived from:
 * 
 * raid5.c : Multiple Devices driver for Linux
 *	   Copyright (C) 1996, 1997 Ingo Molnar, Miguel de Icaza, Gadi Oxman
 *	   Copyright (C) 1999, 2000 Ingo Molnar
 *
 * unRAID management functions.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * You should have received a copy of the GNU General Public License
 * (for example /usr/src/linux/COPYING); if not, write to the Free
 * Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "md_private.h"

/*
 * The following can be used to debug the driver
 */
extern int md_trace;
#define dprintk(x...) ((void)((md_trace >= 4) && printk(x)))

#define UNRAID_PARANOIA	1
#if UNRAID_PARANOIA && defined(CONFIG_SMP)
# define CHECK_DEVLOCK() assert_spin_locked(&conf->device_lock)
#else
# define CHECK_DEVLOCK()
#endif

/*
 * Each stripe contains one buffer per disk.  Each buffer can be in
 * one of a number of states determined by col->state.  Changes between
 * these states happen exclusively under a per-stripe spinlock.
 *
 * The bits that are used to represent these states are:
 *   MD_BUFF_UPTODATE, MD_BUFF_LOCKED
 *
 * State Empty == !Uptodate, !Locked
 *        We have no data, and there is no active request
 * State Want == !Uptodate, Locked
 *        A read request is being submitted for this block
 * State Dirty == Uptodate, Locked
 *        Some new data is in this buffer, and it is being written out
 * State Clean == Uptodate, !Locked
 *        We have valid data which is the same as on disc
 *
 * The possible state transitions are:
 *
 *  Empty -> Want   - on read or write to get old data for parity calc
 *  Empty -> Dirty  - on compute_parity to satisfy write/sync request (RECONSTRUCT_WRITE)
 *  Empty -> Clean  - on compute_block when computing a block for failed drive
 *  Want  -> Empty  - on failed read
 *  Want  -> Clean  - on successful completion of read request
 *  Dirty -> Clean  - on successful completion of write request
 *  Dirty -> Clean  - on failed write
 *  Clean -> Dirty  - on compute_parity to satisfy write/sync (RECONSTRUCT or RMW)
 *
 * There is one possibility that is not covered by these states.  That
 * is if one drive has failed and there is a spare being rebuilt.  We
 * can't distinguish between a clean block that has been generated
 * from parity calculations, and a clean block that has been
 * successfully written to the spare (or to parity when resyncing).
 * To distingush these states we have a stripe bit STRIPE_INSYNC that
 * is set whenever a write is scheduled to the spare, or to the parity
 * disc if there is no spare.  A sync request clears this bit, and
 * when we find it set with no buffers locked, we know the sync is
 * complete.
 *
 * Buffers for the md device that arrive via make_request are attached
 * to the appropriate stripe in one of two lists linked on b_reqnext.
 * One list (read_bi) for read requests, one (write_bi) for write.
 * There should never be more than one buffer on the two lists
 * together, but we are not guaranteed of that so we allow this.
 *
 * If a bio is on the read list when the associated cache buffer is
 * Uptodate, the data is copied into the read buffer and it's b_end_io
 * routine is called.
 *
 * When a bio on the write list is committed for write, write buffer is copied
 * into the cache buffer, which is then marked dirty, and moved onto a
 * third list, the written list (written_bi).  Once both the parity
 * block and the cached buffer are successfully written, any buffer on
 * a written list can be returned with b_end_io.
 *
 * The write list and read list both act as fifos.  The read list is
 * protected by the device_lock.  The write and written lists are
 * protected by the stripe lock.  The device_lock, which can be
 * claimed while the stripe lock is held, is only for list
 * manipulations and will only be held for a very short time.  It can
 * be claimed from interrupts.
 *
 *
 * Stripes in the stripe cache can be on one of two lists, or on
 * neither list.  The "inactive_list" contains stripes which are not
 * currently being used for any request.  They can freely be reused
 * for another stripe.  The "handle_list" contains stripes that need
 * to be handled in some way.  Both of these are fifo queues.  Each
 * stripe is also (potentially) linked to a hash bucket in the hash
 * table so that it can be found by sector number.  Stripes that are
 * not hashed must be on the inactive_list, and will normally be at
 * the front.  All stripes start life this way.
 *
 * The inactive_list, handle_list and hash bucket lists are all protected by the
 * device_lock.
 *  - stripes have a reference counter. If count==0, they are on a list.
 *  - If a stripe might need handling, STRIPE_HANDLE is set.
 *  - When refcount reaches zero, then if STRIPE_HANDLE it is put on
 *    handle_list else inactive_list
 *
 * This, combined with the fact that STRIPE_HANDLE is only ever
 * cleared when a stripe is taken out of the handle_list, means that if the
 * refcount is 0 and STRIPE_HANDLE is set, then it is on the
 * handle_list and if recount is 0 and STRIPE_HANDLE is not set, then
 * the stripe is on inactive_list.
 *
 * The possible transitions are:
 *  activate an unhashed/inactive stripe (get_active_stripe())
 *     lockdev check-hash unlink-stripe cnt++ clean-stripe hash-stripe unlockdev
 *  activate a hashed, possibly active stripe (get_active_stripe())
 *     lockdev check-hash if(!cnt++)unlink-stripe unlockdev
 *  attach a request to an active stripe (add_stripe_bio())
 *     lockstripe attach-buffer lockdev biocnt++ unlockdev unlockstripe
 *  handle a stripe (handle_stripe())
 *     lockstripe ... (lockdev biocnt-- unlockdev) .. change-state .. record io needed unlockstripe schedule io
 *  release an active stripe (release_stripe())
 *     lockdev if (!--cnt) { if  STRIPE_HANDLE, add to handle_list else add to inactive-list } unlockdev
 *
 * The refcount counts each thread that have activated the stripe,
 * plus unraidd if it is handling it, plus one for each active request
 * on a cached buffer.
 * 
 * Since a single bio could span multiple stripes, we keep a count of how many stripes of the bio
 * remain to be completed.  When this count reaches zero, we can return the bio.
  */

/* These are tunables defined in md.c */
extern int md_num_stripes;      /* number of stripes to allocate */
extern int md_write_method;     /* default write algorithm, refer to md_private.h */
extern int md_queue_limit;      /* max queue depth ceiling as percentage [1..100] for I/O */
extern int md_sync_limit;       /* max queue depth ceiling as percentage [1..100] for resync */
extern int md_restrict;

/* Buffer size in bytes */
#define BUFFER_SIZE             PAGE_SIZE

/* Buffer size in sectors (i.e., number of 512-byte sectors per buffer) */
#define BUFFER_SECT             (1ULL << (PAGE_SHIFT-9))

/* Starting sector of stripe */
#define STRIPE_SECTOR(sect)     (sect & ~((sector_t)BUFFER_SECT - 1))

/* Stripe cache  */
#define NR_HASH			(PAGE_SIZE / sizeof(struct hlist_head))
#define HASH_MASK		(NR_HASH - 1)
#define stripe_hash(conf, sect)	(&((conf)->stripe_hashtbl[((sect) / BUFFER_SECT) & HASH_MASK]))

/* Stripe state bits */
#define STRIPE_HANDLE           0
#define	STRIPE_SYNCING		1
#define	STRIPE_CLEARING		2
#define	STRIPE_INSYNC		3

/* additional disk state bits */
#define MD_BUFF_UPTODATE        8 /* buffer is uptodate */
#define MD_BUFF_LOCKED          9 /* buffer is locked */
#define MD_BUFF_READ           10 /* want to read this buffer */
#define MD_BUFF_WRITE          11 /* want to write this buffer */

#define MD_UPDATE_SB           12 /* set if superblock config change */

#define buff_uptodate(d)        ((d)->state &   (1 << MD_BUFF_UPTODATE))
#define set_buff_uptodate(d)    ((d)->state |=  (1 << MD_BUFF_UPTODATE))
#define clr_buff_uptodate(d)    ((d)->state &= ~(1 << MD_BUFF_UPTODATE))

#define buff_locked(d)          ((d)->state &   (1 << MD_BUFF_LOCKED))
#define set_buff_locked(d)      ((d)->state |=  (1 << MD_BUFF_LOCKED))
#define clr_buff_locked(d)      ((d)->state &= ~(1 << MD_BUFF_LOCKED))

typedef struct column_s {
	unsigned long           state;                  /* state flags */

	struct bio	        *read_bi;	        /* read request buffers of the MD device */
	struct bio	        *write_bi;	        /* write request buffers of the MD device */
	struct bio	        *written_bi;            /* write request buffers of the MD device that have been scheduled for write */

	struct bio	        bio;	                /* bio for cache buffer i/o */
	struct bio_vec          vec;
	struct page             *page;                  /* buffer page */
} column_t;

struct stripe_head {
	struct hlist_node       hash;
	struct list_head	lru;			/* inactive_list or handle_list */
	struct unraid_conf	*conf;

        int                     unit;
	sector_t                sector;			/* sector of this row */
	unsigned long		state;			/* stripe state flags */
	atomic_t		count;			/* nr of active thread/requests */
        int                     write_method;

        void                    *srcs[MD_SB_DISKS];     /* buffer addresses */
	column_t                col[0];                 /* column information */
};

typedef struct unraid_conf {
	struct hlist_head	*stripe_hashtbl;

	mddev_t			*mddev;
	int			disks;                  /* number of array disks */
        mdp_disk_t              *disk[MD_SB_DISKS];
        mdk_rdev_t              *rdev[MD_SB_DISKS];

        void                    *p_scribble;            /* these two buffers are */
        void                    *q_scribble;            /*  only used by check_parity() */

	struct kmem_cache       *slab_cache;
	int                     num_stripes;

	mdk_thread_t            *thread[MD_SB_DISKS-1];
	struct list_head	handle_list[MD_SB_DISKS-1]; /* stripes needing handling */

	struct list_head	inactive_list;

	wait_queue_head_t	wait_for_stripe;
	atomic_t                active_flushes;
	atomic_t		active_stripes[MD_SB_DISKS-1];

	spinlock_t		device_lock;
} unraid_conf_t;

#define mddev_to_conf(mddev) ((unraid_conf_t *) mddev->private)

typedef struct flush_stripe {
	mddev_t                 *mddev;
	int                     unit;
	struct bio              *bi;
	atomic_t                flush_pending;
	struct work_struct      flush_work;
	struct bio              flush_bio_D;
	struct bio              flush_bio_P;
	struct bio              flush_bio_Q;
} flush_stripe_t;

static inline void remove_hash(struct stripe_head *sh)
{
	dprintk("remove_hash(), stripe %llu\n", (unsigned long long)sh->sector);
	hlist_del_init(&sh->hash);
}

static inline void insert_hash(unraid_conf_t *conf, struct stripe_head *sh)
{
	struct hlist_head *hp = stripe_hash(conf, sh->sector);

	dprintk("insert_hash(), stripe %llu\n", (unsigned long long)sh->sector);
	hlist_add_head(&sh->hash, hp);
}

static struct stripe_head *find_stripe(unraid_conf_t *conf, sector_t sector)
{
	struct stripe_head *sh;

	CHECK_DEVLOCK();

	dprintk("find_stripe, sector %llu\n", (unsigned long long)sector);
	hlist_for_each_entry(sh, stripe_hash(conf, sector), hash)
		if (sh->sector == sector)
			return sh;

	dprintk("stripe %llu not in cache\n", (unsigned long long)sector);
	return NULL;
}

/* Find an idle stripe, and unhash it.
 */
static struct stripe_head *get_free_stripe(unraid_conf_t *conf)
{
	struct stripe_head *sh = NULL;

	CHECK_DEVLOCK();

	if (!list_empty(&conf->inactive_list)) {
		struct list_head *first;

		first = conf->inactive_list.next;
		sh = list_entry(first, struct stripe_head, lru);
		list_del_init(first);
		remove_hash(sh);
	}

	return sh;
}

/* Initialize a free stripe, and hash it.
 */
static void init_stripe(struct stripe_head *sh, sector_t sector)
{
	unraid_conf_t *conf = sh->conf;
        int i;

	CHECK_DEVLOCK();

	sh->sector = sector;

	for (i = 0; i < conf->disks; i++) {
                column_t *col = &sh->col[i];
                mdp_disk_t *disk = conf->disk[i];

                col->state = disk->state;

                if (disk_active(col) || disk_enabled(col)) {
                        if (sector >= (disk->size *2))
                                mark_disk_disabled(col);
                }
                if (!disk_active(col)) {
                        mark_disk_valid(col);
                        set_buff_uptodate(col);
                }
        }

	insert_hash(conf, sh);

	dprintk("init_stripe: stripe %llu\n", (unsigned long long)sh->sector);
}

/* Sanity checks on alloocated stripe.
 */
static void sanity_check(struct stripe_head *sh)
{
	unraid_conf_t *conf = sh->conf;
        int i;

        if (sh->state != 0) {
                dprintk("sector=%llu state=%lu\n",
                        (unsigned long long)sh->sector, sh->state);
                BUG();
        }
        for (i = 0; i < conf->disks; i++) {
                column_t *col = &sh->col[i];
                if (col->read_bi || col->write_bi || col->written_bi || buff_locked(col)) {
                        dprintk("sector=%llu i=%d %llu %llu %llu %lu\n",
                                (unsigned long long)sh->sector, i,
                                (unsigned long long)col->read_bi,
                                (unsigned long long)col->write_bi,
                                (unsigned long long)col->written_bi,
                                buff_locked(col));
                        BUG();
                }
        }
}

static int stripe_limit( unraid_conf_t *conf, int unit, int *activeP)
{
        int active = 0;
        int queue_limit, i;

        for (i = 0; i <= conf->disks-2; i++) {
                if ((i == unit) || atomic_read(&conf->active_stripes[i]))
                        active++;
	}
        *activeP = active;

        /* throttle back resync process when other I/O is active */
        queue_limit = ((unit == 0) && (active > 1)) ? md_sync_limit : md_queue_limit;

        return ((conf->num_stripes * queue_limit) + (active * 100)/2) / (active * 100);
}

/* If requests are coming in at a faster rate than they are being completed, then eventually a thread
 * call to get_free_stripe() will return NULL, indicating no free stripes are available.  So the thread
 * will enqueue itself onto the conf->wait_for_stripe queue.
 * Meanwhile, as each I/O completes, release_stripe() frees the stripe and wakes up all waiting threads.
 */
static int _get_active_stripe(unraid_conf_t *conf, int unit, sector_t sector, int noblock, struct stripe_head **shP)
{
	struct stripe_head *sh = NULL;
        int active;

	CHECK_DEVLOCK();

        if (atomic_read(&conf->active_stripes[unit]) < stripe_limit(conf, unit, &active)) {
                sh = find_stripe(conf, sector);
                if (!sh) {
                        sh = get_free_stripe(conf);
                        if (sh) {
                                init_stripe(sh, sector);
                        }
                }
                else {
                        if (atomic_read(&sh->count)) {
                                /* stripe has pending i/o, so should not be on any list */
                                BUG_ON(!list_empty(&sh->lru));
                                sh = NULL;
                        }
                        else {
                                /* no i/o pending, so either in handle list or inactive list */
                                BUG_ON(list_empty(&sh->lru));
                                if (test_bit(STRIPE_HANDLE, &sh->state)) {
                                        /* still in handle list */
                                        sh = NULL;
                                }
                                else {
                                        /* remove it from inactive list */
                                        list_del_init(&sh->lru);
                                }
                        }
                }
	}
	if (sh) {
                sanity_check(sh);
                sh->unit = unit;
                /* force read-modify-write if more than one active stream */
                sh->write_method = (active == 1) ? md_write_method : READ_MODIFY_WRITE;
                /* stripe is now active */
		atomic_inc(&sh->count);
                atomic_inc(&conf->active_stripes[unit]);
        }
        *shP = sh;
        return (sh || noblock);
}

static struct stripe_head *get_active_stripe(unraid_conf_t *conf, int unit, sector_t sector, int noblock)
{
	struct stripe_head *sh;

	dprintk("get_stripe, unit %i sector %llu\n", unit, (unsigned long long)sector);

	spin_lock_irq(&conf->device_lock);
        wait_event_lock_irq(conf->wait_for_stripe,
                            _get_active_stripe( conf, unit, sector, noblock, &sh),
                            conf->device_lock);
	spin_unlock_irq(&conf->device_lock);

	return sh;
}

static void add_stripe_bio(struct stripe_head *sh, struct bio *bi)
{
	column_t *col = &sh->col[sh->unit-1];

	if (bio_data_dir(bi) == READ)
		col->read_bi = bi;
	else
		col->write_bi = bi;

        bio_inc_remaining(bi); /* this is atomic */

	dprintk("added bio b#%llu to stripe s#%llu, col %d\n",
		(unsigned long long)bi->bi_iter.bi_sector, (unsigned long long)sh->sector, sh->unit);
}

static int partial_write(struct stripe_head *sh, column_t *col)
{
        struct bio *bi = col->write_bi;

        return (bi->bi_iter.bi_size &&
                ((bi->bi_iter.bi_sector > sh->sector) ||
                 (bi->bi_iter.bi_sector+(bi->bi_iter.bi_size>>9) < sh->sector+(BUFFER_SECT))));
}

static void _release_stripe(unraid_conf_t *conf, struct stripe_head *sh)
{
	CHECK_DEVLOCK();
	BUG_ON(atomic_read(&sh->count) <= 0);
	
	if (atomic_dec_and_test(&sh->count)) {
		/* sh->count is now zero */
		BUG_ON(!list_empty(&sh->lru));

		if (test_bit(STRIPE_HANDLE, &sh->state)) {
                        list_add_tail(&sh->lru, &conf->handle_list[sh->unit]);
                        md_wakeup_thread(conf->thread[sh->unit]);
		}
		else {
			atomic_dec(&conf->active_stripes[sh->unit]);

			list_add_tail(&sh->lru, &conf->inactive_list);
			wake_up_all(&conf->wait_for_stripe);

			dprintk("stripe %llu, released\n", (unsigned long long)sh->sector);
		}
	}
}

static void release_stripe(struct stripe_head *sh)
{
	unraid_conf_t *conf = sh->conf;
	
	spin_lock_irq(&conf->device_lock);
	_release_stripe(conf, sh);
	spin_unlock_irq(&conf->device_lock);
}

/* Interrupt handler.
 */
static void end_request(struct bio *bi)
{
 	struct stripe_head *sh = bi->bi_private;
	unraid_conf_t *conf = sh->conf;

	column_t *col = NULL;
	int disks = conf->disks, i, uptodate;

	unsigned long flags;

	/* find the column this bio is for */
	for (i = 0; i < disks; i++) {
		col = &sh->col[i];
		if (bi == &col->bio)
			break;
	}
        if (i == disks) {
                BUG();
                return;
        }
        BUG_ON(!buff_locked(col));

        uptodate = !bi->bi_status;
        if (bio_data_dir(bi) == READ) {
                if (conf->rdev[i]->simulate_rderror) {
                        conf->rdev[i]->simulate_rderror = 0;
                        uptodate = 0;
                }
                if (uptodate) {
                        set_buff_uptodate(col);
                }
                else {
                        mdp_disk_t *disk = conf->disk[i];
                        spin_lock_irqsave(&conf->device_lock, flags);
                        md_read_error(conf->mddev, disk->number, sh->sector);
                        spin_unlock_irqrestore(&conf->device_lock, flags);
                        mark_disk_invalid(col);
                }
        }
        else {
                if (conf->rdev[i]->simulate_wrerror) {
                        conf->rdev[i]->simulate_wrerror = 0;
                        uptodate = 0;
                }
                if (uptodate) {
                        mark_disk_valid(col);
                }
                else {
                        mdp_disk_t *disk = conf->disk[i];
                        spin_lock_irqsave(&conf->device_lock, flags);
                        if (md_write_error(conf->mddev, disk->number, sh->sector))
                                set_bit(MD_UPDATE_SB, &col->state);
                        spin_unlock_irqrestore(&conf->device_lock, flags);
                        mark_disk_disabled(col);
                        if (disk_active(col))
                                mark_disk_invalid(col);
                }
        }
        bio_uninit(bi);

        clr_buff_locked(col);
	spin_lock_irqsave(&conf->device_lock, flags);
        set_bit(STRIPE_HANDLE, &sh->state);
	_release_stripe(conf, sh);
	spin_unlock_irqrestore(&conf->device_lock, flags);
}

/* Copy data between a page in the stripe cache, and one or more bion.
 * The page could align with the middle of the bio, or there could be
 * several bio_vecs, which cover part of the page
 */
static void copy_data(int frombio, struct bio *bio, char *pa, sector_t sector)
{
	struct bio_vec bvl;
        struct bvec_iter iter;
	int page_offset;

	if (bio->bi_iter.bi_sector >= sector)
		page_offset = (signed)(bio->bi_iter.bi_sector - sector) * 512;
	else
		page_offset = (signed)(sector - bio->bi_iter.bi_sector) * -512;

	bio_for_each_segment(bvl, bio, iter) {
		int len = bvl.bv_len;
		int clen;
		int b_offset = 0;

		if (page_offset < 0) {
			b_offset = -page_offset;
			page_offset += b_offset;
			len -= b_offset;
		}

		if (len > 0 && page_offset + len > BUFFER_SIZE)
			clen = BUFFER_SIZE - page_offset;
		else
                        clen = len;

		if (clen > 0) {
			char *ba = kmap_atomic(bvl.bv_page) + bvl.bv_offset;

			if (frombio)
				memcpy(pa+page_offset, ba+b_offset, clen);
			else
				memcpy(ba+b_offset, pa+page_offset, clen);
			kunmap_atomic(ba);
		}
		if (clen < len) /* hit end of page */
			break;
		page_offset += len;
	}
}

static void check_srcs(struct stripe_head *sh, int faila, int failb)
{
        int i;

        for (i = 0; i < sh->conf->disks; i++) {
		column_t *col = &sh->col[i];

                if (i != faila && i != failb)
                        BUG_ON(!buff_uptodate(col));
        }
}

static void raid6_generate_dd(struct stripe_head *sh, int faila, int failb)
{
        int disks=sh->conf->disks;

	dprintk("raid6_generate_dd, stripe %llu\n",
		(unsigned long long)sh->sector);

        check_srcs(sh, faila, failb);
        raid6_2data_recov(disks, BUFFER_SIZE, faila, failb, sh->srcs);
        set_buff_uptodate(&sh->col[faila]);
        set_buff_locked(&sh->col[faila]);
        set_buff_uptodate(&sh->col[failb]);
        set_buff_locked(&sh->col[failb]);
}

static void raid6_generate_dp(struct stripe_head *sh, int faila)
{
        int disks=sh->conf->disks, pd_idx=disks-2, qd_idx=disks-1;

	dprintk("raid6_generate_dp, stripe %llu\n",
		(unsigned long long)sh->sector);

        check_srcs(sh, faila, pd_idx);
	if (disks == 3) {
		BUG_ON(faila != 0);
                memcpy(sh->srcs[0], sh->srcs[qd_idx], BUFFER_SIZE);
                memcpy(sh->srcs[pd_idx], sh->srcs[qd_idx], BUFFER_SIZE);
        }
        else {
                raid6_datap_recov(disks, BUFFER_SIZE, faila, sh->srcs);
        }
        set_buff_uptodate(&sh->col[faila]);
        set_buff_locked(&sh->col[faila]);
        set_buff_uptodate(&sh->col[pd_idx]);
        set_buff_locked(&sh->col[pd_idx]);
}
                
static void raid6_generate_pq(struct stripe_head *sh)
{
        int disks=sh->conf->disks, pd_idx=disks-2, qd_idx=disks-1;

	dprintk("raid6_generate_pq, stripe %llu\n",
		(unsigned long long)sh->sector);

        check_srcs(sh, pd_idx, qd_idx);
        if (disks == 3) {
                memcpy(sh->srcs[pd_idx], sh->srcs[0], BUFFER_SIZE);
                memcpy(sh->srcs[qd_idx], sh->srcs[0], BUFFER_SIZE);
        }
        else {
                raid6_gen_syndrome(disks, BUFFER_SIZE, sh->srcs);
        }
        set_buff_uptodate(&sh->col[pd_idx]);
        set_buff_locked(&sh->col[pd_idx]);
        set_buff_uptodate(&sh->col[qd_idx]);
        set_buff_locked(&sh->col[qd_idx]);
}

/* Note: there is no raid6 function that just generates Q from D - the one we have to
 * use, raid6_gen_syndrome(), generates both P and Q.  We'll go ahead and set P buffer
 * uptodate, but we don't need it scheduled for write, so we don't lock it.
 */
static void raid6_generate_q(struct stripe_head *sh)
{
        int disks=sh->conf->disks, pd_idx=disks-2, qd_idx=disks-1;

	dprintk("raid6_generate_q, stripe %llu\n",
		(unsigned long long)sh->sector);

        check_srcs(sh, pd_idx, qd_idx);
	if (disks == 3) {
                memcpy(sh->srcs[qd_idx], sh->srcs[0], BUFFER_SIZE);
        }
        else {
                raid6_gen_syndrome(disks, BUFFER_SIZE, sh->srcs);
                set_buff_uptodate(&sh->col[pd_idx]);
        }
        set_buff_uptodate(&sh->col[qd_idx]);
        set_buff_locked(&sh->col[qd_idx]);
}

#define check_xor()   do { 						\
                         if (count == MAX_XOR_BLOCKS) {		        \
                            xor_blocks(count, BUFFER_SIZE, dest, ptr);	\
                            count = 0;				        \
                         }						\
                      } while(0)

static void raid5_generate_d(struct stripe_head *sh, int dd_idx)
{
	int disks=sh->conf->disks, pd_idx=disks-2, count, i;
	void *dest, *ptr[MAX_XOR_BLOCKS];

	dprintk("raid5_generate_d, stripe %llu, idx %d\n", 
		(unsigned long long)sh->sector, dd_idx);

	dest = sh->srcs[dd_idx];
	count = 0;

	memset(dest, 0, BUFFER_SIZE);

	for (i = 0; i <= pd_idx; i++) {
		column_t *col = &sh->col[i];

		if (i == dd_idx)
			continue;

		BUG_ON(!buff_uptodate(col));

                /* no point xor'ing buffer full of zeros */
                if (sh->srcs[i] == (void *)raid6_empty_zero_page)
                        continue;

		ptr[count++] = sh->srcs[i];
		check_xor();
	}
	if (count)
		xor_blocks(count, BUFFER_SIZE, dest, ptr);

	set_buff_uptodate(&sh->col[dd_idx]);
        set_buff_locked(&sh->col[dd_idx]);
}

static void raid5_generate_p(struct stripe_head *sh)
{
        int disks=sh->conf->disks, pd_idx=disks-2;

        raid5_generate_d(sh, pd_idx);
}

static void copy_write_data(struct stripe_head *sh)
{
	int disks=sh->conf->disks, pd_idx=disks-2, i;

        /* insert the "new data" */
        /* and record in col->written_bi which disks have "new data" */
        for (i = 0; i < pd_idx; i++) {
                column_t *col = &sh->col[i];

                if (col->write_bi) {
                        copy_data(1, col->write_bi, sh->srcs[i], sh->sector);
                        set_buff_uptodate(col);
                        set_buff_locked(col);

                        BUG_ON(col->written_bi);
                        col->written_bi = col->write_bi;
                        col->write_bi = NULL;
                        break;
                }
        }
}

void rmw5_write_data(struct stripe_head *sh)
{
	int disks=sh->conf->disks, pd_idx=disks-2, count, i;
	void *dest, *ptr[MAX_XOR_BLOCKS];

	dprintk("rmw5_write_data, stripe %llu\n",
		(unsigned long long)sh->sector);

        BUG_ON(!buff_uptodate(&sh->col[pd_idx]));
	dest = sh->srcs[pd_idx];
	count = 0;

        /* subtract 'old' data */
        for (i = 0; i < pd_idx; i++) {
                column_t *col = &sh->col[i];

                if (col->write_bi) {
                        BUG_ON(!buff_uptodate(col));
                        ptr[count++] = sh->srcs[i];
                        check_xor();
                        break;
                }
        }
        if (count) {
                xor_blocks(count, BUFFER_SIZE, dest, ptr);
                count = 0;
        }

        /* insert and add 'new' data */
        for (i = 0; i < pd_idx; i++) {
                column_t *col = &sh->col[i];

                if (col->write_bi) {
                        copy_data(1, col->write_bi, sh->srcs[i], sh->sector);
                        set_buff_locked(&sh->col[i]);

                        ptr[count++] = sh->srcs[i];
                        check_xor();

                        BUG_ON(col->written_bi);
                        col->written_bi = col->write_bi;
                        col->write_bi = NULL;
                        break;
                }
        }
        if (count) {
                xor_blocks(count, BUFFER_SIZE, dest, ptr);
                count = 0;
        }

        set_buff_locked(&sh->col[pd_idx]);
}

/* xor (add/subtract) write data with P and Q.
 * raid6_xor_syndrome() is designed to xor a range of columns with P and Q, but
 * we can't guarantee a contigous range so we call once for each buffer. This is ok
 * because it will be rare for there to be more than one data column involved.
 * Input pd_uptodate indicates whether P should be involved.
 */
static void rmw6_write_data(struct stripe_head *sh, int pd_uptodate)
{
	int disks=sh->conf->disks, pd_idx=disks-2, qd_idx=disks-1, i;

	dprintk("rmw6_write_data, stripe %llu, pd_uptodate %d\n",
		(unsigned long long)sh->sector, pd_uptodate);

        if (pd_uptodate)
                BUG_ON(!buff_uptodate(&sh->col[pd_idx]));
	BUG_ON(!buff_uptodate(&sh->col[qd_idx]));

        for (i = 0; i < pd_idx; i++) {
                column_t *col = &sh->col[i];

                if (col->write_bi) {
                        BUG_ON(!buff_uptodate(col));

                        /* subtract 'old' data from P/Q */
                        raid6_xor_syndrome(disks, i, i, BUFFER_SIZE, sh->srcs);

                        /* copy 'new' data */
                        copy_data(1, col->write_bi, sh->srcs[i], sh->sector);
                        set_buff_locked(col);

                        /* add 'new' data to P/Q */
                        raid6_xor_syndrome(disks, i, i, BUFFER_SIZE, sh->srcs);

                        BUG_ON(col->written_bi);
                        col->written_bi = col->write_bi;
                        col->write_bi = NULL;
                        break;
                }
        }

        if (pd_uptodate)
                set_buff_locked(&sh->col[pd_idx]);
        else
                clr_buff_uptodate(&sh->col[pd_idx]);
        set_buff_locked(&sh->col[qd_idx]);
}

static int check_parity5(struct stripe_head *sh)
{
	int disks=sh->conf->disks, pd_idx=disks-2, count, i;
	void *dest, *ptr[MAX_XOR_BLOCKS];

	dprintk("check_parity5, stripe %llu\n", (unsigned long long)sh->sector);

	BUG_ON(!buff_uptodate(&sh->col[pd_idx]));
	dest = sh->srcs[pd_idx];
	count = 0;

	for (i = 0; i < pd_idx; i++) {
		column_t *col = &sh->col[i];

		BUG_ON(!buff_uptodate(col));

                /* no point xor'ing buffer full of zeros */
                if (sh->srcs[i] == (void *)raid6_empty_zero_page)
                        continue;

		ptr[count++] = sh->srcs[i];
		check_xor();
	}
	if (count) {
		xor_blocks(count, BUFFER_SIZE, dest, ptr);
		count = 0;
	}

	/* checking parity destroys the parity buffer contents */
	/* (hopefully sets it to all zeros) */
	clr_buff_uptodate(&sh->col[pd_idx]);

	return ((*(u64*)dest)==0 &&
		memcmp(dest, dest+8, BUFFER_SIZE-8)==0);
}

/* Handle parity check cases:
 * (fail_idx == -1)     => no disabled disks
 * (fail_idx == dd_idx) => single data disk dd_idx disabled
 * (fail_idx == pd_idx) => P is disabled
 * (fail_idx == qd_idx) => Q is disabled
 *
 * This would be simpler if raid6_xor_syndrome() wasn't so damn slow.
 */
static void check_parity(struct stripe_head *sh, int fail_idx, int recover)
{
	unraid_conf_t *conf = sh->conf;
        int disks=conf->disks, pd_idx=disks-2, qd_idx=disks-1;
        char error[128] = {0};

	dprintk("check_parity, stripe %llu fail_idx %d recover %d\n",
		(unsigned long long)sh->sector, fail_idx, recover);

        check_srcs(sh, fail_idx, -1);
        if (fail_idx == qd_idx) {
                if (!check_parity5(sh)) {
                        strcat(error, "P");
                        if (recover)
                                raid5_generate_p(sh);
                }
        }
        else {
                void *p_save, *q_save;

                if (fail_idx >= 0 && fail_idx < pd_idx) {
                        raid5_generate_d(sh, fail_idx);
                        /* no point checking P */
                        fail_idx = pd_idx;
                }

                /* generate P/Q in temp buffers */
                p_save = sh->srcs[pd_idx];
                q_save = sh->srcs[qd_idx];
                sh->srcs[pd_idx] = conf->p_scribble;
                sh->srcs[qd_idx] = conf->q_scribble;
                raid6_gen_syndrome(disks, BUFFER_SIZE, sh->srcs);
                sh->srcs[pd_idx] = p_save;
                sh->srcs[qd_idx] = q_save;

                if (fail_idx != pd_idx) {
                        if (memcmp(sh->srcs[pd_idx], conf->p_scribble, BUFFER_SIZE) != 0) {
                                strcat(error, "P");
                                if (recover) {
                                        memcpy(sh->srcs[pd_idx], conf->p_scribble, BUFFER_SIZE);
                                        set_buff_locked(&sh->col[pd_idx]);
                                }
                        }
                }
                else
                        set_buff_uptodate(&sh->col[pd_idx]);

                if (memcmp(sh->srcs[qd_idx], conf->q_scribble, BUFFER_SIZE) != 0) {
                        strcat(error, "Q");
                        if (recover) {
                                memcpy(sh->srcs[qd_idx], conf->q_scribble, BUFFER_SIZE);
                                set_buff_locked(&sh->col[qd_idx]);
                        }
                }
        }

        if (strlen(error)) {
                strcat(error, recover ? " corrected" : " incorrect");
                md_sync_error(conf->mddev, sh->sector, error);
        }
}

static int schedule_read(column_t *col, int idx)
{
        int _locked = 0;

        if (disk_enabled(col)) {
                dprintk("Reading col %d\n", idx);
                set_buff_locked(col);
                _locked++;
                set_bit(MD_BUFF_READ, &col->state);
        }
        else {
                dprintk("Read clearing disabled col %d\n", idx);
                memset(page_address(col->page), 0, BUFFER_SIZE);
                set_buff_uptodate(col);
        }

        return _locked;
}

static int schedule_writes(struct stripe_head *sh)
{
	int disks=sh->conf->disks;
        int _locked = 0;
        int i;

        for (i = 0; i < disks; i++) {
                column_t *col = &sh->col[i];

                if (buff_locked(col)) {
                        if (disk_enabled(col)) {
                                dprintk("Writing col %d\n", i);
                                _locked++;
                                set_bit(MD_BUFF_WRITE, &col->state);
                        }
                        else {
                                dprintk("Skip writing disabled col %d\n", i);
                                clr_buff_locked(col);
                                mark_disk_valid(col);
                        }
                }
        }

        return _locked;
}

/*
 * handle_stripe - do things to a stripe.
 *
 * We lock the stripe and then examine the state of various bits
 * to see what needs to be done.
 * Possible results:
 *    return some read request which now have data
 *    return some write requests which are safely on disc
 *    schedule a read on some buffers
 *    schedule a write of some buffers
 *    return confirmation of parity correctness
 *
 * Parity calculations are done inside the stripe lock
 * buffers are taken off read_list or write_list, and bh_cache buffers
 * get BH_Lock set before the stripe lock is released.
 * 
 * NOTE: the order of checking is very important within this function
 * 
 * MORE: should refactor this later
 *
 */
static void handle_stripe(struct stripe_head *sh)
{
	unraid_conf_t *conf = sh->conf;
	int disks=conf->disks, pd_idx=disks-2, qd_idx=disks-1, i;
	int locked=0, to_read=0, to_write=0, written=0;
	int failed=0, faila=-1, failb=-1;
        int update_sb=0;
        int pq_flags = 0;
	struct bio *return_bi=NULL;

	dprintk("handling stripe %llu, cnt=%d\n", (unsigned long long)sh->sector, atomic_read(&sh->count));

	/* Capture the state of the stripe.
	 */
	for (i = 0; i < disks; i++) {
		column_t *col = &sh->col[i];

		dprintk("check col %d: state 0x%lx read %llu write %llu written %llu\n",
                        i, col->state,
                        (unsigned long long)col->read_bi,
                        (unsigned long long)col->write_bi,
                        (unsigned long long)col->written_bi);

                /* check for config change requiring superblock update */
                if (test_and_clear_bit(MD_UPDATE_SB, &col->state))
                        update_sb++;

		if (buff_locked(col)) locked++;

		if (col->read_bi) to_read++;
		if (col->write_bi) to_write++;
		if (col->written_bi) written++;

		if (!disk_valid(col)) {
			if (++failed == 1)
                                faila = i;
                        else if (failed == 2)
                                failb = i;
		}
	}
        /* check if we need to update the superblock */
        if (update_sb)
                md_update_sb(conf->mddev);

	dprintk("locked=%d to_read=%d to_write=%d written=%d failed=%d faila=%d failb=%d\n",
		locked, to_read, to_write, written, failed, faila, failb);

	/* Check if the array has more than 2 failures, and if so, any request posted to an
	 * invalid drive must be failed.
         * Note: a reconstruct-write could be changed to a straight-write if there are 3 or
         * more failures; however parity would not be updated.  Instead, we choose to fail
         * the write in this corner case.
	 */
	if (failed > 2) {
		for (i = 0; i < pd_idx; i++) {
			column_t *col = &sh->col[i];

			if (!disk_valid(col)) {
				/* fail reads */
				if (col->read_bi) {
					dprintk("Fail read_bi for col %d\n", i);
					return_bi = col->read_bi;
					col->read_bi = NULL;
					return_bi->bi_status = BLK_STS_IOERR;
					to_read--;
                                        break;
				}
				else
				/* fail unstarted writes */
				if (col->write_bi) {
					dprintk("Fail write_bi for col %d\n", i);
					return_bi = col->write_bi;
					col->write_bi = NULL;
					return_bi->bi_status = BLK_STS_IOERR;
					to_write--;
                                        break;
				}
				else
				/* fail writes */
				if (col->written_bi) {
					dprintk("Fail written_bi for col %d\n", i);
					return_bi = col->written_bi;
					col->written_bi = NULL;
					return_bi->bi_status = BLK_STS_IOERR;
					written--;
                                        break;
				}
			}
		}
	}

	/* Now consider reading some blocks to satisfy read requests, and return
	 * completed read requests.
	 */
	if (to_read) {
                int need_recovery = 0;

                /* only check data columns for readers */
                for (i = 0; i < pd_idx; i++) {
                        column_t *col = &sh->col[i];

                        if (col->read_bi) {
                                if (disk_valid(col)) {
                                        if (!buff_uptodate(col) && !buff_locked(col)) {
                                                locked += schedule_read(col, i);
                                        }
                                }
                                else {
                                        need_recovery = 1;
                                }
                                break;
                        }
                }
                if (need_recovery) {
                        /* disk(s) we want to read can't be read; we need to read all enabled disks and reconstruct
                         * note: if failed==1 we don't really need to read Q, but since we have to spin up all
                         * the other disks anyway may as well read Q to spin it up too in case a read of one of
                         * the other disks fails, we'd have to read Q and won't suffer a another spin up delay.
                         */
                        for (i = 0; i < disks; i++) {
                                column_t *col = &sh->col[i];

                                if (disk_valid(col) && 
                                    !buff_uptodate(col) && !buff_locked(col)) {
					locked += schedule_read(col, i);
				}
			}
                        /* if nothing locked, reads are done, reconstruct failed disk(s) */
                        if (locked == 0) {
                                /* note faila has to be for a data disk */
                                if (failed == 1)
                                        raid5_generate_d(sh, faila);
                                else {
                                        if (failb < pd_idx)
                                                raid6_generate_dd(sh, faila, failb);
                                        else if (failb == pd_idx)
                                                raid6_generate_dp(sh, faila);
                                        else if (failb == qd_idx) {
                                                raid5_generate_d(sh, faila);
                                                if (disk_enabled(&sh->col[qd_idx]))
                                                        raid6_generate_q(sh);
                                        }
                                }
                                /* all requested read buffers should now be uptodate */
                                /* dispatch write to reconstructed disk(s) */
                                locked += schedule_writes(sh);
                        }
                }

                /* Check for completed reads, read cache hits, or
                 * reconstruct-reads to be returned.
                 */
                for (i = 0; i < pd_idx; i++) {
                        column_t *col = &sh->col[i];

			if (col->read_bi && buff_uptodate(col)) {
				dprintk("Return read_bi for col %d\n", i);
				return_bi = col->read_bi;
				col->read_bi = NULL;
				copy_data(0, return_bi, sh->srcs[i], sh->sector);
				to_read--;
                                break;
			}
                }
	}

	/* Now consider returning completed write requests.
	 */
	if (written) {
		for (i = 0; i < pd_idx; i++) {
			column_t *col = &sh->col[i];

			if (col->written_bi && buff_uptodate(col) && !buff_locked(col)) {
				dprintk("Return write_bi for col %d\n", i);
				return_bi = col->written_bi;
				col->written_bi = NULL;
				written--;
                                break;
			}
		}
	}
		
	/* Now to consider writing and what else, if anything, should be read to support writing.
	 */
	if (to_write) {
                /* read/modify/write cases */
                if (sh->write_method == READ_MODIFY_WRITE && disks > 3 &&
                    disk_enabled(&sh->col[pd_idx]) && disk_enabled(&sh->col[qd_idx]) &&
                    ((failed == 0) ||
                     (failed == 1 && faila < pd_idx && !sh->col[faila].write_bi) ||
                     (failed == 2 && faila < pd_idx && !sh->col[faila].write_bi
                                  && failb < pd_idx && !sh->col[failb].write_bi))) {
                        /* read d,P,Q */
                        for (i = 0; i < disks; i++) {
                                column_t *col = &sh->col[i];
                                if ((col->write_bi || i >= pd_idx) &&
                                    !buff_uptodate(col) && !buff_locked(col)) {
                                        dprintk("Read_old col %d for Read-modify-write d,P,Q\n", i);
                                        locked += schedule_read(col, i);
                                }
                        }                                        
                        /* if nothing locked, reads are done */
                        if (locked == 0) {
                                rmw6_write_data(sh, 1);
                                locked += schedule_writes(sh);
                        }
                }
                else
                if (sh->write_method == READ_MODIFY_WRITE && disks > 3 &&
                    disk_enabled(&sh->col[pd_idx]) && !disk_enabled(&sh->col[qd_idx]) &&
                    ((failed == 1) ||
                     (faila < pd_idx && !sh->col[faila].write_bi))) {
                        /* read d,P */
                        for (i = 0; i < qd_idx; i++) {
                                column_t *col = &sh->col[i];
                                if ((col->write_bi || i == pd_idx) &&
                                    !buff_uptodate(col) && !buff_locked(col)) {
                                        dprintk("Read_old col %d for Read-modify-write d,P\n", i);
                                        locked += schedule_read(col, i);
                                }
                        }
                        /* if nothing locked, reads are done */
                        if (locked == 0) {
                                rmw5_write_data(sh);
                                locked += schedule_writes(sh);
                        }
                }
                else
                if (sh->write_method == READ_MODIFY_WRITE && disks > 3 &&
                    !disk_enabled(&sh->col[pd_idx]) && disk_enabled(&sh->col[qd_idx]) &&
                    ((failed == 1) ||
                     (failb != qd_idx && !sh->col[faila].write_bi))) {
                        /* read d,Q */
                        for (i = 0; i < disks; i++) {
                                column_t *col = &sh->col[i];
                                if ((col->write_bi || i == qd_idx) &&
                                    !buff_uptodate(col) && !buff_locked(col)) {
                                        dprintk("Read_old col %d for Read-modify-write d,Q\n", i);
                                        locked += schedule_read(col, i);
                                }
                        }
                        /* if nothing locked, reads are done */
                        if (locked == 0) {
                                rmw6_write_data(sh, 0);
                                locked += schedule_writes(sh);
                        }
                }
                else
                /* reconstruct-write cases */
                if (failed == 0) {
                        /* read all data cols except target (unless partial write) */
                        for (i = 0; i < pd_idx; i++) {
                                column_t *col = &sh->col[i];
                                if ((!col->write_bi || partial_write(sh, col)) &&
                                    !buff_uptodate(col) && !buff_locked(col)) {
                                        dprintk("Read_old col %d for Reconstruct write\n", i);
                                        locked += schedule_read(col, i);
                                }
                        }
                        /* if nothing locked, reads are done */
                        if (locked == 0) {
                                copy_write_data(sh);
                                raid6_generate_pq(sh);
                                locked += schedule_writes(sh);
                        }
                }
                else
                if (failed == 1) {
                        if (faila == pd_idx || faila == qd_idx) {
                                /* either P or Q failed */
                                /* read all data cols except target (unless partial write) */
                                for (i = 0; i < pd_idx; i++) {
                                        column_t *col = &sh->col[i];
                                        if ((!col->write_bi || partial_write(sh, col)) &&
                                            !buff_uptodate(col) && !buff_locked(col)) {
                                                dprintk("Read_old col %d for Reconstruct write\n", i);
                                                locked += schedule_read(col, i);
                                        }
                                }
                                /* if nothing locked, reads are done */
                                if (locked == 0) {
                                        copy_write_data(sh);
                                        if (disk_enabled(&sh->col[qd_idx]))
                                                raid6_generate_pq(sh);
                                        else
                                                raid5_generate_p(sh);
                                        locked += schedule_writes(sh);
                                }
                        }
                        else
                        if (sh->col[faila].write_bi && !partial_write(sh, &sh->col[faila])) {
                                /* a target data disk failed, writing full block */
                                /* read all data cols except faila */
                                for (i = 0; i < pd_idx; i++) {
                                        column_t *col = &sh->col[i];
                                        if ((i != faila) &&
                                            !buff_uptodate(col) && !buff_locked(col)) {
                                                dprintk("Read_old col %d for Reconstruct write\n", i);
                                                locked += schedule_read(col, i);
                                        }
                                }
                                /* if nothing locked, reads are done */
                                if (locked == 0) {
                                        copy_write_data(sh);
                                        raid6_generate_pq(sh);
                                        locked += schedule_writes(sh);
                                }
                        }
                        else
                        if (!sh->col[faila].write_bi || partial_write(sh, &sh->col[faila])) {
                                /* some other data disk failed or read for partial-write failed */
                                /* read all cols except faila and Q (need to generate old-d) */
                                for (i = 0; i < qd_idx; i++) {
                                        column_t *col = &sh->col[i];
                                        if ((i != faila) &&
                                            !buff_uptodate(col) && !buff_locked(col)) {
                                                dprintk("Read_old col %d for Reconstruct write\n", i);
                                                locked += schedule_read(col, i);
                                        }
                                }
                                /* if nothing locked, reads are done */
                                if (locked == 0) {
                                        raid5_generate_d(sh, faila);
                                        copy_write_data(sh);
                                        raid6_generate_pq(sh);
                                        locked += schedule_writes(sh);
                                }
                        }
                }
                else
                if (failed == 2 && (disk_enabled(&sh->col[pd_idx]) || disk_enabled(&sh->col[qd_idx]))) {
                        /* read all columns except the two that failed */
                        for (i = 0; i < disks; i++) {
                                column_t *col = &sh->col[i];
                                if ((i != faila && i != failb) &&
                                    !buff_uptodate(col) && !buff_locked(col)) {
                                        dprintk("Read_old col %d for Reconstruct write\n", i);
                                        locked += schedule_read(col, i);
                                }
                        }
                        /* if nothing locked, reads are done */
                        if (locked == 0) {
                                if (faila < pd_idx && failb < pd_idx) {
                                        /* two data disks failed */
                                        raid6_generate_dd(sh, faila, failb);
                                        copy_write_data(sh);
                                        raid6_generate_pq(sh);
                                }
                                else
                                if (faila < pd_idx && failb == pd_idx) {
                                        /* data disk and P failed */
                                        raid6_generate_dp(sh, faila);
                                        copy_write_data(sh);
                                        if (disk_enabled(&sh->col[pd_idx]))
                                                raid6_generate_pq(sh);
                                        else
                                                raid6_generate_q(sh);
                                }
                                else
                                if (faila < pd_idx && failb == qd_idx) {
                                        /* data disk and Q failed */
                                        raid5_generate_d(sh, faila);
                                        copy_write_data(sh);
                                        if (disk_enabled(&sh->col[qd_idx]))
                                                raid6_generate_pq(sh);
                                        else
                                                raid5_generate_p(sh);
                                }
                                else
                                if (faila == pd_idx && failb == qd_idx) {
                                        /* P and Q both failed */
                                        copy_write_data(sh);
                                        if (disk_enabled(&sh->col[qd_idx]))
                                                raid6_generate_pq(sh);
                                        else
                                                raid5_generate_p(sh);
                                }
                                else {
                                        BUG();
                                }
                                locked += schedule_writes(sh);
                        }
                }
                else {
                        /* straight-write cases */
                        /* if partial write we need to read first */
                        for (i = 0; i < pd_idx; i++) {
                                column_t *col = &sh->col[i];
                                if ((col->write_bi && partial_write(sh, col)) &&
                                    !buff_uptodate(col) && !buff_locked(col)) {
                                        dprintk("Read_old col %d for partial write\n", i);
                                        locked += schedule_read(col, i);
                                        break;
                                }
                        }
                        /* if nothing locked, reads are done */
                        if (locked == 0) {
                                copy_write_data(sh);
                                locked += schedule_writes(sh);
                        }
                }
        }

	/* Maybe we need to check and possibly fix the parity for this stripe.
	 * The "check parity" operation is called either when there's been an unclean
	 * shutdown or by user to "check" the array.  We try to read all disks, including
	 * parity disk(s).  If all reads are ok, we check parity; if parity mismatch, we write
	 * the computed parity to the parity disk(s).  If there was a single read error, then we
	 * compute it's contents and write it.  At end of operation, all stripes will be
	 * in sync.
	 */
	if (test_bit(STRIPE_SYNCING, &sh->state) && !test_bit(STRIPE_CLEARING, &sh->state) && failed > 2) {
                /* fail sync */
                md_sync_error(conf->mddev, sh->sector, "multiple disk errors");
                set_bit(STRIPE_INSYNC, &sh->state);
        }
        else
	if (test_bit(STRIPE_SYNCING, &sh->state) && !test_bit(STRIPE_CLEARING, &sh->state) &&
            !test_bit(STRIPE_INSYNC, &sh->state)) {
                /* read all non-failed columns */
                for (i = 0; i < disks; i++) {
                        column_t *col = &sh->col[i];
                        if (disk_valid(col) &&
                            !buff_uptodate(col) && !buff_locked(col)) {
                                dprintk("Reading col %d for sync\n", i);
                                locked += schedule_read(col, i);
                        }
                }
		/* if nothing is locked, reads are done, check/reconstruct */
		if (locked == 0) {
                        /* note: following locks columns with changed data */

                        /* these are the "check" cases */
                        if ((failed == 0) ||
                            (failed == 1 && !disk_enabled(&sh->col[faila]))) {
                                check_parity(sh, faila, conf->mddev->recovery_option);
                        }
                        /* these are the "recon" cases */
                        else
                        if (failed == 1) {
                                if (faila == pd_idx && disk_enabled(&sh->col[pd_idx])) {
                                        /* rebuilding P or read P failure */
                                        raid5_generate_p(sh);
                                }
                                else
                                if (faila == qd_idx && disk_enabled(&sh->col[qd_idx])) {
                                        /* rebuilding Q or read Q failure */
                                        raid6_generate_q(sh);
                                }
                                else
                                if (faila < pd_idx) {
                                        /* rebuiding D or read D failure */
                                        raid5_generate_d(sh, faila);
                                }
                        }
                        else
                        if (failed == 2) {
                                if (faila == pd_idx && failb == qd_idx) {
                                        if (disk_enabled(&sh->col[pd_idx])&& !disk_enabled(&sh->col[qd_idx])) {
                                                /* no Q disk, rebuilding P or read P failure */
                                                raid5_generate_p(sh);
                                        }
                                        else
                                        if (disk_enabled(&sh->col[pd_idx])&& disk_enabled(&sh->col[qd_idx])) {
                                                /* generating P+Q */
                                                raid6_generate_pq(sh);
                                        }
                                        else
                                        if (!disk_enabled(&sh->col[pd_idx]) && disk_enabled(&sh->col[qd_idx])) {
                                                /* no P disk, rebuilding Q or read Q failure */
                                                raid6_generate_q(sh);
                                        }
                                        else {
                                                /* no P disk and no Q disk */
                                                /* no-op (operation is data read-check) */
                                        }
                                }
                                else
                                if (faila < pd_idx && failb == pd_idx) {
                                        /* generating D+P */
                                        raid6_generate_dp(sh, faila);
                                }
                                else
                                if (faila < pd_idx && failb == qd_idx) {
                                        /* generating D+Q */
                                        raid5_generate_d(sh, faila);
                                        if (disk_enabled(&sh->col[qd_idx])) {
                                                raid6_generate_q(sh);
                                        }
                                }
                                else
                                if (faila < pd_idx && failb < pd_idx) {
                                        /* generating two Data disks */
                                        raid6_generate_dd(sh, faila, failb);
                                }
                        }

                        /* schedule write of reconstructed data */
                        locked += schedule_writes(sh);
                        set_bit(STRIPE_INSYNC, &sh->state);
                }
        }
        else
	/* Maybe we need to clear new data disks for this stripe.
         */
	if (test_bit(STRIPE_SYNCING, &sh->state) && test_bit(STRIPE_CLEARING, &sh->state) &&
            !test_bit(STRIPE_INSYNC, &sh->state)) {
                /* if nothing locked, no reads in progress */
                if (locked == 0) {
                        /* check for new data disks to be cleared */
                        for (i = 0; i < pd_idx; i++) {
                                column_t *col = &sh->col[i];

                                if (!disk_active(col) && disk_enabled(col)) {
                                        set_buff_locked(col);
                                }
                        }
                        /* schedule writes */
                        locked += schedule_writes(sh);
                        set_bit(STRIPE_INSYNC, &sh->state);
                }
        }

        /* Maybe a sync stripe completed.
         */
	if (test_bit(STRIPE_SYNCING, &sh->state) && test_bit(STRIPE_INSYNC, &sh->state) && (locked == 0)) {
                md_sync_done(conf->mddev, sh->sector, BUFFER_SECT);
		clear_bit(STRIPE_SYNCING, &sh->state);
		clear_bit(STRIPE_CLEARING, &sh->state);
		clear_bit(STRIPE_INSYNC, &sh->state);
	}
                        
        /* Maybe a request completed.
         */
	if (return_bi)
		bio_endio(return_bi);

	/* start new i/o */
	for (i = 0; i < disks; i++) {
		column_t *col = &sh->col[i];
		struct bio *bi = &col->bio;
                mdk_rdev_t *rdev = conf->rdev[i];
                unsigned int op_flags = 0;

                /* ensure D op_flags set for P,Q as well */
                if (col->written_bi) {
                        op_flags = (col->written_bi->bi_opf & REQ_FUA) | (col->written_bi->bi_opf & REQ_SYNC);
                        pq_flags |= op_flags;
                }

		if (test_and_clear_bit(MD_BUFF_READ, &col->state)) {
                        bio_init(bi, rdev->bdev, &col->vec, 1, REQ_OP_READ);
                        rdev->reads++;
                }
		else
		if (test_and_clear_bit(MD_BUFF_WRITE, &col->state)) {
                        bio_init(bi, rdev->bdev, &col->vec, 1, REQ_OP_WRITE | ((i < pd_idx) ? op_flags : pq_flags));
                        rdev->writes++;
                }
		else
			continue;

		dprintk("for %llu schedule op %x on col %d\n", (unsigned long long)sh->sector, bi->bi_opf, i);
		BUG_ON(!buff_locked(col));

		bi->bi_iter.bi_sector = rdev->offset + sh->sector;
		bi->bi_iter.bi_size = BUFFER_SIZE;
		bi->bi_iter.bi_idx = 0;
		bi->bi_vcnt = 1;
                bi->bi_io_vec[0].bv_page = col->page;
		bi->bi_io_vec[0].bv_len = BUFFER_SIZE;
		bi->bi_io_vec[0].bv_offset = 0;
		bi->bi_private = sh;
		bi->bi_end_io = end_request;

		atomic_inc(&sh->count);
                submit_bio_noacct(bi);
	}
}

static void end_flush(struct bio *bio)
{
	flush_stripe_t *flush_stripe = (flush_stripe_t *)bio->bi_private;

	dprintk("end_flush: unit=%d\n", flush_stripe->unit);
	
	if (atomic_dec_and_test(&flush_stripe->flush_pending))
		queue_work(md_wq, &flush_stripe->flush_work);
}

static void submit_flush_data(struct work_struct *ws)
{
	flush_stripe_t *flush_stripe = container_of(ws, flush_stripe_t, flush_work);
	unraid_conf_t *conf = mddev_to_conf(flush_stripe->mddev);
	struct bio *bi = flush_stripe->bi;
	
	if (bi->bi_iter.bi_size == 0) {
		/* an empty barrier, all done */
		dprintk("submit_flush_data: unit=%d empty barrier\n", flush_stripe->unit);
		bio_endio(bi);
	}
	else {
		dprintk("submit_flush_data: unit=%d resubmitting bio\n", flush_stripe->unit);
		bi->bi_opf &= ~REQ_PREFLUSH;
		unraid_make_request(flush_stripe->mddev, flush_stripe->unit, bi);
	}
	
	atomic_dec(&conf->active_flushes);
	kfree(flush_stripe);
}

static void submit_flush_bio(flush_stripe_t *flush_stripe, struct bio *bi, mdk_rdev_t *rdev)
{
        bio_init(bi, rdev->bdev, NULL, 0, REQ_OP_WRITE | REQ_PREFLUSH);
        bi->bi_private = flush_stripe;
        bi->bi_end_io = end_flush;
        atomic_inc(&flush_stripe->flush_pending);
        submit_bio(bi);
}

static void handle_flush(mddev_t *mddev, int unit, struct bio *bi)
{
	unraid_conf_t *conf = mddev_to_conf(mddev);
	flush_stripe_t *flush_stripe = kzalloc(sizeof(flush_stripe_t), GFP_KERNEL);

	if (flush_stripe == NULL) {
		printk("md: handle_flush: could not allocate flush_stripe!\n");
		bi->bi_status = BLK_STS_RESOURCE;
		bio_endio(bi);
		return;
	}
	atomic_inc(&conf->active_flushes);

	/* record original request */
	flush_stripe->mddev = mddev;
	flush_stripe->unit = unit;
	flush_stripe->bi = bi;

	INIT_WORK(&flush_stripe->flush_work, submit_flush_data);

        atomic_set(&flush_stripe->flush_pending, 1);
	if (disk_valid(&mddev->sb.disks[unit])) {
		/* flush target data disk */
		dprintk("handle_flush: unit=%d flush data\n", unit);
                submit_flush_bio(flush_stripe, &flush_stripe->flush_bio_D, &mddev->rdev[unit]);
	}
	if (disk_valid(&mddev->sb.disks[0])) {
		/* flush P */
		dprintk("handle_flush: unit=%d flush P\n", unit);
                submit_flush_bio(flush_stripe, &flush_stripe->flush_bio_P, &mddev->rdev[0]);
	}
	if (disk_valid(&mddev->sb.disks[MD_SB_DISKS-1])) {
		/* flush Q */
		dprintk("handle_flush: unit=%d flush Q\n", unit);
                submit_flush_bio(flush_stripe, &flush_stripe->flush_bio_Q, &mddev->rdev[MD_SB_DISKS-1]);
	}
	if (atomic_dec_and_test(&flush_stripe->flush_pending))
		queue_work(md_wq, &flush_stripe->flush_work);
}

void unraid_make_request(mddev_t *mddev, int unit, struct bio *bi)
{
	unraid_conf_t *conf = mddev_to_conf(mddev);
	sector_t stripe_sector, last_sector;

	if (md_trace >= 3)
		printk("unraid_make_request: unit=%d rwa=%x sector=%llu nsect=%u vcnt=%u\n",
		       unit, bi->bi_opf, (unsigned long long)bi->bi_iter.bi_sector, (bi->bi_iter.bi_size>>9), bi->bi_vcnt);

	/* check for requests before we're running */
	BUG_ON(!conf);

	/* check for REQ_FLUSH */
        if (bi->bi_opf & REQ_PREFLUSH) {
                dprintk("got a flush: disk: %d\n", unit);
		handle_flush(mddev, unit, bi);
		return;
	}
	
	stripe_sector = STRIPE_SECTOR(bi->bi_iter.bi_sector);
	last_sector = bio_end_sector(bi);
	while (stripe_sector < last_sector) {
		struct stripe_head *sh;

		sh = get_active_stripe(conf, unit, stripe_sector, (bi->bi_opf & REQ_RAHEAD) && (md_restrict & 2));
		if (sh) {
			add_stripe_bio(sh, bi);
                        set_bit(STRIPE_HANDLE, &sh->state);
			release_stripe(sh);
		}
		else {
			/* cannot get stripe for read-ahead, just give up */
			bi->bi_status = BLK_STS_IOERR;
			break;
		}
		
		stripe_sector += BUFFER_SECT;
	}
	
        bio_endio(bi);
        return;
}

/* Force reads to ensure only unraidd executes check_parity(). This lets us have
 * two total scribble buffers for check_parity() instead of two per stripe.
 */
int unraid_sync(mddev_t *mddev, sector_t sector_nr)
{
	unraid_conf_t *conf = mddev_to_conf(mddev);
	struct stripe_head *sh = get_active_stripe(conf, 0, sector_nr, 0);
        int i;

        set_bit(STRIPE_SYNCING, &sh->state);
        if (mddev->num_new) {
                set_bit(STRIPE_CLEARING, &sh->state);
        }
        else {
                for (i = 0; i < conf->disks; i++) {
                        column_t *col = &sh->col[i];

                        if (disk_active(col) && disk_enabled(col) &&
                            disk_valid(col) && !buff_locked(col)) {
                                clr_buff_uptodate(col);
                        }
                }
        }

        set_bit(STRIPE_HANDLE, &sh->state);
	release_stripe(sh);

	return BUFFER_SECT;
}

/*
 * This is our unraid kernel thread.
 */
static void unraidd(mddev_t *mddev, unsigned long unit)
{
	unraid_conf_t *conf = mddev_to_conf(mddev);
	int count = 0;
	struct blk_plug plug;

	dprintk("unraidd%d: activated\n", (int)unit);

	blk_start_plug(&plug);

	spin_lock_irq(&conf->device_lock);
	while (!list_empty(&conf->handle_list[unit])) {
		struct list_head *first;
		struct stripe_head *sh;

		first = conf->handle_list[unit].next;
		sh = list_entry(first, struct stripe_head, lru);
		list_del_init(first);
		clear_bit(STRIPE_HANDLE, &sh->state);
		BUG_ON(atomic_read(&sh->count) != 0);
		atomic_inc(&sh->count);

		spin_unlock_irq(&conf->device_lock);

		handle_stripe(sh);
		release_stripe(sh);
		count++;

		spin_lock_irq(&conf->device_lock);
	}
	spin_unlock_irq(&conf->device_lock);

	blk_finish_plug(&plug);

        dprintk("unraidd%d: handled %d stripes\n", (int)unit, count);
}

/* Resource allocation */

static int grow_buffers(struct stripe_head *sh, int num)
{
	int i;

	for (i = 0; i < num; i++) {
                mdp_disk_t *disk = sh->conf->disk[i];

                if (disk->state) {
                        struct page *page;

                        page = alloc_page(GFP_KERNEL);
                        if (!page)
                                return 1;
                        sh->col[i].page = page;

                        sh->srcs[i] = page_address(page);
                        memset(sh->srcs[i], 0, PAGE_SIZE);
                }
                else {
                        sh->col[i].page = NULL;
                        sh->srcs[i] = (void *)raid6_empty_zero_page;
                }
	}
	return 0;
}

static void shrink_buffers(struct stripe_head *sh, int num)
{
	int i;

	for (i = 0; i < num; i++) {
		struct page *page;

		page = sh->col[i].page;
		if (page) {
			sh->col[i].page = NULL;
			put_page(page);
		}
	}
}

static int grow_stripes(unraid_conf_t *conf, int num)
{
        size_t stripe_head_size = sizeof(struct stripe_head) + sizeof(column_t)*conf->disks;

	if (conf->slab_cache == NULL) {
                conf->slab_cache = kmem_cache_create("unraid/md", stripe_head_size, 0, 0, NULL);
                if (conf->slab_cache == NULL)
                        return 0;
        }

	while (num--) {
                struct stripe_head *sh;

		sh = kmem_cache_alloc(conf->slab_cache, GFP_KERNEL);
		if (sh == NULL)
			return 1;
		memset(sh, 0, stripe_head_size);

		sh->conf = conf;
		INIT_LIST_HEAD(&sh->lru);

		/* allocate stripe page buffers */
		if (grow_buffers(sh, conf->disks))
			return 1;

		conf->num_stripes++;

		/* we just created an active stripe so... */
		atomic_inc(&conf->active_stripes[0]);
		atomic_set(&sh->count, 1);
		release_stripe(sh);
	}

	return 1;
}

static void shrink_stripes(unraid_conf_t *conf, int num)
{
	struct stripe_head *sh;

	while (conf->num_stripes && num--) {
                spin_lock_irq(&conf->device_lock);
                wait_event_lock_irq(conf->wait_for_stripe,
                                    ((sh = get_free_stripe(conf))!=NULL),
                                    conf->device_lock);
                spin_unlock_irq(&conf->device_lock);

		shrink_buffers(sh, conf->disks);
		kmem_cache_free(conf->slab_cache, sh);

                conf->num_stripes--;
        }

	if (conf->slab_cache && !conf->num_stripes) {
		kmem_cache_destroy(conf->slab_cache);
		conf->slab_cache = NULL;
	}
}

int unraid_num_stripes(mddev_t *mddev, int num_stripes)
{
	unraid_conf_t *conf = mddev_to_conf(mddev);

        if (num_stripes > conf->num_stripes)
                grow_stripes(conf, num_stripes-conf->num_stripes);
        else
        if (num_stripes < conf->num_stripes)
                shrink_stripes(conf, conf->num_stripes-num_stripes);

        return conf->num_stripes;
}

/* The superblock device slots are assigned:
 *   0 => P (parity)
 *   1 => disk1
 *   :
 *  28 => disk28
 *  29 => Q
 *  30 => reserved (for R)
 *
 * Max number of devices = MD_SB_DISKS = 30.
 *
 * Where N is number of data disks, sh->col[] is arranged:
 *   0 => disk1
 *   :
 * N-1 => diskN
 *   N => P (parity)
 * N+1 => Q
 *
 * conf->thread[] is arranged:
 *   0 => sync thread
 *   1 => disk1 thread
 *   :
 *   N => diskN thread
 */
int unraid_run(mddev_t *mddev)
{
	unraid_conf_t *conf;
	unsigned int memory, pd_idx, i;

	/* allocate unraid specific data area */
	mddev->private = kzalloc (sizeof (unraid_conf_t), GFP_KERNEL);
	if ((conf = mddev->private) == NULL)
		goto abort;

	/* start data area initialization */
	conf->mddev = mddev;
        conf->disks = mddev->sb.num_disks;
        BUG_ON(conf->disks < 3);

        /* map columns */
        pd_idx = conf->disks-2;
        for (i = 0; i < conf->disks; i++) {
                int idx = (i < pd_idx)? i+1 : ((i == pd_idx)? 0 : MD_SB_DISKS-1);
                conf->disk[i] = &mddev->sb.disks[idx];
                conf->rdev[i] = &mddev->rdev[idx];
        }
        /* here's our scribble buffers */
        conf->p_scribble = kzalloc(BUFFER_SIZE, GFP_KERNEL);
        conf->q_scribble = kzalloc(BUFFER_SIZE, GFP_KERNEL);

	spin_lock_init(&conf->device_lock);
	init_waitqueue_head(&conf->wait_for_stripe);
	INIT_LIST_HEAD(&conf->inactive_list);

	/* allocate our i/o completion daemons */
        for (i = 0; i <= conf->disks-2; i++) {
                char name[16];

                INIT_LIST_HEAD(&conf->handle_list[i]);

                sprintf( name, "unraidd%d", i);
                conf->thread[i] = md_register_thread(unraidd, mddev, i, name);
                if (!conf->thread[i]) {
                        printk("unraid: couldn't allocate %s thread\n", name);
                        goto abort;
                }
        }

	/* allocate stripe cache hash table */
	if ((conf->stripe_hashtbl = kzalloc(PAGE_SIZE, GFP_KERNEL)) == NULL)
		goto abort;

	/* allocate the stripe cache */
	memory = md_num_stripes *
		(sizeof(struct stripe_head) + (conf->disks * PAGE_SIZE)) / 1024;
	printk("unraid: allocating %uK for %d stripes (%d disks)\n",
	       memory, md_num_stripes, conf->disks);

	if (!grow_stripes(conf, md_num_stripes)) {
		printk("unraid: couldn't allocate stripe cache\n");
		shrink_stripes(conf, conf->num_stripes);
		goto abort;
	}

	return 0;
abort:
	if (conf) {
                for (i = 0; i <= conf->disks-2; i++) {
                        if (conf->thread[i])
                                md_unregister_thread(conf->thread[i]);
                }
		kfree(conf->stripe_hashtbl);
		kfree(conf);
		mddev->private = NULL;
	}
	printk("unraid: failed to run\n");
	return -EIO;
}

int unraid_stop(mddev_t *mddev)
{
	unraid_conf_t *conf = mddev_to_conf(mddev);
        int i;
	
	if (conf) {
                int n;
                for (i = 0; i <= conf->disks-2; i++) {
                        n = atomic_read(&conf->active_stripes[i]);
                        if (n)
                                printk("unraid_stop: called with %d active stripes!\n", n);
                }
		n = atomic_read(&conf->active_flushes);
		if (n)
			printk("unraid_stop: called with %d active flushes!\n", n);

		shrink_stripes(conf, conf->num_stripes);

                for (i = 0; i <= conf->disks-2; i++) {
                        if (conf->thread[i])
                                md_unregister_thread(conf->thread[i]);
                }
		kfree(conf->stripe_hashtbl);
                kfree(conf->p_scribble);
                kfree(conf->q_scribble);
		kfree(conf);
		mddev->private = NULL;
	}

	return 0;
}

int unraid_dump(mddev_t *mddev)
{
	unraid_conf_t *conf = mddev_to_conf(mddev);
	int i;

        for (i = 0; i <= conf->disks-2; i++)
                printk("active_stripes[%i]=%i\n", i, atomic_read(&conf->active_stripes[i]));

	return 0;
}
