/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RAILFS_COMPAT_H
#define RAILFS_COMPAT_H

/* Every kernel difference the module builds across, 5.15 to 6.17. What a
 * kernel lacks is defined here under its upstream name; what a distribution
 * backported is found by railfs-compat.mk grepping the kernel's own headers.
 */

#include <linux/version.h>
#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/uio.h>
#include <linux/writeback.h>

#ifdef CONFIG_HIGHMEM
#error "railfs maps a folio once and reads it linearly, which needs a kernel without highmem"
#endif

#define RAILFS_KERNEL_AT_LEAST(major, minor) (LINUX_VERSION_CODE >= KERNEL_VERSION(major, minor, 0))

#define RAILFS_HAS_FOLIO RAILFS_KERNEL_AT_LEAST(5, 16)
#define RAILFS_HAS_ALLOC_INODE_SB RAILFS_KERNEL_AT_LEAST(5, 18)
#define RAILFS_HAS_DIRTY_FOLIO RAILFS_KERNEL_AT_LEAST(5, 18)
#define RAILFS_HAS_READ_FOLIO RAILFS_KERNEL_AT_LEAST(5, 19)
#define RAILFS_HAS_FMODE_CAN_ODIRECT RAILFS_KERNEL_AT_LEAST(5, 19)
#define RAILFS_HAS_WRITE_BEGIN_FLAGS (!RAILFS_KERNEL_AT_LEAST(5, 19))
#define RAILFS_HAS_MNT_IDMAP RAILFS_KERNEL_AT_LEAST(6, 3)
#define RAILFS_HAS_EXTRACT_PAGES RAILFS_KERNEL_AT_LEAST(6, 5)
#define RAILFS_HAS_FOLIO_WRITEPAGE_T RAILFS_KERNEL_AT_LEAST(6, 3)
#define RAILFS_HAS_FGP_WRITEBEGIN RAILFS_KERNEL_AT_LEAST(6, 4)
#define RAILFS_HAS_FGF_ORDER RAILFS_KERNEL_AT_LEAST(6, 6)
#define RAILFS_HAS_MEMCPY_FOLIO RAILFS_KERNEL_AT_LEAST(6, 6)
#ifndef RAILFS_HAS_CTIME_ACCESSORS
#define RAILFS_HAS_CTIME_ACCESSORS RAILFS_KERNEL_AT_LEAST(6, 6)
#endif
#define RAILFS_HAS_TIME_ACCESSORS RAILFS_KERNEL_AT_LEAST(6, 7)
#define RAILFS_HAS_WRITEBACK_ITER RAILFS_KERNEL_AT_LEAST(6, 9)
#define RAILFS_HAS_FOLIO_WRITE_BEGIN RAILFS_KERNEL_AT_LEAST(6, 12)
#define RAILFS_HAS_FOLIO_ORDER_RANGE RAILFS_KERNEL_AT_LEAST(6, 12)
#define RAILFS_HAS_REVALIDATE_NAME RAILFS_KERNEL_AT_LEAST(6, 14)
#define RAILFS_HAS_MKDIR_DENTRY RAILFS_KERNEL_AT_LEAST(6, 15)
#define RAILFS_HAS_KIOCB_WRITE_BEGIN RAILFS_KERNEL_AT_LEAST(6, 17)
#define RAILFS_HAS_MMAP_PREPARE RAILFS_KERNEL_AT_LEAST(6, 17)
#define RAILFS_HAS_DEFAULT_D_OP RAILFS_KERNEL_AT_LEAST(6, 17)

static inline ssize_t railfs_extract_pages(struct iov_iter *iter, struct page ***pages, size_t size,
					 unsigned int maxpages, size_t *offset)
{
#if RAILFS_HAS_EXTRACT_PAGES
	return iov_iter_extract_pages(iter, pages, size, maxpages, 0, offset);
#else
	ssize_t got;

	if (!*pages) {
		*pages = kvmalloc_array(maxpages, sizeof(**pages), GFP_KERNEL);
		if (!*pages) {
			return -ENOMEM;
		}
	}
#if RAILFS_KERNEL_AT_LEAST(6, 0)
	got = iov_iter_get_pages2(iter, *pages, size, maxpages, offset);
#else
	got = iov_iter_get_pages(iter, *pages, size, maxpages, offset);
	if (got > 0) {
		iov_iter_advance(iter, got);
	}
#endif
	return got;
#endif
}

static inline void railfs_release_pages(const struct iov_iter *iter, struct page **pages, unsigned int nr)
{
#if RAILFS_HAS_EXTRACT_PAGES
	if (iov_iter_extract_will_pin(iter)) {
		unpin_user_pages(pages, nr);
	}
#else
	unsigned int i;

	for (i = 0; i < nr; i++) {
		put_page(pages[i]);
	}
#endif
}

#if !RAILFS_HAS_FOLIO
struct folio {
	struct page page;
};

static inline struct folio *page_folio(struct page *page)
{
	return (struct folio *)page;
}

#define folio_page(folio, n) (&(folio)->page)

static inline struct page *folio_file_page(struct folio *folio, pgoff_t index)
{
	return &folio->page;
}

static inline size_t folio_size(struct folio *folio)
{
	return PAGE_SIZE;
}

static inline loff_t folio_pos(struct folio *folio)
{
	return page_offset(&folio->page);
}

#define offset_in_folio(folio, p) offset_in_page(p)

static inline void *folio_address(struct folio *folio)
{
	return page_address(&folio->page);
}

static inline struct inode *folio_inode(struct folio *folio)
{
	return folio->page.mapping->host;
}

