/*
 * Ram backed encrypted block device driver.
 *
 * Copyright (C) 2007 Nick Piggin
 * Copyright (C) 2007 Novell Inc.
 * Crypto implementation by Brock Smedley.
 *
 * Parts derived from drivers/block/rd.c, and drivers/block/loop.c, copyright
 * of their respective owners.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/major.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/highmem.h>
#include <linux/mutex.h>
#include <linux/radix-tree.h>
#include <linux/fs.h>
#include <linux/slab.h>

#include <asm/uaccess.h>

// for encryption n stuff
#include <linux/crypto.h>
#include <linux/err.h>
#include <linux/scatterlist.h>

#define SECTOR_SHIFT		9
#define PAGE_SECTORS_SHIFT	(PAGE_SHIFT - SECTOR_SHIFT)
#define PAGE_SECTORS		(1 << PAGE_SECTORS_SHIFT)

// also for encryption ( ͡° ͜ʖ°
#define SHA1_LENGTH     20

/*
 * Each block ramdisk device has a radix_tree brk_pages of pages that stores
 * the pages containing the block device's contents. A brk page's ->index is
 * its offset in PAGE_SIZE units. This is similar to, but in no way connected
 * with, the kernel's pagecache or buffer cache (which sit above our block
 * device).
 */
struct brk_device {
	int		brk_number;

	struct request_queue	*brk_queue;
	struct gendisk		*brk_disk;
	struct list_head	brk_list;

	/*
	 * Backing store of pages and lock to protect it. This is the contents
	 * of the block device.
	 */
	spinlock_t		brk_lock;
	struct radix_tree_root	brk_pages;
};

/*
 * Look up and return a brk's page for a given sector.
 */
static DEFINE_MUTEX(brk_mutex);
static struct page *brk_lookup_page(struct brk_device *brk, sector_t sector)
{
	pgoff_t idx;
	struct page *page;

	/*
	 * The page lifetime is protected by the fact that we have opened the
	 * device node -- brk pages will never be deleted under us, so we
	 * don't need any further locking or refcounting.
	 *
	 * This is strictly true for the radix-tree nodes as well (ie. we
	 * don't actually need the rcu_read_lock()), however that is not a
	 * documented feature of the radix-tree API so it is better to be
	 * safe here (we don't have total exclusion from radix tree updates
	 * here, only deletes).
	 */
	rcu_read_lock();
	idx = sector >> PAGE_SECTORS_SHIFT; /* sector to page index */
	page = radix_tree_lookup(&brk->brk_pages, idx);
	rcu_read_unlock();

	BUG_ON(page && page->index != idx);

	return page;
}

/*
 * Look up and return a brk's page for a given sector.
 * If one does not exist, allocate an empty page, and insert that. Then
 * return it.
 */
static struct page *brk_insert_page(struct brk_device *brk, sector_t sector)
{
	pgoff_t idx;
	struct page *page;
	gfp_t gfp_flags;

	page = brk_lookup_page(brk, sector);
	if (page)
		return page;

	/*
	 * Must use NOIO because we don't want to recurse back into the
	 * block or filesystem layers from page reclaim.
	 *
	 * Cannot support XIP and highmem, because our ->direct_access
	 * routine for XIP must return memory that is always addressable.
	 * If XIP was reworked to use pfns and kmap throughout, this
	 * restriction might be able to be lifted.
	 */
	gfp_flags = GFP_NOIO | __GFP_ZERO;
#ifndef CONFIG_BRK_DEV_XIP
	gfp_flags |= __GFP_HIGHMEM;
#endif
	page = alloc_page(gfp_flags);
	if (!page)
		return NULL;

	if (radix_tree_preload(GFP_NOIO)) {
		__free_page(page);
		return NULL;
	}

	spin_lock(&brk->brk_lock);
	idx = sector >> PAGE_SECTORS_SHIFT;
	page->index = idx;
	if (radix_tree_insert(&brk->brk_pages, idx, page)) {
		__free_page(page);
		page = radix_tree_lookup(&brk->brk_pages, idx);
		BUG_ON(!page);
		BUG_ON(page->index != idx);
	}
	spin_unlock(&brk->brk_lock);

	radix_tree_preload_end();

	return page;
}

