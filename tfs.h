#include <limits.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef _TFS_H
#define _TFS_H

#define MAGIC_NUM 0x5C3A
#define MAX_INUM 1024
#define MAX_DNUM 16384


struct superblock {
	uint32_t magic_num;       // File system magic number
	uint16_t max_inum;        // Maximum inode count
	uint16_t max_dnum;        // Maximum data block count
	uint32_t i_bitmap_blk;    // Inode bitmap block number
	uint32_t d_bitmap_blk;    // Data bitmap block number
	uint32_t i_start_blk;     // First inode table block
	uint32_t d_start_blk;     // First data region block
};

struct inode {
	uint16_t ino;             // Inode number
	uint16_t valid;           // Inode validity flag
	uint32_t size;            // File size in bytes
	uint32_t type;            // File type bits
	uint32_t link;            // Link count
	int direct_ptr[16];       // Direct data block numbers
	int indirect_ptr[8];      // Single-indirect index block numbers
	struct stat vstat;        // POSIX file attributes
};

struct dirent {
	uint16_t ino;             // Referenced inode number
	uint16_t valid;           // Directory entry validity flag
	char name[252];           // Null-terminated entry name
};


/*
 * bitmap operations
 */
typedef unsigned char* bitmap_t;

/* Set one resource bit in a bitmap. */
static inline void set_bitmap(bitmap_t b, int i) {
	    b[i / 8] |= 1 << (i & 7);
}

/* Clear one resource bit in a bitmap. */
static inline void unset_bitmap(bitmap_t b, int i) {
	    b[i / 8] &= ~(1 << (i & 7));
}

/* Return whether one resource bit is set. */
static inline uint8_t get_bitmap(bitmap_t b, int i) {
	    return b[i / 8] & (1 << (i & 7)) ? 1 : 0;
}

#endif
