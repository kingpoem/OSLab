#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/time.h>
#include <time.h>
#include <libgen.h>
#include <limits.h>
#include <stdbool.h>

#include "block.h"
#include "tfs.h"

char diskfile_path[PATH_MAX];

static struct superblock superblock;

int get_avail_ino(void);
int get_avail_blkno(void);
int readi(uint16_t ino, struct inode *inode);
int writei(uint16_t ino, struct inode *inode);

/* Return the number of inodes stored in one inode table block. */
static uint32_t inodes_per_block(void) {
	return BLOCK_SIZE / sizeof(struct inode);
}

/* Return the number of blocks reserved for the inode table. */
static uint32_t inode_table_blocks(void) {
	uint32_t per_block = inodes_per_block();

	return (MAX_INUM + per_block - 1) / per_block;
}

/* Clear an allocated inode bit. */
static int release_ino(uint16_t ino) {
	unsigned char bitmap[BLOCK_SIZE];

	if (ino >= superblock.max_inum)
		return -EINVAL;
	if (bio_read(superblock.i_bitmap_blk, bitmap) != BLOCK_SIZE)
		return -EIO;
	unset_bitmap(bitmap, ino);
	if (bio_write(superblock.i_bitmap_blk, bitmap) != BLOCK_SIZE)
		return -EIO;
	return 0;
}

/* Clear an allocated data block bit. */
static int release_blkno(int block_no) {
	unsigned char bitmap[BLOCK_SIZE];
	int index = block_no - (int)superblock.d_start_blk;

	if (index < 0 || index >= superblock.max_dnum)
		return -EINVAL;
	if (bio_read(superblock.d_bitmap_blk, bitmap) != BLOCK_SIZE)
		return -EIO;
	unset_bitmap(bitmap, index);
	if (bio_write(superblock.d_bitmap_blk, bitmap) != BLOCK_SIZE)
		return -EIO;
	return 0;
}

/* Split an absolute path into a parent path and one final name. */
static int split_parent_path(const char *path, char *parent, char *name) {
	char copy[PATH_MAX];
	char *last_slash;
	size_t length;

	if (path == NULL || parent == NULL || name == NULL || path[0] != '/')
		return -EINVAL;
	length = strnlen(path, sizeof(copy));
	if (length == 0 || length >= sizeof(copy))
		return -ENAMETOOLONG;
	strcpy(copy, path);
	while (length > 1 && copy[length - 1] == '/')
		copy[--length] = '\0';
	if (strcmp(copy, "/") == 0)
		return -EINVAL;

	last_slash = strrchr(copy, '/');
	if (last_slash == NULL || last_slash[1] == '\0')
		return -EINVAL;
	if (strlen(last_slash + 1) >= sizeof(((struct dirent *)0)->name))
		return -ENAMETOOLONG;
	strcpy(name, last_slash + 1);
	if (last_slash == copy) {
		strcpy(parent, "/");
	} else {
		*last_slash = '\0';
		strcpy(parent, copy);
	}
	return 0;
}

/* Initialize a new in-memory inode with no allocated blocks. */
static void initialize_inode(struct inode *inode, uint16_t ino, mode_t mode) {
	int i;
	time_t now = time(NULL);

	memset(inode, 0, sizeof(*inode));
	inode->ino = ino;
	inode->valid = 1;
	inode->type = mode & S_IFMT;
	inode->link = S_ISDIR(mode) ? 2 : 1;
	for (i = 0; i < 16; i++)
		inode->direct_ptr[i] = -1;
	for (i = 0; i < 8; i++)
		inode->indirect_ptr[i] = -1;
	inode->vstat.st_ino = ino;
	inode->vstat.st_mode = mode;
	inode->vstat.st_nlink = inode->link;
	inode->vstat.st_uid = getuid();
	inode->vstat.st_gid = getgid();
	inode->vstat.st_blksize = BLOCK_SIZE;
	inode->vstat.st_atime = now;
	inode->vstat.st_mtime = now;
	inode->vstat.st_ctime = now;
}

