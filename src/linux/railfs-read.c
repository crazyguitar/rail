// SPDX-License-Identifier: GPL-2.0
//
// Filling the page cache from the peer. A readahead window is cut into fetches
// of one page each, and every fetch holds a connection only for its exchange so
// the window is bounded by how many may be in flight, not by how many
// connections were mounted.

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/slab.h>

#include "railfs.h"
#include "railfs-tcp.h"
#include "railfs-trace.h"

struct railfs_read_req {
	const char *path;
	loff_t offset;
	void *buf;
	u32 len;
};

static int railfs_read_op(struct railfs_conn *conn, void *arg)
{
	struct railfs_read_req *req = arg;

	return railfs_read(conn, req->path, req->offset, req->buf, req->len);
}

static int railfs_read_retrying(struct railfs_pool *pool, const char *path, loff_t offset, void *buf, u32 len)
{
	struct railfs_read_req req = { .path = path, .offset = offset, .buf = buf, .len = len };

	return railfs_pool_call(pool, railfs_read_op, &req);
}

int railfs_fill_folio(struct folio *folio)
{
	struct inode *inode = folio->mapping->host;
	struct railfs_options *opts = inode->i_sb->s_fs_info;
	struct railfs_path *name = railfs_path_hold(inode);
	size_t size = folio_size(folio);
	loff_t pos = folio_pos(folio);
	void *buf = NULL;
	int got;
	int err;

	if (!opts || !opts->pool || !name) {
		err = -ENOTCONN;
		goto out;
	}

	buf = kvmalloc(size, GFP_NOFS);
	if (!buf) {
		err = -ENOMEM;
		goto out;
	}

	got = railfs_read_retrying(opts->pool, name->name, pos, buf, (u32)size);
	if (got < 0) {
		err = got;
		goto out;
	}

	folio_zero_range(folio, 0, size);

	if (got > 0) {
		memcpy_to_folio(folio, 0, buf, got);
	}

	folio_mark_uptodate(folio);
	err = 0;
out:
	kvfree(buf);
	railfs_path_put(name);
	return err;
}

int railfs_read_folio(struct file *file, struct folio *folio)
{
	int err = railfs_fill_folio(folio);

	folio_unlock(folio);
	return err;
}

struct railfs_fetch {
	struct work_struct work;
	struct inode *inode;
	struct address_space *mapping;
	struct railfs_pool *pool;
	char *path;
	loff_t offset;
	loff_t isize;
	u32 len;
	struct folio **folios;
	unsigned int nr;
	bool widened;
};


static void railfs_fetch_land(struct railfs_fetch *fetch, const void *buf, size_t filled)
{
	loff_t have = fetch->offset + (loff_t)filled;
	unsigned int i;

	for (i = 0; i < fetch->nr; i++) {
		struct folio *folio = fetch->folios[i];
		size_t size = folio_size(folio);
		loff_t pos = folio_pos(folio);
		loff_t stop = min_t(loff_t, pos + (loff_t)size, fetch->isize);
		size_t copy = 0;
		bool ready = false;

		if (pos >= fetch->isize) {
			ready = true;
		} else if (buf && pos >= fetch->offset && stop <= have) {
			copy = (size_t)(stop - pos);
			ready = true;
		}

		if (ready) {
			folio_zero_range(folio, 0, size);

			if (copy) {
				memcpy_to_folio(folio, 0, (const char *)buf + (pos - fetch->offset), copy);
			}

			folio_mark_uptodate(folio);
		}

		folio_unlock(folio);
		folio_put(folio);
	}
}

static void railfs_fill_around(struct railfs_fetch *fetch, const void *buf, size_t filled)
{
	pgoff_t index = (pgoff_t)(fetch->offset >> PAGE_SHIFT);
	pgoff_t last;

	if (!filled || !fetch->mapping) {
		return;
	}

	last = (pgoff_t)((fetch->offset + (loff_t)filled - 1) >> PAGE_SHIFT);

	for (; index <= last; index++) {
		struct folio *folio;
		size_t size;
		loff_t at;

		folio = __filemap_get_folio(fetch->mapping, index, FGP_LOCK | FGP_CREAT | FGP_NOWAIT, mapping_gfp_mask(fetch->mapping));
		if (IS_ERR(folio)) {
			continue;
		}

		size = folio_size(folio);
		at = folio_pos(folio) - fetch->offset;

		if (folio_test_uptodate(folio) || at < 0 || (size_t)at + size > filled) {
			folio_unlock(folio);
			folio_put(folio);
			continue;
		}

		memcpy_to_folio(folio, 0, (const char *)buf + at, size);
		folio_mark_uptodate(folio);
		folio_unlock(folio);
		folio_put(folio);
	}
}

