// SPDX-License-Identifier: GPL-2.0
//
// Listing a directory, and priming the dcache from that listing.

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/statfs.h>

#include "railfs.h"
#include "railfs-tcp.h"
#include "railfs-trace.h"

// A listing is the only place a name arrives off the wire rather than from a
// path walk. The dcache cannot hold one with a separator in it, and one that
// shadows the dots readdir emits itself would be seen twice.
bool railfs_name_is_usable(const char *name)
{
	return *name && strcmp(name, ".") != 0 && strcmp(name, "..") != 0 && !strchr(name, '/');
}

// A listing already carries every entry's attributes, and a cold ls -l then
// asks for each of them again, one name and one round trip at a time. Handing
// them to the dcache here costs nothing extra on the wire.
void railfs_prime(struct dentry *parent, const struct railfs_attrs *a, const char *path, const struct qstr *name)
{
	DECLARE_WAIT_QUEUE_HEAD_ONSTACK(wq);
	struct dentry *dentry;
	struct dentry *alias;
	struct inode *inode;

	dentry = d_alloc_parallel(parent, name, &wq);
	if (IS_ERR(dentry)) {
		return;
	}

	// Whatever the dcache already holds for this name is either in use or was
	// made negative deliberately, and neither is a listing's business.
	if (!d_in_lookup(dentry)) {
		goto out;
	}

	inode = railfs_inode_for(parent->d_sb, a, path);
	if (inode) {
		alias = d_splice_alias(inode, dentry);
		if (!IS_ERR_OR_NULL(alias)) {
			dput(alias);
		}
	}

	d_lookup_done(dentry);
out:
	dput(dentry);
}

// Everything below a mount lives on the peer, so both of these are one List
// away. The daemon answers a whole directory at once, which is why lookup
// costs the same as readdir and neither caches yet.
int railfs_readdir(struct file *file, struct dir_context *ctx)
{
	struct railfs_conn *conn;
	struct inode *dir = file_inode(file);
	struct railfs_options *opts = dir->i_sb->s_fs_info;
	const char *here = dir->i_private ? dir->i_private : ".";
	struct railfs_dirent *entries = NULL;
	u32 count = 0;
	u32 i;
	int err = 0;

	if (!opts || !opts->pool) {
		dir_emit_dots(file, ctx);
		goto out;
	}

	// Listed before the dots are emitted: getdents reports the bytes it filled
	// and drops the error behind them, so a directory the peer has lost would
	// read as an empty one.
	conn = railfs_pool_take(opts->pool);
	err = railfs_list(conn, here, &entries, &count);
	railfs_pool_give(opts->pool, conn);
	if (err) {
		goto out;
	}

	if (!dir_emit_dots(file, ctx)) {
		goto out;
	}

	for (i = (u32)(ctx->pos - 2); ctx->pos >= 2 && i < count; i++) {
		unsigned int type = entries[i].attrs.directory ? DT_DIR : DT_REG;
		struct qstr name = QSTR_INIT(entries[i].name, strlen(entries[i].name));
		char *path = railfs_path_under(here, entries[i].name);

		if (entries[i].attrs.link) {
			type = DT_LNK;
		}

		if (!path) {
			err = -ENOMEM;
			break;
		}

		if (!railfs_name_is_usable(entries[i].name)) {
			kfree(path);
			err = -EBADMSG;
			break;
		}

		// The same number a later stat reports. Hashing the bare name here
		// gave one file two inode numbers depending on which call asked.
		if (!dir_emit(ctx, name.name, name.len, railfs_ino_of(path), type)) {
			kfree(path);
			break;
		}

		name.hash = full_name_hash(file_dentry(file), name.name, name.len);
		railfs_prime(file_dentry(file), &entries[i].attrs, path, &name);
		kfree(path);
		ctx->pos++;
	}
out:
	railfs_free_dirents(entries, count);
	return err;
}
