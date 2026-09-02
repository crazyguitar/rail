// SPDX-License-Identifier: GPL-2.0
//
// Handing dirty folios back to the peer. Writeback gathers what it can into
// whole pages, spreads one file over a window of connections, and bounds how
// many flushes may be outstanding at once.

#include <linux/backing-dev.h>
#include <linux/fs.h>
#include <linux/hash.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/writeback.h>

#include "railfs.h"
#include "railfs-tcp.h"
#include "railfs-trace.h"

static atomic_t railfs_flushes = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(railfs_flush_room);

struct railfs_flush {
	struct work_struct work;
	struct address_space *mapping;
	struct railfs_pool *pool;
	const char *path;
	unsigned int span;
	unsigned int limit;
	loff_t offset;
	size_t len;
	void *buf;
	struct folio **folios;
	unsigned int nr;
	unsigned int room;
	// Which part of the connection pool this file's writeback keeps to.
	unsigned int hint;
};

static void railfs_flush_free(struct railfs_flush *flush)
{
	kvfree(flush->buf);
	kfree(flush->folios);
	kfree(flush);
}

static void railfs_flush_one(struct work_struct *work)
{
	struct railfs_flush *flush = container_of(work, struct railfs_flush, work);
	u64 whole = railfs_now();
	struct railfs_conn *conn;
	unsigned int i;
	u64 mark;
	int put;

	mark = railfs_now();
	conn = railfs_pool_take_near(flush->pool, flush->hint, flush->span);
	railfs_trace_add(RAILFS_PHASE_POOL_WAIT, mark, 0);

	// Straight out of the page cache while the folios fit one send; past that
	// they have already been staged into the buffer.
	if (flush->nr <= RAILFS_MAX_SEND_FOLIOS) {
		put = railfs_write_folios(conn, flush->path, flush->offset, flush->folios, flush->nr, (u32)flush->len, false);
	} else {
		put = railfs_write(conn, flush->path, flush->offset, flush->buf, (u32)flush->len, false);
	}
	railfs_pool_give(flush->pool, conn);
	railfs_trace_add(RAILFS_PHASE_WRITE_TOTAL, whole, put > 0 ? (u64)put : 0);

	if (put < 0) {
		mapping_set_error(flush->mapping, put);
	}

	for (i = 0; i < flush->nr; i++) {
		folio_end_writeback(flush->folios[i]);
	}

	railfs_flush_free(flush);
	atomic_dec(&railfs_flushes);
	wake_up(&railfs_flush_room);
}

static void railfs_flush_queue(struct railfs_flush *flush)
{
	wait_event(railfs_flush_room, atomic_read(&railfs_flushes) < flush->limit);
	atomic_inc(&railfs_flushes);
	queue_work(railfs_page_wq, &flush->work);
}

static int railfs_flush_big(struct address_space *mapping, struct railfs_options *opts, const char *path, struct folio *folio, size_t bytes)
{
	loff_t pos = folio_pos(folio);
	size_t at = 0;
	void *buf;
	int err = 0;

	buf = kvmalloc(RAILFS_PAGE_SIZE, GFP_NOFS);
	if (!buf) {
		err = -ENOMEM;
		goto out;
	}

	while (at < bytes) {
		size_t piece = min_t(size_t, bytes - at, RAILFS_PAGE_SIZE);
		struct railfs_conn *conn;
		int put;

		memcpy_from_folio(buf, folio, at, piece);

		conn = railfs_pool_take(opts->pool);
		put = railfs_write(conn, path, pos + (loff_t)at, buf, (u32)piece, false);
		railfs_pool_give(opts->pool, conn);

		if (put < 0) {
			err = put;
			break;
		}

		at += piece;
	}

	kvfree(buf);
out:
	if (err) {
		mapping_set_error(mapping, err);
	}

	folio_end_writeback(folio);
	return err;
}

static struct railfs_flush *railfs_flush_new(struct address_space *mapping, struct railfs_options *opts, const char *path, unsigned int room)
{
	struct railfs_flush *flush = kzalloc(sizeof(*flush), GFP_NOFS);

	if (!flush) {
		goto out;
	}

	// No staging buffer yet: most flushes are one folio and never need one.
	flush->folios = kcalloc(room, sizeof(*flush->folios), GFP_NOFS);

	if (!flush->folios) {
		railfs_flush_free(flush);
		flush = NULL;
		goto out;
	}

	flush->mapping = mapping;
	flush->pool = opts->pool;
	flush->span = opts->flush_span;
	flush->limit = opts->flush_limit;
	flush->path = path;
	flush->room = room;
	flush->hint = (unsigned int)hash_long(mapping->host->i_ino, 32);
	INIT_WORK(&flush->work, railfs_flush_one);
out:
	return flush;
}