/* Return whether a directory has no valid child entries. */
static int directory_is_empty(const struct inode *inode) {
	struct dirent entries[BLOCK_SIZE / sizeof(struct dirent)];
	int block_index;
	size_t entry_index;

	if (inode == NULL || !S_ISDIR(inode->vstat.st_mode))
		return -ENOTDIR;
	for (block_index = 0; block_index < 16; block_index++) {
		if (inode->direct_ptr[block_index] < 0)
			continue;
		if (bio_read(inode->direct_ptr[block_index], entries) != BLOCK_SIZE)
			return -EIO;
		for (entry_index = 0;
		     entry_index < BLOCK_SIZE / sizeof(struct dirent);
		     entry_index++) {
			if (entries[entry_index].valid)
				return 0;
		}
	}
	return 1;
}

/* Release direct blocks and clear an inode from disk and bitmap. */
static int delete_inode(struct inode *inode) {
	struct inode cleared;
	int block_index;

	if (inode == NULL || inode->ino == 0)
		return -EINVAL;
	for (block_index = 0; block_index < 16; block_index++) {
		if (inode->direct_ptr[block_index] < 0)
			continue;
		if (release_blkno(inode->direct_ptr[block_index]) < 0)
			return -EIO;
	}
	memset(&cleared, 0, sizeof(cleared));
	if (writei(inode->ino, &cleared) < 0)
		return -EIO;
	return release_ino(inode->ino);
}

/* Return one direct file block and allocate it when requested. */
static int get_file_block(struct inode *inode, uint32_t logical_block,
			  bool allocate) {
	char zero_block[BLOCK_SIZE];
	int block_no;

	if (inode == NULL)
		return -EINVAL;
	if (logical_block >= 16)
		return -EFBIG;
	if (inode->direct_ptr[logical_block] >= 0)
		return inode->direct_ptr[logical_block];
	if (!allocate)
		return -ENOENT;

	block_no = get_avail_blkno();
	if (block_no < 0)
		return block_no;
	memset(zero_block, 0, sizeof(zero_block));
	if (bio_write(block_no, zero_block) != BLOCK_SIZE) {
		release_blkno(block_no);
		return -EIO;
	}
	inode->direct_ptr[logical_block] = block_no;
	inode->vstat.st_blocks += BLOCK_SIZE / 512;
	return block_no;
}

/* 
 * Get available inode number from bitmap
 */
int get_avail_ino() {
	unsigned char bitmap[BLOCK_SIZE];
	int ino;

	/* Allocate the first free inode recorded in the inode bitmap. */
	if (bio_read(superblock.i_bitmap_blk, bitmap) != BLOCK_SIZE)
		return -EIO;

	for (ino = 0; ino < superblock.max_inum; ino++) {
		if (get_bitmap(bitmap, ino))
			continue;
		set_bitmap(bitmap, ino);
		if (bio_write(superblock.i_bitmap_blk, bitmap) != BLOCK_SIZE)
			return -EIO;
		return ino;
	}

	return -ENOSPC;
}

/* 
 * Get available data block number from bitmap
 */
int get_avail_blkno() {
	unsigned char bitmap[BLOCK_SIZE];
	int index;

	/* Allocate the first free block recorded in the data bitmap. */
	if (bio_read(superblock.d_bitmap_blk, bitmap) != BLOCK_SIZE)
		return -EIO;

	for (index = 0; index < superblock.max_dnum; index++) {
		if (get_bitmap(bitmap, index))
			continue;
		set_bitmap(bitmap, index);
		if (bio_write(superblock.d_bitmap_blk, bitmap) != BLOCK_SIZE)
			return -EIO;
		return (int)superblock.d_start_blk + index;
	}

	return -ENOSPC;
}

/* 
 * inode operations
 */
int readi(uint16_t ino, struct inode *inode) {
	char block[BLOCK_SIZE];
	uint32_t per_block = inodes_per_block();
	uint32_t block_no;
	uint32_t offset;

	/* Read one inode from its fixed slot in the inode table. */
	if (inode == NULL || ino >= superblock.max_inum || per_block == 0)
		return -EINVAL;
	block_no = superblock.i_start_blk + ino / per_block;
	offset = (ino % per_block) * sizeof(struct inode);
	if (bio_read(block_no, block) != BLOCK_SIZE)
		return -EIO;
	memcpy(inode, block + offset, sizeof(struct inode));
	return 0;
}

