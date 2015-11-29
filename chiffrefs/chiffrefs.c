#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/page-flags.h>
#include <linux/highmem.h>
#include <linux/time.h>
#include <linux/string.h>
#include <linux/backing-dev.h>
#include <linux/sched.h>
#include <linux/parser.h>
#include <linux/magic.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/uio.h>

#include "internal.h"

#define CHIFFREFS_DEFAULT_ROTATE	0

static const struct super_operations chiffrefs_ops;
static const struct inode_operations chiffrefs_dir_inode_operations;

static ssize_t chiffrefs_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	printk("iov: count=%lu\n", from->count);
	return generic_file_write_iter(iocb, from);
}

static int chiffrefs_set_page_dirty(struct page *page)
{
	if (!PageReserved(page))
		SetPageDirty(page);
	return 0;
}

static const struct address_space_operations chiffrefs_aops = {
	.readpage	= simple_readpage,
	.write_begin	= simple_write_begin,
	.write_end	= simple_write_end,
	.set_page_dirty	= chiffrefs_set_page_dirty,
};

const struct file_operations chiffrefs_file_operations = {
	.read_iter	= generic_file_read_iter,
	.write_iter	= chiffrefs_file_write_iter,
	.mmap		= generic_file_mmap,
	.fsync		= noop_fsync,
	.splice_read	= generic_file_splice_read,
	.splice_write	= iter_file_splice_write,
	.llseek		= generic_file_llseek,
};

const struct inode_operations chiffrefs_file_inode_operations = {
	.setattr	= simple_setattr,
	.getattr	= simple_getattr,
};


struct inode *chiffrefs_get_inode(struct super_block *sb,
				const struct inode *dir, umode_t mode, dev_t dev)
{
	struct inode * inode = new_inode(sb);

	if (inode) {
		inode->i_ino = get_next_ino();
		inode_init_owner(inode, dir, mode);
		inode->i_mapping->a_ops = &chiffrefs_aops;
		mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
		mapping_set_unevictable(inode->i_mapping);
		inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
		switch (mode & S_IFMT) {
		default:
			init_special_inode(inode, mode, dev);
			break;
		case S_IFREG:
			inode->i_op = &chiffrefs_file_inode_operations;
			inode->i_fop = &chiffrefs_file_operations;
			break;
		case S_IFDIR:
			inode->i_op = &chiffrefs_dir_inode_operations;
			inode->i_fop = &simple_dir_operations;

			/* directory inodes start off with i_nlink == 2 (for "." entry) */
			inc_nlink(inode);
			break;
		case S_IFLNK:
			inode->i_op = &page_symlink_inode_operations;
			break;
		}
	}
	return inode;
}

/*
 * File creation. Allocate an inode, and we're done..
 */
/* SMP-safe */
static int
chiffrefs_mknod(struct inode *dir, struct dentry *dentry, umode_t mode, dev_t dev)
{
	struct inode * inode = chiffrefs_get_inode(dir->i_sb, dir, mode, dev);
	int error = -ENOSPC;

	if (inode) {
		d_instantiate(dentry, inode);
		dget(dentry);	/* Extra count - pin the dentry in core */
		error = 0;
		dir->i_mtime = dir->i_ctime = CURRENT_TIME;
	}
	return error;
}

static int chiffrefs_mkdir(struct inode * dir, struct dentry * dentry, umode_t mode)
{
	int retval = chiffrefs_mknod(dir, dentry, mode | S_IFDIR, 0);
	if (!retval)
		inc_nlink(dir);
	return retval;
}

static int chiffrefs_create(struct inode *dir, struct dentry *dentry, umode_t mode, bool excl)
{
	return chiffrefs_mknod(dir, dentry, mode | S_IFREG, 0);
}

static int chiffrefs_symlink(struct inode * dir, struct dentry *dentry, const char * symname)
{
	struct inode *inode;
	int error = -ENOSPC;

	inode = chiffrefs_get_inode(dir->i_sb, dir, S_IFLNK|S_IRWXUGO, 0);
	if (inode) {
		int l = strlen(symname)+1;
		error = page_symlink(inode, symname, l);
		if (!error) {
			d_instantiate(dentry, inode);
			dget(dentry);
			dir->i_mtime = dir->i_ctime = CURRENT_TIME;
		} else
			iput(inode);
	}
	return error;
}

