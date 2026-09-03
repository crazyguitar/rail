// SPDX-License-Identifier: GPL-2.0
//
// railfs: a filesystem mounted by mount.railfs. This is the skeleton the transport
// hangs off: it registers the type, builds a superblock and serves one file,
// so the mount path can be exercised before any of the fabric exists.

#include <linux/fs.h>
#include <linux/hash.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/pagemap.h>
#include <linux/backing-dev.h>
#include <linux/workqueue.h>
#include <linux/writeback.h>
#include <linux/statfs.h>
#include <linux/namei.h>

#include "railfs.h"
#include "railfs-rdma.h"
#include "railfs-tcp.h"
#include "railfs-trace.h"


enum railfs_param {
	Opt_host,
	Opt_export,
	Opt_port,
	Opt_conns,
	Opt_fetch,
	Opt_minfolio,
	Opt_readahead,
	Opt_block,
	Opt_flush_span,
	Opt_flush_limit,
	Opt_rdma,
	Opt_noverify,
	Opt_actimeo,
	Opt_uid,
	Opt_gid
};

static const struct fs_parameter_spec railfs_parameters[] = {
	fsparam_string("host", Opt_host),
	fsparam_string("export", Opt_export),
	fsparam_u32("port", Opt_port),
	fsparam_u32("conns", Opt_conns),
	fsparam_u32("fetch", Opt_fetch),
	fsparam_u32("minfolio", Opt_minfolio),
	fsparam_u32("readahead", Opt_readahead),
	fsparam_u32("block", Opt_block),
	fsparam_u32("flush_span", Opt_flush_span),
	fsparam_u32("flush_limit", Opt_flush_limit),
	fsparam_flag("rdma", Opt_rdma),
	fsparam_flag("noverify", Opt_noverify),
	fsparam_u32("actimeo", Opt_actimeo),
	fsparam_u32("uid", Opt_uid),
	fsparam_u32("gid", Opt_gid),
	{}
};

static u16 railfs_port_of(const struct railfs_options *opts)
{
	return opts->port ? opts->port : RAILFS_DEFAULT_PORT;
}

static void railfs_free_options(struct railfs_options *opts)
{
	if (!opts) {
		return;
	}
	railfs_pool_close(opts->pool);
	kfree(opts->host);
	kfree(opts->export);
	kfree(opts);
}

const struct inode_operations railfs_dir_inode_ops;
const struct inode_operations railfs_file_inode_ops;
const struct inode_operations railfs_link_inode_ops;
const struct file_operations railfs_dir_ops;


const struct file_operations railfs_file_ops = {
	.owner = THIS_MODULE,
	.read_iter = railfs_read_iter,
	.write_iter = railfs_write_iter,
	.open = railfs_file_open,
	.release = railfs_file_release,
	.fsync = railfs_fsync,
	.flush = railfs_file_flush,
	.llseek = generic_file_llseek,
	.mmap_prepare = generic_file_mmap_prepare,
};






// /proc/mounts has to name what was mounted, or two railfs mounts are
// indistinguishable from each other.
static int railfs_show_options(struct seq_file *m, struct dentry *root)
{
	struct railfs_options *opts = root->d_sb->s_fs_info;

	if (!opts) {
		return 0;
	}
	if (opts->host) {
		seq_printf(m, ",host=%s", opts->host);
	}
	// The root it actually serves, so a trimmed or empty option reads back as
	// the "." it became rather than as nothing at all.
	seq_printf(m, ",export=%s", railfs_export_root(opts));
	seq_printf(m, ",port=%u", railfs_port_of(opts));

	seq_printf(m, ",conns=%u", opts->conns);
	seq_printf(m, ",fetch=%u", opts->fetch);
	seq_printf(m, ",minfolio=%u", opts->minfolio);
	seq_printf(m, ",readahead=%u", opts->readahead);
	seq_printf(m, ",block=%u", opts->block);
	seq_printf(m, ",flush_span=%u", opts->flush_span);
	seq_printf(m, ",flush_limit=%u", opts->flush_limit);

	if (opts->rdma) {
		seq_puts(m, ",rdma");
	}
	if (opts->noverify) {
		seq_puts(m, ",noverify");
	}

	seq_printf(m, ",actimeo=%lu", opts->actimeo / HZ);
	seq_printf(m, ",uid=%u", from_kuid_munged(&init_user_ns, opts->uid));
	seq_printf(m, ",gid=%u", from_kgid_munged(&init_user_ns, opts->gid));
	return 0;
}