static void brk_free_page(struct brk_device *brk, sector_t sector)
{
	struct page *page;
	pgoff_t idx;

	spin_lock(&brk->brk_lock);
	idx = sector >> PAGE_SECTORS_SHIFT;
	page = radix_tree_delete(&brk->brk_pages, idx);
	spin_unlock(&brk->brk_lock);
	if (page)
		__free_page(page);
}

static void brk_zero_page(struct brk_device *brk, sector_t sector)
{
	struct page *page;

	page = brk_lookup_page(brk, sector);
	if (page)
		clear_highpage(page);
}

/*
 * Free all backing store pages and radix tree. This must only be called when
 * there are no other users of the device.
 */
#define FREE_BATCH 16
static void brk_free_pages(struct brk_device *brk)
{
	unsigned long pos = 0;
	struct page *pages[FREE_BATCH];
	int nr_pages;

	do {
		int i;

		nr_pages = radix_tree_gang_lookup(&brk->brk_pages,
				(void **)pages, pos, FREE_BATCH);

		for (i = 0; i < nr_pages; i++) {
			void *ret;

			BUG_ON(pages[i]->index < pos);
			pos = pages[i]->index;
			ret = radix_tree_delete(&brk->brk_pages, pos);
			BUG_ON(!ret || ret != pages[i]);
			__free_page(pages[i]);
		}

		pos++;

		/*
		 * This assumes radix_tree_gang_lookup always returns as
		 * many pages as possible. If the radix-tree code changes,
		 * so will this have to.
		 */
	} while (nr_pages == FREE_BATCH);
}

/*
 * copy_to_brk_setup must be called before copy_to_brk. It may sleep.
 */
static int copy_to_brk_setup(struct brk_device *brk, sector_t sector, size_t n)
{
	unsigned int offset = (sector & (PAGE_SECTORS-1)) << SECTOR_SHIFT;
	size_t copy;

	copy = min_t(size_t, n, PAGE_SIZE - offset);
	if (!brk_insert_page(brk, sector))
		return -ENOSPC;
	if (copy < n) {
		sector += copy >> SECTOR_SHIFT;
		if (!brk_insert_page(brk, sector))
			return -ENOSPC;
	}
	return 0;
}

// get rid of some stuff on your brick
static void discard_from_brk(struct brk_device *brk,
			sector_t sector, size_t n)
{
	while (n >= PAGE_SIZE) {
		/*
		 * Don't want to actually discard pages here because
		 * re-allocating the pages can result in writeback
		 * deadlocks under heavy load.
		 */
		if (0)
			brk_free_page(brk, sector);
		else
			brk_zero_page(brk, sector);
		sector += PAGE_SIZE >> SECTOR_SHIFT;
		n -= PAGE_SIZE;
	}
}

/*
 * Copy n bytes from src to the brk starting at sector. Does not sleep.
 */
static void copy_to_brk(struct brk_device *brk, const void *src,
			sector_t sector, size_t n)
{
	struct page *page;
	void *dst;
	unsigned int offset = (sector & (PAGE_SECTORS-1)) << SECTOR_SHIFT;
	size_t copy;
	struct crypto_hash *bitcoin;
	struct hash_desc money;
	unsigned char lambos[SHA1_LENGTH];
//	unsigned char dayjob[PAGE_SIZE];
	unsigned char dayjob[10];
	int lol;
	struct scatterlist brain;

	copy = min_t(size_t, n, PAGE_SIZE - offset);
	page = brk_lookup_page(brk, sector);
	BUG_ON(!page);

	dst = kmap_atomic(page);
	printk(KERN_INFO "sha1: %s\n", __FUNCTION__);

	// make some fake data
	memset(dayjob, 'A', 10);
	memset(lambos, 0x00, SHA1_LENGTH);

	bitcoin = crypto_alloc_hash("sha1", 0, CRYPTO_ALG_ASYNC);

	money.tfm = bitcoin;
	money.flags = 0;

	
	// encrypt data here
	sg_init_one(&brain, &dayjob, copy);
	crypto_hash_init(&money);
	crypto_hash_update(&money, &brain, 10);
	crypto_hash_final(&money, lambos);
	// this is an example that prints some bullshit
	for (lol = 0; lol < 20; lol++){
	  printk(KERN_ERR "%d-%d\n", lambos[lol], lol);
	}
	crypto_free_hash(bitcoin);
	
	// then copy into disk
	memcpy(dst + offset, src, copy);
	kunmap_atomic(dst);

	if (copy < n) {
		src += copy;
		sector += copy >> SECTOR_SHIFT;
		copy = n - copy;
		page = brk_lookup_page(brk, sector);
		BUG_ON(!page);

		dst = kmap_atomic(page);
		memcpy(dst, src, copy);
		kunmap_atomic(dst);
	}
}