int writei(uint16_t ino, struct inode *inode) {
	char block[BLOCK_SIZE];
	uint32_t per_block = inodes_per_block();
	uint32_t block_no;
	uint32_t offset;

	/* Update one inode in its fixed slot without changing nearby inodes. */
	if (inode == NULL || ino >= superblock.max_inum || per_block == 0)
		return -EINVAL;
	block_no = superblock.i_start_blk + ino / per_block;
	offset = (ino % per_block) * sizeof(struct inode);
	if (bio_read(block_no, block) != BLOCK_SIZE)
		return -EIO;
	memcpy(block + offset, inode, sizeof(struct inode));
	if (bio_write(block_no, block) != BLOCK_SIZE)
		return -EIO;
	return 0;
}


/* 
 * directory operations
 */
int dir_find(uint16_t ino, const char *fname, size_t name_len, struct dirent *dirent) {
	struct inode dir_inode;
	struct dirent entries[BLOCK_SIZE / sizeof(struct dirent)];
	int block_index;
	size_t entry_index;

	/* Find a named entry in the direct blocks of one directory. */
	if (fname == NULL || dirent == NULL || name_len == 0 ||
	    name_len >= sizeof(entries[0].name))
		return -EINVAL;
	if (readi(ino, &dir_inode) < 0 || !dir_inode.valid)
		return -ENOENT;
	if (!S_ISDIR(dir_inode.vstat.st_mode))
		return -ENOTDIR;

	for (block_index = 0; block_index < 16; block_index++) {
		if (dir_inode.direct_ptr[block_index] < 0)
			continue;
		if (bio_read(dir_inode.direct_ptr[block_index], entries) != BLOCK_SIZE)
			return -EIO;
		for (entry_index = 0;
		     entry_index < BLOCK_SIZE / sizeof(struct dirent);
		     entry_index++) {
			if (!entries[entry_index].valid)
				continue;
			if (strnlen(entries[entry_index].name,
				    sizeof(entries[entry_index].name)) != name_len)
				continue;
			if (memcmp(entries[entry_index].name, fname, name_len) == 0) {
				*dirent = entries[entry_index];
				return 0;
			}
		}
	}

	return -ENOENT;
}

int dir_add(struct inode dir_inode, uint16_t f_ino, const char *fname, size_t name_len) {
	struct dirent entries[BLOCK_SIZE / sizeof(struct dirent)];
	struct dirent existing;
	int block_index;
	int free_block_index = -1;
	int new_block = -1;
	int find_result;
	size_t entry_index;
	time_t now;

	/* Add one unique name to a directory, allocating a block if needed. */
	if (!dir_inode.valid || !S_ISDIR(dir_inode.vstat.st_mode))
		return -ENOTDIR;
	if (fname == NULL || name_len == 0 ||
	    name_len >= sizeof(entries[0].name) ||
	    memchr(fname, '/', name_len) != NULL)
		return -EINVAL;
	find_result = dir_find(dir_inode.ino, fname, name_len, &existing);
	if (find_result == 0)
		return -EEXIST;
	if (find_result != -ENOENT)
		return find_result;

	for (block_index = 0; block_index < 16; block_index++) {
		if (dir_inode.direct_ptr[block_index] < 0) {
			if (free_block_index < 0)
				free_block_index = block_index;
			continue;
		}
		if (bio_read(dir_inode.direct_ptr[block_index], entries) != BLOCK_SIZE)
			return -EIO;
		for (entry_index = 0;
		     entry_index < BLOCK_SIZE / sizeof(struct dirent);
		     entry_index++) {
			if (entries[entry_index].valid)
				continue;
			entries[entry_index].ino = f_ino;
			entries[entry_index].valid = 1;
			memset(entries[entry_index].name, 0,
			       sizeof(entries[entry_index].name));
			memcpy(entries[entry_index].name, fname, name_len);
			if (bio_write(dir_inode.direct_ptr[block_index], entries) !=
			    BLOCK_SIZE)
				return -EIO;
			goto update_inode;
		}
	}

	if (free_block_index < 0)
		return -ENOSPC;
	new_block = get_avail_blkno();
	if (new_block < 0)
		return new_block;
	memset(entries, 0, sizeof(entries));
	entries[0].ino = f_ino;
	entries[0].valid = 1;
	memcpy(entries[0].name, fname, name_len);
	if (bio_write(new_block, entries) != BLOCK_SIZE) {
		release_blkno(new_block);
		return -EIO;
	}
	dir_inode.direct_ptr[free_block_index] = new_block;
	dir_inode.vstat.st_blocks += BLOCK_SIZE / 512;

update_inode:
	dir_inode.size += sizeof(struct dirent);
	dir_inode.vstat.st_size = dir_inode.size;
	now = time(NULL);
	dir_inode.vstat.st_mtime = now;
	dir_inode.vstat.st_ctime = now;
	if (writei(dir_inode.ino, &dir_inode) < 0) {
		if (new_block >= 0)
			release_blkno(new_block);
		return -EIO;
	}
	return 0;
}