static void railfs_put_super(struct super_block *sb)
{
	pr_debug("railfs: put_super sb=%p\n", sb);
	railfs_free_options(sb->s_fs_info);
	sb->s_fs_info = NULL;
}






const struct file_operations railfs_dir_ops = {
	.owner = THIS_MODULE,
	.read = generic_read_dir,
	.iterate_shared = railfs_readdir,
	.llseek = generic_file_llseek,
};

// Creating, removing and making directories all resolve the child's path the
// same way a lookup does, then hand the peer one operation and instantiate or
// drop the dentry to match.
char *railfs_child_path(struct inode *dir, struct dentry *dentry)
{
	struct railfs_path *here = railfs_path_hold(dir);
	char *name;
	char *path;

	name = kstrndup(dentry->d_name.name, dentry->d_name.len, GFP_NOFS);
	if (!name) {
		railfs_path_put(here);
		return NULL;
	}

	path = railfs_path_under(here ? here->name : ".", name);
	kfree(name);
	railfs_path_put(here);
	return path;
}









const struct dentry_operations railfs_dentry_ops = {
	.d_revalidate = railfs_revalidate,
};





const struct inode_operations railfs_dir_inode_ops = {
	.lookup = railfs_lookup,
	.create = railfs_create,
	.unlink = railfs_unlink,
	.mkdir = railfs_mkdir_op,
	.rmdir = railfs_rmdir,
	.rename = railfs_rename,
	.symlink = railfs_symlink,
	.link = railfs_link,
	.setattr = railfs_setattr,
	.getattr = simple_getattr,
};

const struct inode_operations railfs_file_inode_ops = {
	.setattr = railfs_setattr,
	.getattr = simple_getattr,
};

const struct inode_operations railfs_link_inode_ops = {
	.get_link = railfs_get_link,
	.setattr = railfs_setattr,
	.getattr = simple_getattr,
};






static const struct super_operations railfs_super_ops = {
	.alloc_inode = railfs_alloc_inode,
	.free_inode = railfs_free_inode,
	.statfs = railfs_statfs,
	.drop_inode = generic_delete_inode,
	.show_options = railfs_show_options,
	.put_super = railfs_put_super,
	.evict_inode = railfs_evict_inode,
};

// One stat of the served directory, so the mount root reports what the peer
// holds. A peer that cannot answer leaves the mode alone rather than failing
// the mount, which would turn a slow daemon into an unmountable filesystem.
static int railfs_adopt_root_mode(struct super_block *sb, struct railfs_options *opts)
{
	struct inode *root = d_inode(sb->s_root);
	const char *served = railfs_export_root(opts);
	struct railfs_attrs attrs = {};
	bool found = false;
	int err;

	err = railfs_pool_stat(opts->pool, served, &attrs, &found);

	// An export the peer does not have is refused here. Left to the first
	// read, it looks like an empty directory instead of a wrong mount. A
	// symlink goes with it: the peer lstats, and will not list through one.
	if (err) {
		goto out;
	}

	if (!found) {
		pr_err("railfs: the peer has no %s\n", served);
		err = -ENOENT;
		goto out;
	}

	if (!attrs.directory) {
		pr_err("railfs: %s is not a directory on the peer\n", served);
		err = -ENOTDIR;
		goto out;
	}

	root->i_mode = S_IFDIR | (attrs.mode & RAILFS_MODE_BITS);

	if (attrs.mtime) {
		struct timespec64 when = { .tv_sec = attrs.mtime, .tv_nsec = 0 };

		inode_set_mtime_to_ts(root, when);
		inode_set_ctime_to_ts(root, when);
	}

	RAILFS_I(root)->mtime = attrs.mtime;
	RAILFS_I(root)->size = attrs.size;
	RAILFS_I(root)->checked = jiffies;
	err = 0;
out:
	return err;
}

const char *railfs_export_root(const struct railfs_options *opts)
{
	if (!opts->export || !opts->export[0] || opts->export[0] == '/') {
		return ".";
	}

	return opts->export;
}

static struct railfs_peer railfs_peer_of(const struct railfs_options *opts)
{
	struct railfs_peer peer = {
		.host = opts->host,
		.port = railfs_port_of(opts),
		.rdma = opts->rdma,
		.verify = !opts->noverify,
	};

