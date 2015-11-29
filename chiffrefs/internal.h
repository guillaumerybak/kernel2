#ifndef __CHIFFREFS__
# define __CHIFFREFS__

# define CHIFFREFS_MAGIC 0x1337101

#define CHIFFREFS_DEFAULT_ROTATE	3

struct chiffrefs_mount_opts {
	int rotate;
};

enum {
	Opt_rotate,
	Opt_err
};

static const match_table_t tokens = {
	{Opt_rotate, "n=%d"},
	{Opt_err, NULL}
};

struct chiffrefs_fs_info {
	struct chiffrefs_mount_opts mount_opts;
};

extern const struct inode_operations chiffrefs_file_inode_operations;

#endif /* !__CHIFFREFS__ */