int dir_remove(struct inode dir_inode, const char *fname, size_t name_len) {
	struct dirent entries[BLOCK_SIZE / sizeof(struct dirent)];
	int block_index;
	size_t entry_index;
	time_t now;

	/* Remove one named entry and release its directory block if empty. */
	if (!dir_inode.valid || !S_ISDIR(dir_inode.vstat.st_mode))
		return -ENOTDIR;
	if (fname == NULL || name_len == 0 ||
	    name_len >= sizeof(entries[0].name))
		return -EINVAL;

	for (block_index = 0; block_index < 16; block_index++) {
		bool block_empty = true;
		bool found = false;

		if (dir_inode.direct_ptr[block_index] < 0)
			continue;
		if (bio_read(dir_inode.direct_ptr[block_index], entries) != BLOCK_SIZE)
			return -EIO;
		for (entry_index = 0;
		     entry_index < BLOCK_SIZE / sizeof(struct dirent);
		     entry_index++) {
			if (!entries[entry_index].valid)
				continue;
			if (strnlen(entries[entry_index].name,
				    sizeof(entries[entry_index].name)) == name_len &&
			    memcmp(entries[entry_index].name, fname, name_len) == 0) {
				memset(&entries[entry_index], 0,
				       sizeof(entries[entry_index]));
				found = true;
				break;
			}
		}
		if (!found)
			continue;

		for (entry_index = 0;
		     entry_index < BLOCK_SIZE / sizeof(struct dirent);
		     entry_index++) {
			if (entries[entry_index].valid) {
				block_empty = false;
				break;
			}
		}

		if (block_empty) {
			if (release_blkno(dir_inode.direct_ptr[block_index]) < 0)
				return -EIO;
			dir_inode.direct_ptr[block_index] = -1;
			dir_inode.vstat.st_blocks -= BLOCK_SIZE / 512;
		} else if (bio_write(dir_inode.direct_ptr[block_index], entries) !=
			   BLOCK_SIZE) {
			return -EIO;
		}

		dir_inode.size -= sizeof(struct dirent);
		dir_inode.vstat.st_size = dir_inode.size;
		now = time(NULL);
		dir_inode.vstat.st_mtime = now;
		dir_inode.vstat.st_ctime = now;
		return writei(dir_inode.ino, &dir_inode);
	}

	return -ENOENT;
}

/* 
 * namei operation
 */