	return peer;
}

static int railfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct railfs_options *opts = fc->fs_private;
	struct railfs_attrs root_attrs = { .mode = 0755, .directory = 1 };
	struct railfs_peer peer;
	struct railfs_pool *pool;
	struct inode *root;
	int err;

	sb->s_magic = RAILFS_MAGIC;
	sb->s_blocksize = PAGE_SIZE;
	sb->s_blocksize_bits = PAGE_SHIFT;
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_op = &railfs_super_ops;
	set_default_d_op(sb, &railfs_dentry_ops);
	sb->s_time_gran = 1;

	if (!opts->actimeo_set) {
		opts->actimeo = RAILFS_DEFAULT_ACTIMEO * HZ;
	}

	err = super_setup_bdi(sb);
	if (err) {
		goto out;
	}

	sb->s_bdi->ra_pages = opts->readahead / PAGE_SIZE;
	sb->s_bdi->io_pages = opts->readahead / PAGE_SIZE;

	pr_debug("railfs: fill_super enter sb=%p\n", sb);

	root = railfs_inode_for(sb, &root_attrs, railfs_export_root(opts));
	if (!root) {
		err = -ENOMEM;
		goto out;
	}

	// s_fs_info is only handed over once the pool is up, so this one inode
	// cannot read the options the way every later one does.
	root->i_uid = opts->uid;
	root->i_gid = opts->gid;

	// Nothing may be owned by the superblock until s_root exists:
	// generic_shutdown_super skips put_super without it, so anything hung off
	// the superblock before this point is never given back.
	sb->s_root = d_make_root(root);
	if (!sb->s_root) {
		err = -ENOMEM;
		goto out;
	}

	// Without one there is nobody to ask, and the mount would answer every
	// lookup from an empty root rather than say so.
	if (!opts->host) {
		pr_err("railfs: no host given; mount needs host= or a HOST:EXPORT spec\n");
		err = -EINVAL;
		goto out;
	}

	// A mount that names a host dials it now, so a daemon that is not there
	// fails the mount rather than every read after it.
	peer = railfs_peer_of(opts);
	pool = railfs_pool_open(&peer, opts->conns);
	if (IS_ERR(pool)) {
		err = PTR_ERR(pool);
		goto out;
	}

	opts->pool = pool;
	sb->s_fs_info = fc->fs_private;
	fc->fs_private = NULL;

	// Every other directory gets its mode from the lookup that found it; the
	// root has no lookup, so it costs one stat here rather than claiming a
	// mode the export does not have.
	err = railfs_adopt_root_mode(sb, opts);
	if (err) {
		goto out;
	}

	pr_debug("railfs: fill_super ok sb=%p\n", sb);
out:
	return err;
}