/*
 * Copy n bytes to dst from the brk starting at sector. Does not sleep.
 */
static void copy_from_brk(void *dst, struct brk_device *brk,
			sector_t sector, size_t n)
{
	struct page *page;
	void *src;
	unsigned int offset = (sector & (PAGE_SECTORS-1)) << SECTOR_SHIFT;
	size_t copy;

	copy = min_t(size_t, n, PAGE_SIZE - offset);
	page = brk_lookup_page(brk, sector);
	if (page) {
		src = kmap_atomic(page);
		// decrypt data here
		// ...
		// then copy from brick
		memcpy(dst, src + offset, copy);
		kunmap_atomic(src);
	} else
		memset(dst, 0, copy);

	if (copy < n) {
		dst += copy;
		sector += copy >> SECTOR_SHIFT;
		copy = n - copy;
		page = brk_lookup_page(brk, sector);
		if (page) {
			src = kmap_atomic(page);
			memcpy(dst, src, copy);
			kunmap_atomic(src);
		} else
			memset(dst, 0, copy);
	}
}

/*
 * Process a single bvec of a bio.
 */
static int brk_do_bvec(struct brk_device *brk, struct page *page,
			unsigned int len, unsigned int off, int rw,
			sector_t sector)
{
	void *mem;
	int err = 0;

	if (rw != READ) {
		err = copy_to_brk_setup(brk, sector, len);
		if (err)
			goto out;
	}

	mem = kmap_atomic(page);
	if (rw == READ) {
		copy_from_brk(mem + off, brk, sector, len);
		flush_dcache_page(page);
	} else {
		flush_dcache_page(page);
		copy_to_brk(brk, mem + off, sector, len);
	}
	kunmap_atomic(mem);

out:
	return err;
}

// request a brick
static void brk_make_request(struct request_queue *q, struct bio *bio)
{
	struct block_device *bdev = bio->bi_bdev;
	struct brk_device *brk = bdev->bd_disk->private_data;
	int rw;
	struct bio_vec bvec;
	sector_t sector;
	struct bvec_iter iter;
	int err = -EIO;

	sector = bio->bi_iter.bi_sector;
	if (bio_end_sector(bio) > get_capacity(bdev->bd_disk))
		goto out;

	if (unlikely(bio->bi_rw & REQ_DISCARD)) {
		err = 0;
		discard_from_brk(brk, sector, bio->bi_iter.bi_size);
		goto out;
	}

	rw = bio_rw(bio);
	if (rw == READA)
		rw = READ;

	bio_for_each_segment(bvec, bio, iter) {
		unsigned int len = bvec.bv_len;
		err = brk_do_bvec(brk, bvec.bv_page, len,
					bvec.bv_offset, rw, sector);
		if (err)
			break;
		sector += len >> SECTOR_SHIFT;
	}

out:
	bio_endio(bio, err);
}

// get a read-write brick page
static int brk_rw_page(struct block_device *bdev, sector_t sector,
		       struct page *page, int rw)
{
	struct brk_device *brk = bdev->bd_disk->private_data;
	int err = brk_do_bvec(brk, page, PAGE_CACHE_SIZE, 0, rw, sector);
	page_endio(page, rw & WRITE, err);
	return err;
}

#ifdef CONFIG_BRK_DEV_XIP
static int brk_direct_access(struct block_device *bdev, sector_t sector,
			void **kaddr, unsigned long *pfn)
{
	struct brk_device *brk = bdev->bd_disk->private_data;
	struct page *page;

	if (!brk)
		return -ENODEV;
	if (sector & (PAGE_SECTORS-1))
		return -EINVAL;
	if (sector + PAGE_SECTORS > get_capacity(bdev->bd_disk))
		return -ERANGE;
	page = brk_insert_page(brk, sector);
	if (!page)
		return -ENOSPC;
	*kaddr = page_address(page);
	*pfn = page_to_pfn(page);

	return 0;
}
#endif