int get_node_by_path(const char *path, uint16_t ino, struct inode *inode) {
	char path_copy[PATH_MAX];
	char *component;
	char *saveptr = NULL;
	struct inode current;

	/* Resolve an absolute path one directory component at a time. */
	if (path == NULL || inode == NULL || path[0] != '/')
		return -EINVAL;
	if (strnlen(path, sizeof(path_copy)) >= sizeof(path_copy))
		return -ENAMETOOLONG;
	if (readi(ino, &current) < 0 || !current.valid)
		return -ENOENT;
	if (strcmp(path, "/") == 0) {
		*inode = current;
		return 0;
	}

	strcpy(path_copy, path);
	component = strtok_r(path_copy, "/", &saveptr);
	while (component != NULL) {
		struct dirent entry;
		size_t name_len = strlen(component);

		if (!S_ISDIR(current.vstat.st_mode))
			return -ENOTDIR;
		if (name_len == 0 || name_len >= sizeof(entry.name))
			return -ENAMETOOLONG;
		if (dir_find(current.ino, component, name_len, &entry) < 0)
			return -ENOENT;
		if (readi(entry.ino, &current) < 0 || !current.valid)
			return -ENOENT;
		component = strtok_r(NULL, "/", &saveptr);
	}

	*inode = current;
	return 0;
}

/* 
 * Make file system
 */
int tfs_mkfs() {
	char block[BLOCK_SIZE];
	struct inode root_inode;
	int root_ino;
	int i;

	/* Create a clean disk and write the fixed on-disk layout. */
	dev_init(diskfile_path);
	memset(&superblock, 0, sizeof(superblock));
	superblock.magic_num = MAGIC_NUM;
	superblock.max_inum = MAX_INUM;
	superblock.max_dnum = MAX_DNUM;
	superblock.i_bitmap_blk = 1;
	superblock.d_bitmap_blk = 2;
	superblock.i_start_blk = 3;
	superblock.d_start_blk = superblock.i_start_blk + inode_table_blocks();

	memset(block, 0, sizeof(block));
	memcpy(block, &superblock, sizeof(superblock));
	if (bio_write(0, block) != BLOCK_SIZE)
		return -EIO;

	memset(block, 0, sizeof(block));
	if (bio_write(superblock.i_bitmap_blk, block) != BLOCK_SIZE)
		return -EIO;
	if (bio_write(superblock.d_bitmap_blk, block) != BLOCK_SIZE)
		return -EIO;

	root_ino = get_avail_ino();
	if (root_ino != 0)
		return root_ino < 0 ? root_ino : -EIO;

	memset(&root_inode, 0, sizeof(root_inode));
	root_inode.ino = root_ino;
	root_inode.valid = 1;
	root_inode.type = S_IFDIR;
	root_inode.link = 2;
	for (i = 0; i < 16; i++)
		root_inode.direct_ptr[i] = -1;
	for (i = 0; i < 8; i++)
		root_inode.indirect_ptr[i] = -1;
	root_inode.vstat.st_ino = root_ino;
	root_inode.vstat.st_mode = S_IFDIR | 0755;
	root_inode.vstat.st_nlink = 2;
	root_inode.vstat.st_uid = getuid();
	root_inode.vstat.st_gid = getgid();
	root_inode.vstat.st_blksize = BLOCK_SIZE;
	root_inode.vstat.st_atime = time(NULL);
	root_inode.vstat.st_mtime = root_inode.vstat.st_atime;
	root_inode.vstat.st_ctime = root_inode.vstat.st_atime;
	if (writei(root_ino, &root_inode) < 0) {
		release_ino(root_ino);
		return -EIO;
	}

	return 0;
}


/* 
 * FUSE file operations
 */
static void *tfs_init(struct fuse_conn_info *conn) {
	char block[BLOCK_SIZE];

	/* Open an existing file system or create a new formatted disk. */
	(void)conn;
	if (access(diskfile_path, F_OK) != 0) {
		if (tfs_mkfs() < 0)
			return NULL;
	} else if (dev_open(diskfile_path) < 0) {
		return NULL;
	}

	if (bio_read(0, block) != BLOCK_SIZE)
		return NULL;
	memcpy(&superblock, block, sizeof(superblock));
	if (superblock.magic_num != MAGIC_NUM) {
		fprintf(stderr, "Invalid TFS disk image\n");
		dev_close();
		return NULL;
	}

	return NULL;
}