static int railfs_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	struct railfs_options *opts = fc->fs_private;
	struct fs_parse_result result;
	int opt = fs_parse(fc, railfs_parameters, param, &result);

	if (opt < 0) {
		return opt;
	}

	switch (opt) {
	case Opt_host:
		kfree(opts->host);
		opts->host = param->string;
		param->string = NULL;
		break;
	case Opt_export: {
		// Trimmed and judged the way rail::exportRoot does it for the fuse
		// and nfs mounts: the peer resolves a path against the directory it
		// serves and refuses an absolute one, so anything below / would name
		// the same root while looking like it named a subtree.
		size_t len = strlen(param->string);

		while (len && param->string[len - 1] == '/') {
			param->string[--len] = '\0';
		}

		if (len && param->string[0] == '/') {
			return invalfc(fc, "export is relative to what the daemon serves; use / for the whole of it");
		}

		kfree(opts->export);
		opts->export = param->string;
		param->string = NULL;
		break;
	}
	case Opt_port:
		if (result.uint_32 == 0 || result.uint_32 > U16_MAX) {
			return invalfc(fc, "port out of range");
		}

		opts->port = (u16)result.uint_32;
		break;
	case Opt_conns:
		// Refused rather than clamped, because show_options reports what it
		// was given and a mount that says conns=100 while running sixteen is
		// a lie in /proc/mounts.
		if (result.uint_32 < 1 || result.uint_32 > RAILFS_MAX_CONNS) {
			return invalfc(fc, "conns out of range");
		}

		opts->conns = result.uint_32;
		break;
	case Opt_fetch:
		if (!is_power_of_2(result.uint_32) || result.uint_32 < PAGE_SIZE || result.uint_32 > RAILFS_MAX_FETCH_BYTES) {
			return invalfc(fc, "fetch has to be a power of two between one page and the session's page");
		}

		opts->fetch = result.uint_32;
		break;
	case Opt_minfolio:
		if (!is_power_of_2(result.uint_32) || result.uint_32 < PAGE_SIZE) {
			return invalfc(fc, "minfolio has to be a power of two of at least one page");
		}

		opts->minfolio = result.uint_32;
		break;
	case Opt_readahead:
		if (!is_power_of_2(result.uint_32) || result.uint_32 < PAGE_SIZE) {
			return invalfc(fc, "readahead has to be a power of two of at least one page");
		}

		opts->readahead = result.uint_32;
		break;
	case Opt_block:
		if (!is_power_of_2(result.uint_32) || result.uint_32 < PAGE_SIZE) {
			return invalfc(fc, "block has to be a power of two of at least one page");
		}

		opts->block = result.uint_32;
		break;
	case Opt_flush_span:
		if (result.uint_32 < 1 || result.uint_32 > RAILFS_MAX_CONNS) {
			return invalfc(fc, "flush_span out of range");
		}

		opts->flush_span = result.uint_32;
		break;
	case Opt_flush_limit:
		if (result.uint_32 < 1) {
			return invalfc(fc, "flush_limit has to be at least one");
		}

		opts->flush_limit = result.uint_32;
		break;
	case Opt_rdma:
		opts->rdma = true;
		break;
	case Opt_noverify:
		opts->noverify = true;
		break;
	case Opt_actimeo:
		if (result.uint_32 > RAILFS_MAX_ACTIMEO) {
			return invalfc(fc, "actimeo out of range");
		}

		opts->actimeo = result.uint_32 * HZ;
		opts->actimeo_set = true;
		break;
	case Opt_uid:
		opts->uid = make_kuid(fc->user_ns, result.uint_32);
		if (!uid_valid(opts->uid)) {
			return invalfc(fc, "uid is not one this namespace can name");
		}
		break;
	case Opt_gid:
		opts->gid = make_kgid(fc->user_ns, result.uint_32);
		if (!gid_valid(opts->gid)) {
			return invalfc(fc, "gid is not one this namespace can name");
		}
		break;
	}
	return 0;
}

// The peer has inode numbers but does not send them, so a name is hashed into
// one. It has to be stable: a fresh number per lookup makes the same file look
// like a different one to anything that remembers.
// The daemon resolves names under the export, so every inode remembers the
// path it was found at rather than only its last component. Without it a
// lookup below the root asks about the wrong directory.
char *railfs_path_under(const char *parent, const char *name)
{
	size_t len;
	char *path;

	if (!parent || strcmp(parent, ".") == 0) {
		return kstrdup(name, GFP_NOFS);
	}

	len = strlen(parent) + 1 + strlen(name) + 1;

	path = kmalloc(len, GFP_NOFS);
	if (!path) {
		return NULL;
	}

	snprintf(path, len, "%s/%s", parent, name);
	return path;
}

unsigned long railfs_ino_of(const char *name)
{
	unsigned long h = full_name_hash(NULL, name, strlen(name));

	return h ? h : 1;
}


// kill_anon_super, not kill_litter_super: nothing here is pinned at fill_super
// time any more. Every dentry comes from lookup and belongs to the dcache, and
// d_genocide would drop references that were never taken for it.
static void railfs_kill_sb(struct super_block *sb)
{
	pr_debug("railfs: kill_sb sb=%p root=%p\n", sb, sb->s_root);
	kill_anon_super(sb);
}

static int railfs_get_tree(struct fs_context *fc)
{
	int err = get_tree_nodev(fc, railfs_fill_super);

	pr_debug("railfs: get_tree returned %d\n", err);
	return err;
}

static void railfs_free_fc(struct fs_context *fc)
{
	pr_debug("railfs: free_fc\n");
	railfs_free_options(fc->fs_private);
	fc->fs_private = NULL;
}

static const struct fs_context_operations railfs_context_ops = {
	.parse_param = railfs_parse_param,
	.get_tree = railfs_get_tree,
	.free = railfs_free_fc,
};