int railfs_writepages(struct address_space *mapping, struct writeback_control *wbc)
{
	struct inode *inode = mapping->host;
	struct railfs_options *opts = inode->i_sb->s_fs_info;
	const char *path = inode->i_private;
	unsigned int room = RAILFS_PAGE_SIZE / PAGE_SIZE;
	struct railfs_flush *flush = NULL;
	struct folio *folio = NULL;
	int error = 0;
	u64 gather;

	if (!opts || !opts->pool || !path || !railfs_page_wq) {
		error = -ENOTCONN;
		goto out;
	}

	while ((folio = writeback_iter(mapping, wbc, folio, &error))) {
		loff_t isize = i_size_read(inode);
		loff_t pos = folio_pos(folio);
		size_t bytes = folio_size(folio);

		if (pos >= isize) {
			bytes = 0;
		} else if (pos + (loff_t)bytes > isize) {
			bytes = (size_t)(isize - pos);
		}

		folio_start_writeback(folio);
		folio_unlock(folio);

		if (!bytes) {
			folio_end_writeback(folio);
			error = 0;
			continue;
		}

		if (bytes > RAILFS_PAGE_SIZE) {
			error = railfs_flush_big(mapping, opts, path, folio, bytes);
			continue;
		}

		if (flush && (pos != flush->offset + (loff_t)flush->len || flush->len + bytes > RAILFS_PAGE_SIZE || flush->nr == flush->room)) {
			railfs_flush_queue(flush);
			flush = NULL;
		}

		if (!flush) {
			flush = railfs_flush_new(mapping, opts, path, room);
		}

		if (!flush) {
			folio_end_writeback(folio);
			error = -ENOMEM;
			continue;
		}

		if (!flush->nr) {
			flush->offset = pos;
		}

		// Copied only once a flush outgrows what one send can scatter. Up to
		// that they are left where they are and sent from the page cache.
		if (flush->nr >= RAILFS_MAX_SEND_FOLIOS && !flush->buf) {
			flush->buf = kvmalloc(RAILFS_PAGE_SIZE, GFP_NOFS);
			if (!flush->buf) {
				folio_end_writeback(folio);
				error = -ENOMEM;
				continue;
			}
		}

		if (flush->nr == RAILFS_MAX_SEND_FOLIOS) {
			unsigned int seen;
			size_t at = 0;

			gather = railfs_now();
			for (seen = 0; seen < flush->nr; seen++) {
				size_t part = min_t(size_t, flush->len - at, folio_size(flush->folios[seen]));

				memcpy_from_folio((char *)flush->buf + at, flush->folios[seen], 0, part);
				at += part;
			}
			railfs_trace_add(RAILFS_PHASE_WRITE_GATHER, gather, flush->len);
		}

		if (flush->nr >= RAILFS_MAX_SEND_FOLIOS) {
			gather = railfs_now();
			memcpy_from_folio((char *)flush->buf + flush->len, folio, 0, bytes);
			railfs_trace_add(RAILFS_PHASE_WRITE_GATHER, gather, bytes);
		}

		flush->folios[flush->nr++] = folio;
		flush->len += bytes;
		error = 0;
	}

	if (flush) {
		railfs_flush_queue(flush);
	}

out:
	return error;
}

int railfs_write_begin(const struct kiocb *iocb, struct address_space *mapping, loff_t pos, unsigned int len, struct folio **foliop,
			    void **fsdata)
{
	struct folio *folio;
	int err = 0;

	// Sized to the write, not to a page. Without this a megabyte of writeback
	// is two hundred and fifty six separate folios, which is that many copies
	// into the flush buffer and far more scatter entries than a queue pair will
	// take. The mapping already allows the order; the read path uses it.
	folio = __filemap_get_folio(mapping, pos >> PAGE_SHIFT, FGP_WRITEBEGIN | fgf_set_order(len), mapping_gfp_mask(mapping));
	if (IS_ERR(folio)) {
		err = PTR_ERR(folio);
		goto out;
	}

	if (folio_test_uptodate(folio)) {
		goto ready;
	}

	if (offset_in_folio(folio, pos) == 0 && len >= folio_size(folio)) {
		folio_zero_range(folio, 0, folio_size(folio));
		folio_mark_uptodate(folio);
		goto ready;
	}

	err = railfs_fill_folio(folio);
	if (err) {
		folio_unlock(folio);
		folio_put(folio);
		goto out;
	}

ready:
	*foliop = folio;
out:
	return err;
}

int railfs_write_end(const struct kiocb *iocb, struct address_space *mapping, loff_t pos, unsigned int len, unsigned int copied,
			  struct folio *folio, void *fsdata)
{
	struct inode *inode = mapping->host;
	loff_t last = pos + copied;

	if (copied < len && !folio_test_uptodate(folio)) {
		copied = 0;
	}

	if (!copied) {
		goto out;
	}

	if (!folio_test_uptodate(folio)) {
		folio_mark_uptodate(folio);
	}

	folio_mark_dirty(folio);
	RAILFS_I(inode)->mine = true;

	if (last > i_size_read(inode)) {
		i_size_write(inode, last);
	}

out:
	folio_unlock(folio);
	folio_put(folio);
	return copied;
}