static int brk_ioctl(struct block_device *bdev, fmode_t mode,
			unsigned int cmd, unsigned long arg)
{
	int error;
	struct brk_device *brk = bdev->bd_disk->private_data;

	if (cmd != BLKFLSBUF)
		return -ENOTTY;

	/*
	 * ram device BLKFLSBUF has special semantics, we want to actually
	 * release and destroy the ramdisk data.
	 */
	mutex_lock(&brk_mutex);
	mutex_lock(&bdev->bd_mutex);
	error = -EBUSY;
	if (bdev->bd_openers <= 1) {
		/*
		 * Kill the cache first, so it isn't written back to the
		 * device.
		 *
		 * Another thread might instantiate more buffercache here,
		 * but there is not much we can do to close that race.
		 */
		kill_bdev(bdev);
		brk_free_pages(brk);
		error = 0;
	}
	mutex_unlock(&bdev->bd_mutex);
	mutex_unlock(&brk_mutex);

	return error;
}

static const struct block_device_operations brk_fops = {
	.owner =		THIS_MODULE,
	.rw_page =		brk_rw_page,
	.ioctl =		brk_ioctl,
#ifdef CONFIG_BRK_DEV_XIP
	.direct_access =	brk_direct_access,
#endif
};

/*
 * And now the modules code and kernel interface.
 */
static int rd_nr;
int rd_size = CONFIG_BRK_DEV_RAM_SIZE;
static int max_part;
static int part_shift;
static int part_show = 0;
module_param(rd_nr, int, S_IRUGO);
MODULE_PARM_DESC(rd_nr, "Maximum number of brk devices");
module_param(rd_size, int, S_IRUGO);
MODULE_PARM_DESC(rd_size, "Size of each RAM disk in kbytes.");
module_param(max_part, int, S_IRUGO);
MODULE_PARM_DESC(max_part, "Maximum number of partitions per RAM disk");
module_param(part_show, int, S_IRUGO);
MODULE_PARM_DESC(part_show, "Control RAM disk visibility in /proc/partitions");
MODULE_LICENSE("GPL");
MODULE_ALIAS_BLOCKDEV_MAJOR(RAMDISK_MAJOR);
MODULE_ALIAS("rd");

#ifndef MODULE
/* Legacy boot options - nonmodular */
static int __init ramdisk_size(char *str)
{
	rd_size = simple_strtol(str, NULL, 0);
	return 1;
}
__setup("ramdisk_size=", ramdisk_size);
#endif

/*
 * The device scheme is derived from loop.c. Keep them in synch where possible
 * (should share code eventually).
 */
static LIST_HEAD(brk_devices);
static DEFINE_MUTEX(brk_devices_mutex);

// allocate a brick
static struct brk_device *brk_alloc(int i)
{
	struct brk_device *brk;
	struct gendisk *disk;

	brk = kzalloc(sizeof(*brk), GFP_KERNEL);
	if (!brk)
		goto out;
	brk->brk_number		= i;
	spin_lock_init(&brk->brk_lock);
	INIT_RADIX_TREE(&brk->brk_pages, GFP_ATOMIC);

	brk->brk_queue = blk_alloc_queue(GFP_KERNEL);
	if (!brk->brk_queue)
		goto out_free_dev;
	blk_queue_make_request(brk->brk_queue, brk_make_request);
	blk_queue_max_hw_sectors(brk->brk_queue, 1024);
	blk_queue_bounce_limit(brk->brk_queue, BLK_BOUNCE_ANY);

	brk->brk_queue->limits.discard_granularity = PAGE_SIZE;
	brk->brk_queue->limits.max_discard_sectors = UINT_MAX;
	brk->brk_queue->limits.discard_zeroes_data = 1;
	queue_flag_set_unlocked(QUEUE_FLAG_DISCARD, brk->brk_queue);

	disk = brk->brk_disk = alloc_disk(1 << part_shift);
	if (!disk)
		goto out_free_queue;
	disk->major		= RAMDISK_MAJOR;
	disk->first_minor	= i << part_shift;
	disk->fops		= &brk_fops;
	disk->private_data	= brk;
	disk->queue		= brk->brk_queue;
	if (!part_show)
		disk->flags |= GENHD_FL_SUPPRESS_PARTITION_INFO;
	sprintf(disk->disk_name, "brick%d", i);
	set_capacity(disk, rd_size * 2);

	return brk;

out_free_queue:
	blk_cleanup_queue(brk->brk_queue);
out_free_dev:
	kfree(brk);
out:
	return NULL;
}