static int railfs_init_fs_context(struct fs_context *fc)
{
	struct railfs_options *opts = kzalloc(sizeof(*opts), GFP_KERNEL);

	if (!opts) {
		return -ENOMEM;
	}

	opts->conns = RAILFS_DEFAULT_CONNS;
	opts->fetch = RAILFS_FETCH_BYTES;
	opts->minfolio = RAILFS_MINFOLIO_BYTES;
	opts->readahead = RAILFS_READAHEAD_BYTES;
	opts->block = RAILFS_BLOCK_BYTES;
	opts->flush_span = RAILFS_FLUSH_SPAN;
	opts->flush_limit = RAILFS_FLUSH_LIMIT;

	fc->fs_private = opts;
	fc->ops = &railfs_context_ops;
	return 0;
}

static struct file_system_type railfs_type = {
	.owner = THIS_MODULE,
	.name = "railfs",
	.init_fs_context = railfs_init_fs_context,
	.kill_sb = railfs_kill_sb,
	.fs_flags = 0,
};
MODULE_ALIAS_FS("railfs");

struct kmem_cache *railfs_inode_cache;
struct workqueue_struct *railfs_page_wq;

const struct address_space_operations railfs_aops = {
	.read_folio = railfs_read_folio,
	.readahead = railfs_readahead,
	.writepages = railfs_writepages,
	.write_begin = railfs_write_begin,
	.write_end = railfs_write_end,
	.dirty_folio = filemap_dirty_folio,
};

static unsigned int railfs_inflight = RAILFS_FETCH_INFLIGHT;

// The workqueue is the whole module's, built once before any mount exists, so
// this is a parameter rather than a mount option. Writable because a workqueue
// can be resized in place, which is what makes it sweepable without a remount.
static int railfs_set_inflight(const char *val, const struct kernel_param *kp)
{
	unsigned int want;

	if (kstrtouint(val, 0, &want) || want < 1) {
		return -EINVAL;
	}

	*(unsigned int *)kp->arg = want;
	if (railfs_page_wq) {
		workqueue_set_max_active(railfs_page_wq, want);
	}
	return 0;
}

static const struct kernel_param_ops railfs_inflight_ops = {
	.set = railfs_set_inflight,
	.get = param_get_uint,
};

module_param_cb(inflight, &railfs_inflight_ops, &railfs_inflight, 0644);
MODULE_PARM_DESC(inflight, "fetches on the wire at once (default 64)");

int railfs_pages_init(void)
{
	railfs_page_wq = alloc_workqueue("railfs-pages", WQ_UNBOUND | WQ_MEM_RECLAIM, railfs_inflight);
	return railfs_page_wq ? 0 : -ENOMEM;
}

void railfs_pages_exit(void)
{
	if (railfs_page_wq) {
		destroy_workqueue(railfs_page_wq);
		railfs_page_wq = NULL;
	}
}

static int __init railfs_init(void)
{
	int err = -ENOMEM;

	railfs_trace_start();

	railfs_inode_cache = kmem_cache_create("railfs_inode", sizeof(struct railfs_inode), 0,
					     SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT, railfs_init_once);
	if (!railfs_inode_cache) {
		goto drop_trace;
	}

	err = railfs_pages_init();
	if (err) {
		goto drop_cache;
	}

	err = register_filesystem(&railfs_type);
	if (err) {
		goto drop;
	}

	// Reported, not required: a machine with no fabric still mounts over tcp.
	if (railfs_rdma_start()) {
		pr_info("railfs: no rdma devices\n");
	}

	goto out;

drop:
	destroy_workqueue(railfs_page_wq);
	railfs_page_wq = NULL;
drop_cache:
	kmem_cache_destroy(railfs_inode_cache);
	railfs_inode_cache = NULL;
drop_trace:
	railfs_trace_stop();
out:
	return err;
}

static void __exit railfs_exit(void)
{
	railfs_rdma_stop();
	unregister_filesystem(&railfs_type);
	destroy_workqueue(railfs_page_wq);

	// An inode freed through RCU is still in flight when the filesystem is
	// unregistered, and destroying the cache under it frees memory twice.
	rcu_barrier();
	kmem_cache_destroy(railfs_inode_cache);
	railfs_trace_stop();
}

module_init(railfs_init);
module_exit(railfs_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("railfs: a filesystem served over RDMA");