static void tfs_destroy(void *userdata) {
	/* Close the disk when FUSE unmounts the file system. */
	(void)userdata;
	dev_close();
}

/* Fill POSIX attributes for a path resolved by TFS. */
static int tfs_getattr(const char *path, struct stat *stbuf) {
	struct inode inode;
	int result;

	if (stbuf == NULL)
		return -EINVAL;
	result = get_node_by_path(path, 0, &inode);
	if (result < 0)
		return result;
	memset(stbuf, 0, sizeof(*stbuf));
	*stbuf = inode.vstat;
	stbuf->st_ino = inode.ino;
	stbuf->st_size = inode.size;
	stbuf->st_nlink = inode.link;
	return 0;
}

/* Verify that a path exists and refers to a directory. */
static int tfs_opendir(const char *path, struct fuse_file_info *fi) {
	struct inode inode;
	int result;

	(void)fi;
	result = get_node_by_path(path, 0, &inode);
	if (result < 0)
		return result;
	if (!S_ISDIR(inode.vstat.st_mode))
		return -ENOTDIR;
	return 0;
}

/* Enumerate valid entries from every direct block of a directory. */
static int tfs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
	struct inode inode;
	struct dirent entries[BLOCK_SIZE / sizeof(struct dirent)];
	int block_index;
	size_t entry_index;
	int result;

	(void)offset;
	(void)fi;
	if (buffer == NULL || filler == NULL)
		return -EINVAL;
	result = get_node_by_path(path, 0, &inode);
	if (result < 0)
		return result;
	if (!S_ISDIR(inode.vstat.st_mode))
		return -ENOTDIR;

	if (filler(buffer, ".", NULL, 0) != 0)
		return 0;
	if (filler(buffer, "..", NULL, 0) != 0)
		return 0;

	for (block_index = 0; block_index < 16; block_index++) {
		if (inode.direct_ptr[block_index] < 0)
			continue;
		if (bio_read(inode.direct_ptr[block_index], entries) != BLOCK_SIZE)
			return -EIO;
		for (entry_index = 0;
		     entry_index < BLOCK_SIZE / sizeof(struct dirent);
		     entry_index++) {
			if (!entries[entry_index].valid)
				continue;
			if (filler(buffer, entries[entry_index].name, NULL, 0) != 0)
				return 0;
		}
	}

	return 0;
}


/* Create one empty directory and link it into its parent. */
static int tfs_mkdir(const char *path, mode_t mode) {
	char parent_path[PATH_MAX];
	char name[sizeof(((struct dirent *)0)->name)];
	struct inode parent_inode;
	struct inode new_inode;
	struct dirent existing;
	int new_ino;
	int result;

	result = split_parent_path(path, parent_path, name);
	if (result < 0)
		return result;
	result = get_node_by_path(parent_path, 0, &parent_inode);
	if (result < 0)
		return result;
	if (!S_ISDIR(parent_inode.vstat.st_mode))
		return -ENOTDIR;
	if (dir_find(parent_inode.ino, name, strlen(name), &existing) == 0)
		return -EEXIST;

	new_ino = get_avail_ino();
	if (new_ino < 0)
		return new_ino;
	initialize_inode(&new_inode, new_ino, S_IFDIR | (mode & 07777));
	if (writei(new_ino, &new_inode) < 0) {
		release_ino(new_ino);
		return -EIO;
	}
	result = dir_add(parent_inode, new_ino, name, strlen(name));
	if (result < 0) {
		delete_inode(&new_inode);
		return result;
	}

	if (readi(parent_inode.ino, &parent_inode) == 0) {
		parent_inode.link++;
		parent_inode.vstat.st_nlink = parent_inode.link;
		parent_inode.vstat.st_ctime = time(NULL);
		writei(parent_inode.ino, &parent_inode);
	}
	return 0;
}