static void railfs_fetch_free(struct railfs_fetch *fetch)
{
	iput(fetch->inode);
	kfree(fetch->folios);
	kfree(fetch->path);
	kfree(fetch);
}

static void railfs_fetch_one(struct work_struct *work)
{
	struct railfs_fetch *fetch = container_of(work, struct railfs_fetch, work);
	u64 whole = railfs_now();
	u64 mark;
	void *buf;
	int got = 0;

	mark = railfs_now();
	buf = kvmalloc(fetch->len, GFP_NOFS);
	railfs_trace_add(RAILFS_PHASE_READ_ALLOC, mark, fetch->len);

	if (buf) {
		got = railfs_read_retrying(fetch->pool, fetch->path, fetch->offset, buf, fetch->len);
	}

	mark = railfs_now();
	railfs_fetch_land(fetch, got > 0 ? buf : NULL, got > 0 ? (size_t)got : 0);
	railfs_trace_add(RAILFS_PHASE_READ_LAND, mark, got > 0 ? (u64)got : 0);

	if (got > 0 && fetch->widened) {
		mark = railfs_now();
		railfs_fill_around(fetch, buf, (size_t)got);
		railfs_trace_add(RAILFS_PHASE_READ_AROUND, mark, (u64)got);
	}

	railfs_trace_add(RAILFS_PHASE_READ_TOTAL, whole, got > 0 ? (u64)got : 0);
	railfs_trace_inflight(-1);
	kvfree(buf);
	railfs_fetch_free(fetch);
}

static void railfs_fetch_drop(struct railfs_fetch *fetch)
{
	railfs_fetch_land(fetch, NULL, 0);
	railfs_fetch_free(fetch);
}

static struct railfs_fetch *railfs_fetch_new(struct railfs_options *opts, struct address_space *mapping, const char *path, loff_t isize, loff_t offset,
					 unsigned int room)
{
	struct railfs_fetch *fetch = kzalloc(sizeof(*fetch), GFP_NOFS);

	if (!fetch) {
		goto out;
	}

	fetch->folios = kcalloc(room, sizeof(*fetch->folios), GFP_NOFS);
	fetch->path = kstrdup(path, GFP_NOFS);

	if (!fetch->folios || !fetch->path) {
		kfree(fetch->folios);
		kfree(fetch->path);
		kfree(fetch);
		fetch = NULL;
		goto out;
	}

	fetch->pool = opts->pool;
	fetch->isize = isize;
	fetch->offset = offset;
	fetch->mapping = mapping;
	fetch->inode = igrab(mapping->host);
	INIT_WORK(&fetch->work, railfs_fetch_one);
out:
	return fetch;
}

void railfs_readahead(struct readahead_control *rac)
{
	struct address_space *mapping = rac->mapping;
	struct inode *inode = rac->mapping->host;
	struct railfs_options *opts = inode->i_sb->s_fs_info;
	struct railfs_path *path = railfs_path_hold(inode);
	loff_t isize = i_size_read(inode);
	unsigned int room = opts->fetch / PAGE_SIZE;
	struct railfs_fetch *fetch = NULL;
	struct folio *folio;
	bool serving = opts && opts->pool && path && railfs_page_wq;

	while ((folio = readahead_folio(rac)) != NULL) {
		folio_get(folio);

		if (!serving) {
			folio_unlock(folio);
			folio_put(folio);
			continue;
		}

		if (fetch && (fetch->nr == room || fetch->len + folio_size(folio) > opts->fetch ||
			      folio_pos(folio) != fetch->offset + (loff_t)fetch->len)) {
			railfs_trace_inflight(1);
			queue_work(railfs_page_wq, &fetch->work);
			fetch = NULL;
		}

		if (!fetch) {
			fetch = railfs_fetch_new(opts, mapping, path->name, isize, folio_pos(folio), room);
		}

		if (!fetch) {
			folio_unlock(folio);
			folio_put(folio);
			continue;
		}

		fetch->folios[fetch->nr++] = folio;
		fetch->len += (u32)folio_size(folio);
	}

	if (fetch && fetch->len) {
		if (fetch->len < opts->block) {
			loff_t end = fetch->offset + (loff_t)fetch->len;

			fetch->offset = round_down(fetch->offset, opts->block);
			fetch->len = (u32)max_t(loff_t, opts->block, end - fetch->offset);
			fetch->widened = true;
		}

		railfs_trace_inflight(1);
		queue_work(railfs_page_wq, &fetch->work);
	} else if (fetch) {
		railfs_fetch_drop(fetch);
	}

	railfs_path_put(path);
}

