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

  // Step 1: Call readi() to get the inode using ino (inode number of current directory)

  // Step 2: Get data block of current directory from inode

  // Step 3: Read directory's data block and check each directory entry.
  //If the name matches, then copy directory entry to dirent structure

	return 0;
}

int dir_add(struct inode dir_inode, uint16_t f_ino, const char *fname, size_t name_len) {

	// Step 1: Read dir_inode's data block and check each directory entry of dir_inode
	
	// Step 2: Check if fname (directory name) is already used in other entries

	// Step 3: Add directory entry in dir_inode's data block and write to disk

	// Allocate a new data block for this directory if it does not exist

	// Update directory inode

	// Write directory entry

	return 0;
}

int dir_remove(struct inode dir_inode, const char *fname, size_t name_len) {

	// Step 1: Read dir_inode's data block and checks each directory entry of dir_inode
	
	// Step 2: Check if fname exist

	// Step 3: If exist, then remove it from dir_inode's data block and write to disk

	return 0;
}

/* 
 * namei operation
 */
int get_node_by_path(const char *path, uint16_t ino, struct inode *inode) {
	
	// Step 1: Resolve the path name, walk through path, and finally, find its inode.
	// Note: You could either implement it in a iterative way or recursive way

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

static int tfs_getattr(const char *path, struct stat *stbuf) {

	// Step 1: call get_node_by_path() to get inode from path

	// Step 2: fill attribute of file into stbuf from inode

		stbuf->st_mode   = S_IFDIR | 0755;
		stbuf->st_nlink  = 2;
		time(&stbuf->st_mtime);

	return 0;
}

static int tfs_opendir(const char *path, struct fuse_file_info *fi) {

	// Step 1: Call get_node_by_path() to get inode from path

	// Step 2: If not find, return -1

    return 0;
}

static int tfs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {

	// Step 1: Call get_node_by_path() to get inode from path

	// Step 2: Read directory entries from its data blocks, and copy them to filler

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

static int tfs_open(const char *path, struct fuse_file_info *fi) {

	// Step 1: Call get_node_by_path() to get inode from path

	// Step 2: If not find, return -1

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