/* Remove one empty directory and reclaim its inode. */
static int tfs_rmdir(const char *path) {
	char parent_path[PATH_MAX];
	char name[sizeof(((struct dirent *)0)->name)];
	struct inode parent_inode;
	struct inode target_inode;
	int result;

	result = split_parent_path(path, parent_path, name);
	if (result < 0)
		return result;
	result = get_node_by_path(path, 0, &target_inode);
	if (result < 0)
		return result;
	if (!S_ISDIR(target_inode.vstat.st_mode))
		return -ENOTDIR;
	result = directory_is_empty(&target_inode);
	if (result < 0)
		return result;
	if (result == 0)
		return -ENOTEMPTY;
	result = get_node_by_path(parent_path, 0, &parent_inode);
	if (result < 0)
		return result;
	result = dir_remove(parent_inode, name, strlen(name));
	if (result < 0)
		return result;
	result = delete_inode(&target_inode);
	if (result < 0)
		return result;

	if (readi(parent_inode.ino, &parent_inode) == 0 &&
	    parent_inode.link > 2) {
		parent_inode.link--;
		parent_inode.vstat.st_nlink = parent_inode.link;
		parent_inode.vstat.st_ctime = time(NULL);
		writei(parent_inode.ino, &parent_inode);
	}
	return 0;
}

static int tfs_releasedir(const char *path, struct fuse_file_info *fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}

/* Create one empty regular file and link it into its parent. */
static int tfs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
	char parent_path[PATH_MAX];
	char name[sizeof(((struct dirent *)0)->name)];
	struct inode parent_inode;
	struct inode new_inode;
	struct dirent existing;
	int new_ino;
	int result;

	(void)fi;
	result = split_parent_path(path, parent_path, name);
	if (result < 0)
		return result;
	result = get_node_by_path(parent_path, 0, &parent_inode);
	if (result < 0)
		return result;
	if (!S_ISDIR(parent_inode.vstat.st_mode))
		return -ENOTDIR;
	if (dir_find(parent_inode.ino, name, strlen(name), &existing) == 0)
		return -EEXIST;

	new_ino = get_avail_ino();
	if (new_ino < 0)
		return new_ino;
	initialize_inode(&new_inode, new_ino, S_IFREG | (mode & 07777));
	if (writei(new_ino, &new_inode) < 0) {
		release_ino(new_ino);
		return -EIO;
	}
	result = dir_add(parent_inode, new_ino, name, strlen(name));
	if (result < 0) {
		delete_inode(&new_inode);
		return result;
	}
	return 0;
}

/* Verify that a path exists and refers to a regular file. */
static int tfs_open(const char *path, struct fuse_file_info *fi) {
	struct inode inode;
	int result;

	(void)fi;
	result = get_node_by_path(path, 0, &inode);
	if (result < 0)
		return result;
	if (S_ISDIR(inode.vstat.st_mode))
		return -EISDIR;
	return 0;
}

/* Read regular-file data across direct blocks with offset support. */
static int tfs_read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {
	struct inode inode;
	char block[BLOCK_SIZE];
	size_t completed = 0;
	size_t available;
	int result;

	(void)fi;
	if (buffer == NULL || offset < 0)
		return -EINVAL;
	result = get_node_by_path(path, 0, &inode);
	if (result < 0)
		return result;
	if (!S_ISREG(inode.vstat.st_mode))
		return -EISDIR;
	if ((uint64_t)offset >= inode.size || size == 0)
		return 0;

	available = inode.size - (size_t)offset;
	if (size > available)
		size = available;
	while (completed < size) {
		uint64_t position = (uint64_t)offset + completed;
		uint32_t logical_block = position / BLOCK_SIZE;
		size_t block_offset = position % BLOCK_SIZE;
		size_t chunk = BLOCK_SIZE - block_offset;
		int block_no;

		if (chunk > size - completed)
			chunk = size - completed;
		block_no = get_file_block(&inode, logical_block, false);
		if (block_no == -ENOENT) {
			memset(buffer + completed, 0, chunk);
		} else if (block_no < 0) {
			return completed > 0 ? (int)completed : block_no;
		} else {
			if (bio_read(block_no, block) != BLOCK_SIZE)
				return completed > 0 ? (int)completed : -EIO;
			memcpy(buffer + completed, block + block_offset, chunk);
		}
		completed += chunk;
	}

	inode.vstat.st_atime = time(NULL);
	writei(inode.ino, &inode);
	return (int)completed;
}