// free a brick
static void brk_free(struct brk_device *brk)
{
	put_disk(brk->brk_disk);
	blk_cleanup_queue(brk->brk_queue);
	brk_free_pages(brk);
	kfree(brk);
}

// initialize a brick
static struct brk_device *brk_init_one(int i)
{
	struct brk_device *brk;

	list_for_each_entry(brk, &brk_devices, brk_list) {
		if (brk->brk_number == i)
			goto out;
	}

	brk = brk_alloc(i);
	if (brk) {
		add_disk(brk->brk_disk);
		list_add_tail(&brk->brk_list, &brk_devices);
	}
out:
	return brk;
}

// delete one brick
static void brk_del_one(struct brk_device *brk)
{
	list_del(&brk->brk_list);
	del_gendisk(brk->brk_disk);
	brk_free(brk);
}

// probe a brick
static struct kobject *brk_probe(dev_t dev, int *part, void *data)
{
	struct brk_device *brk;
	struct kobject *kobj;

	mutex_lock(&brk_devices_mutex);
	brk = brk_init_one(MINOR(dev) >> part_shift);
	kobj = brk ? get_disk(brk->brk_disk) : NULL;
	mutex_unlock(&brk_devices_mutex);

	*part = 0;
	return kobj;
}

static int __init brk_init(void)
{
	int i, nr;
	unsigned long range;
	struct brk_device *brk, *next;

	/*
	 * brk module now has a feature to instantiate underlying device
	 * structure on-demand, provided that there is an access dev node.
	 * However, this will not work well with user space tool that doesn't
	 * know about such "feature".  In order to not break any existing
	 * tool, we do the following:
	 *
	 * (1) if rd_nr is specified, create that many upfront, and this
	 *     also becomes a hard limit.
	 * (2) if rd_nr is not specified, create CONFIG_BRK_DEV_RAM_COUNT
	 *     (default 16) rd device on module load, user can further
	 *     extend brk device by create dev node themselves and have
	 *     kernel automatically instantiate actual device on-demand.
	 */

	part_shift = 0;
	if (max_part > 0) {
		part_shift = fls(max_part);

		/*
		 * Adjust max_part according to part_shift as it is exported
		 * to user space so that user can decide correct minor number
		 * if [s]he want to create more devices.
		 *
		 * Note that -1 is required because partition 0 is reserved
		 * for the whole disk.
		 */
		max_part = (1UL << part_shift) - 1;
	}

	if ((1UL << part_shift) > DISK_MAX_PARTS)
		return -EINVAL;

	if (rd_nr > 1UL << (MINORBITS - part_shift))
		return -EINVAL;

	if (rd_nr) {
		nr = rd_nr;
		range = rd_nr << part_shift;
	} else {
		nr = CONFIG_BRK_DEV_RAM_COUNT;
		range = 1UL << MINORBITS;
	}

	if (register_blkdev(RAMDISK_MAJOR, "ramdisk"))
		return -EIO;

	for (i = 0; i < nr; i++) {
		brk = brk_alloc(i);
		if (!brk)
			goto out_free;
		list_add_tail(&brk->brk_list, &brk_devices);
	}

	/* point of no return */

	list_for_each_entry(brk, &brk_devices, brk_list)
		add_disk(brk->brk_disk);

	blk_register_region(MKDEV(RAMDISK_MAJOR, 0), range,
				  THIS_MODULE, brk_probe, NULL, NULL);

	printk(KERN_INFO "brk: module loaded\n");
	return 0;

out_free:
	list_for_each_entry_safe(brk, next, &brk_devices, brk_list) {
		list_del(&brk->brk_list);
		brk_free(brk);
	}
	unregister_blkdev(RAMDISK_MAJOR, "ramdisk");

	return -ENOMEM;
}

static void __exit brk_exit(void)
{
	unsigned long range;
	struct brk_device *brk, *next;

	range = rd_nr ? rd_nr << part_shift : 1UL << MINORBITS;

	list_for_each_entry_safe(brk, next, &brk_devices, brk_list)
		brk_del_one(brk);

	blk_unregister_region(MKDEV(RAMDISK_MAJOR, 0), range);
	unregister_blkdev(RAMDISK_MAJOR, "ramdisk");
}

module_init(brk_init);
module_exit(brk_exit);