static inline void folio_get(struct folio *folio)
{
	get_page(&folio->page);
}

static inline void folio_put(struct folio *folio)
{
	put_page(&folio->page);
}

static inline void folio_unlock(struct folio *folio)
{
	unlock_page(&folio->page);
}

static inline bool folio_test_uptodate(struct folio *folio)
{
	return PageUptodate(&folio->page);
}

static inline void folio_mark_uptodate(struct folio *folio)
{
	SetPageUptodate(&folio->page);
}

static inline bool folio_mark_dirty(struct folio *folio)
{
	return set_page_dirty(&folio->page);
}

static inline void folio_start_writeback(struct folio *folio)
{
	set_page_writeback(&folio->page);
}

static inline void folio_end_writeback(struct folio *folio)
{
	end_page_writeback(&folio->page);
}

static inline void folio_redirty_for_writepage(struct writeback_control *wbc, struct folio *folio)
{
	redirty_page_for_writepage(wbc, &folio->page);
}

static inline void folio_zero_range(struct folio *folio, size_t start, size_t length)
{
	zero_user(&folio->page, start, length);
}

static inline void *kmap_local_folio(struct folio *folio, size_t offset)
{
	return kmap_local_page(&folio->page) + offset;
}

static inline struct folio *readahead_folio(struct readahead_control *rac)
{
	struct page *page = readahead_page(rac);

	if (!page) {
		return NULL;
	}
	put_page(page);
	return page_folio(page);
}
#endif

#if !RAILFS_HAS_MEMCPY_FOLIO
static inline void memcpy_to_folio(struct folio *folio, size_t offset, const char *from, size_t len)
{
	memcpy(folio_address(folio) + offset, from, len);
}

static inline void memcpy_from_folio(char *to, struct folio *folio, size_t offset, size_t len)
{
	memcpy(to, folio_address(folio) + offset, len);
}
#endif

/* No ceiling means folios wider than a fetch, which a short read would zero. */
#if !RAILFS_HAS_FOLIO_ORDER_RANGE
static inline void mapping_set_folio_order_range(struct address_space *mapping, unsigned int min, unsigned int max)
{
}
#endif

#if !RAILFS_HAS_FGP_WRITEBEGIN
#ifdef FGP_STABLE
#define FGP_WRITEBEGIN (FGP_LOCK | FGP_WRITE | FGP_CREAT | FGP_STABLE)
#else
#define FGP_WRITEBEGIN (FGP_LOCK | FGP_WRITE | FGP_CREAT)
#endif
#endif

#if !RAILFS_HAS_FGF_ORDER
typedef unsigned int __bitwise fgf_t;

static inline fgf_t fgf_set_order(size_t size)
{
	return 0;
}
#endif

#if RAILFS_HAS_FGP_WRITEBEGIN
static inline struct folio *railfs_get_folio(struct address_space *mapping, pgoff_t index, fgf_t fgp)
{
	return __filemap_get_folio(mapping, index, fgp, mapping_gfp_mask(mapping));
}
#elif RAILFS_HAS_FOLIO
static inline struct folio *railfs_get_folio(struct address_space *mapping, pgoff_t index, fgf_t fgp)
{
	struct folio *folio = __filemap_get_folio(mapping, index, fgp, mapping_gfp_mask(mapping));

	return folio ? folio : ERR_PTR(-ENOMEM);
}
#else
static inline struct folio *railfs_get_folio(struct address_space *mapping, pgoff_t index, fgf_t fgp)
{
	struct page *page = pagecache_get_page(mapping, index, fgp, mapping_gfp_mask(mapping));

	return page ? page_folio(page) : ERR_PTR(-ENOMEM);
}
#endif

#if !RAILFS_HAS_ALLOC_INODE_SB
static inline void *alloc_inode_sb(struct super_block *sb, struct kmem_cache *cache, gfp_t gfp)
{
	return kmem_cache_alloc(cache, gfp);
}
#endif

#if !RAILFS_HAS_FMODE_CAN_ODIRECT
#define FMODE_CAN_ODIRECT ((__force fmode_t)0)
#endif

#if !RAILFS_HAS_CTIME_ACCESSORS
static inline struct timespec64 inode_set_ctime_to_ts(struct inode *inode, struct timespec64 ts)
{
	inode->i_ctime = ts;
	return ts;
}

static inline struct timespec64 inode_set_ctime_current(struct inode *inode)
{
	return inode_set_ctime_to_ts(inode, current_time(inode));
}
#endif

#if !RAILFS_HAS_TIME_ACCESSORS
static inline struct timespec64 inode_set_mtime_to_ts(struct inode *inode, struct timespec64 ts)
{
	inode->i_mtime = ts;
	return ts;
}

static inline struct timespec64 inode_set_atime_to_ts(struct inode *inode, struct timespec64 ts)
{
	inode->i_atime = ts;
	return ts;
}

static inline time64_t inode_get_mtime_sec(const struct inode *inode)
{
	return inode->i_mtime.tv_sec;
}

static inline struct timespec64 simple_inode_init_ts(struct inode *inode)
{
	struct timespec64 ts = inode_set_ctime_current(inode);

	inode_set_atime_to_ts(inode, ts);
	inode_set_mtime_to_ts(inode, ts);
	return ts;
}
#endif

#if !RAILFS_HAS_MNT_IDMAP
#define mnt_idmap user_namespace
#endif

#if !RAILFS_HAS_DEFAULT_D_OP
static inline void set_default_d_op(struct super_block *sb, const struct dentry_operations *ops)
{
	sb->s_d_op = ops;
}
#endif

#endif