/* Write regular-file data across direct blocks with offset support. */
static int tfs_write(const char *path, const char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {
	struct inode inode;
	char block[BLOCK_SIZE];
	size_t completed = 0;
	uint64_t end_position;
	int result;

	(void)fi;
	if (buffer == NULL || offset < 0)
		return -EINVAL;
	if (size == 0)
		return 0;
	if ((uint64_t)offset > UINT32_MAX ||
	    size > UINT32_MAX - (uint64_t)offset)
		return -EFBIG;
	end_position = (uint64_t)offset + size;
	if (end_position > (uint64_t)16 * BLOCK_SIZE)
		return -EFBIG;

	result = get_node_by_path(path, 0, &inode);
	if (result < 0)
		return result;
	if (!S_ISREG(inode.vstat.st_mode))
		return -EISDIR;

	while (completed < size) {
		uint64_t position = (uint64_t)offset + completed;
		uint32_t logical_block = position / BLOCK_SIZE;
		size_t block_offset = position % BLOCK_SIZE;
		size_t chunk = BLOCK_SIZE - block_offset;
		int block_no;

		if (chunk > size - completed)
			chunk = size - completed;
		block_no = get_file_block(&inode, logical_block, true);
		if (block_no < 0)
			break;
		if (bio_read(block_no, block) != BLOCK_SIZE)
			break;
		memcpy(block + block_offset, buffer + completed, chunk);
		if (bio_write(block_no, block) != BLOCK_SIZE)
			break;
		completed += chunk;
	}

	if (completed == 0)
		return -EIO;
	if ((uint64_t)offset + completed > inode.size)
		inode.size = offset + completed;
	inode.vstat.st_size = inode.size;
	inode.vstat.st_mtime = time(NULL);
	inode.vstat.st_ctime = inode.vstat.st_mtime;
	if (writei(inode.ino, &inode) < 0)
		return -EIO;
	return (int)completed;
}

/* Remove one regular file and reclaim its inode and direct blocks. */
static int tfs_unlink(const char *path) {
	char parent_path[PATH_MAX];
	char name[sizeof(((struct dirent *)0)->name)];
	struct inode parent_inode;
	struct inode target_inode;
	int result;

	result = split_parent_path(path, parent_path, name);
	if (result < 0)
		return result;
	result = get_node_by_path(path, 0, &target_inode);
	if (result < 0)
		return result;
	if (S_ISDIR(target_inode.vstat.st_mode))
		return -EISDIR;
	result = get_node_by_path(parent_path, 0, &parent_inode);
	if (result < 0)
		return result;
	result = dir_remove(parent_inode, name, strlen(name));
	if (result < 0)
		return result;
	return delete_inode(&target_inode);
}

static int tfs_truncate(const char *path, off_t size) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}

static int tfs_release(const char *path, struct fuse_file_info *fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int tfs_flush(const char * path, struct fuse_file_info * fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}

static int tfs_utimens(const char *path, const struct timespec tv[2]) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}


static struct fuse_operations tfs_ope = {
	.init		= tfs_init,
	.destroy	= tfs_destroy,

	.getattr	= tfs_getattr,
	.readdir	= tfs_readdir,
	.opendir	= tfs_opendir,
	.releasedir	= tfs_releasedir,
	.mkdir		= tfs_mkdir,
	.rmdir		= tfs_rmdir,

	.create		= tfs_create,
	.open		= tfs_open,
	.read 		= tfs_read,
	.write		= tfs_write,
	.unlink		= tfs_unlink,

	.truncate   = tfs_truncate,
	.flush      = tfs_flush,
	.utimens    = tfs_utimens,
	.release	= tfs_release
};


int main(int argc, char *argv[]) {
	int fuse_stat;

	getcwd(diskfile_path, PATH_MAX);
	strcat(diskfile_path, "/DISKFILE");

	fuse_stat = fuse_main(argc, argv, &tfs_ope, NULL);

	return fuse_stat;
}
