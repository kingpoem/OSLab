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


static int tfs_mkdir(const char *path, mode_t mode) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target directory name

	// Step 2: Call get_node_by_path() to get inode of parent directory

	// Step 3: Call get_avail_ino() to get an available inode number

	// Step 4: Call dir_add() to add directory entry of target directory to parent directory

	// Step 5: Update inode for target directory

	// Step 6: Call writei() to write inode to disk
	

	return 0;
}

static int tfs_rmdir(const char *path) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target directory name

	// Step 2: Call get_node_by_path() to get inode of target directory

	// Step 3: Clear data block bitmap of target directory

	// Step 4: Clear inode bitmap and its data block

	// Step 5: Call get_node_by_path() to get inode of parent directory

	// Step 6: Call dir_remove() to remove directory entry of target directory in its parent directory

	return 0;
}

static int tfs_releasedir(const char *path, struct fuse_file_info *fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}

static int tfs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target file name

	// Step 2: Call get_node_by_path() to get inode of parent directory

	// Step 3: Call get_avail_ino() to get an available inode number

	// Step 4: Call dir_add() to add directory entry of target file to parent directory

	// Step 5: Update inode for target file

	// Step 6: Call writei() to write inode to disk

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

static int tfs_read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {

	// Step 1: You could call get_node_by_path() to get inode from path

	// Step 2: Based on size and offset, read its data blocks from disk

	// Step 3: copy the correct amount of data from offset to buffer

	// Note: this function should return the amount of bytes you copied to buffer
	return 0;
}

static int tfs_write(const char *path, const char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {
	// Step 1: You could call get_node_by_path() to get inode from path

	// Step 2: Based on size and offset, read its data blocks from disk

	// Step 3: Write the correct amount of data from offset to disk

	// Step 4: Update the inode info and write it to disk

	// Note: this function should return the amount of bytes you write to disk
	return size;
}

static int tfs_unlink(const char *path) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target file name

	// Step 2: Call get_node_by_path() to get inode of target file

	// Step 3: Clear data block bitmap of target file

	// Step 4: Clear inode bitmap and its data block

	// Step 5: Call get_node_by_path() to get inode of parent directory

	// Step 6: Call dir_remove() to remove directory entry of target file in its parent directory

	return 0;
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
