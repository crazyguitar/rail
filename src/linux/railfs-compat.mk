ccflags-y += $(shell grep -q inode_set_ctime_to_ts $(srctree)/include/linux/fs.h && echo -DRAILFS_HAS_CTIME_ACCESSORS=1)