static const struct inode_operations chiffrefs_dir_inode_operations = {
	.create		= chiffrefs_create,
	.lookup		= simple_lookup,
	.link		= simple_link,
	.unlink		= simple_unlink,
	.symlink	= chiffrefs_symlink,
	.mkdir		= chiffrefs_mkdir,
	.rmdir		= simple_rmdir,
	.mknod		= chiffrefs_mknod,
	.rename		= simple_rename,
};

static const struct super_operations chiffrefs_ops = {
	.statfs		= simple_statfs,
	.drop_inode	= generic_delete_inode,
	.show_options	= generic_show_options,
};

struct chiffrefs_mount_opts {
	int rotate;
};

enum {
	Opt_mode,
	Opt_err
};

static const match_table_t tokens = {
	{Opt_mode, "n=%d"},
	{Opt_err, NULL}
};

struct chiffrefs_fs_info {
	struct chiffrefs_mount_opts mount_opts;
};

static int chiffrefs_parse_options(char *data, struct chiffrefs_mount_opts *opts)
{
	substring_t args[MAX_OPT_ARGS];
	int option;
	int token;
	char *p;

	opts->rotate = CHIFFREFS_DEFAULT_ROTATE;

	while ((p = strsep(&data, ",")) != NULL) {
		if (!*p)
			continue;

		token = match_token(p, tokens, args);
		switch (token) {
		case Opt_mode:
			if (match_octal(&args[0], &option))
				return -EINVAL;
			opts->rotate = option;
			break;
		}
	}

	return 0;
}

int chiffrefs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct chiffrefs_fs_info *fsi;
	struct inode *inode;
	int err;

	save_mount_options(sb, data);

	fsi = kzalloc(sizeof(struct chiffrefs_fs_info), GFP_KERNEL);
	sb->s_fs_info = fsi;
	if (!fsi)
		return -ENOMEM;

	err = chiffrefs_parse_options(data, &fsi->mount_opts);
	if (err)
		return err;

	sb->s_maxbytes		= MAX_LFS_FILESIZE;
	sb->s_blocksize		= PAGE_CACHE_SIZE;
	sb->s_blocksize_bits	= PAGE_CACHE_SHIFT;
	sb->s_magic		= CHIFFREFS_MAGIC;
	sb->s_op		= &chiffrefs_ops;
	sb->s_time_gran		= 1;

	inode = chiffrefs_get_inode(sb, NULL, S_IFDIR | 0755, 0);
	sb->s_root = d_make_root(inode);
	if (!sb->s_root)
		return -ENOMEM;

	return 0;
}

struct dentry *chiffrefs_mount(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data)
{
	return mount_nodev(fs_type, flags, data, chiffrefs_fill_super);
}

static void chiffrefs_kill_sb(struct super_block *sb)
{
	kfree(sb->s_fs_info);
	kill_litter_super(sb);
}

static struct file_system_type chiffrefs_fs_type = {
	.name		= "chiffrefs",
	.mount		= chiffrefs_mount,
	.kill_sb	= chiffrefs_kill_sb,
	.fs_flags	= FS_USERNS_MOUNT,
};

static unsigned long once = 0;

static int __init init_chiffrefs_fs(void)
{
	if (once > 0) {
		printk("CANNOT LOAD CHIFFREFS MODULE");
		return -EBUSY;
	}
	++once;
	printk("TOTOTOTOTOTOTOT");
	return register_filesystem(&chiffrefs_fs_type);
}
fs_initcall(init_chiffrefs_fs);

static void __exit exit_chiffrefs_fs(void)
{
	--once;
	unregister_filesystem(&chiffrefs_fs_type);
}
module_exit(exit_chiffrefs_fs);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Guillaume Rybak");
MODULE_DESCRIPTION("Chiffrefs module, based on Torvald's ramfs");
