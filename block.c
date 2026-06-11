#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "block.h"

//Disk size set to 32MB
#define DISK_SIZE	32*1024*1024

int diskfile = -1;

/* Create and size the flat file used as the emulated disk. */
void dev_init(const char* diskfile_path) {
    if (diskfile >= 0) {
		return;
    }
    
    diskfile = open(diskfile_path, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
    if (diskfile < 0) {
		perror("disk_open failed");
		exit(EXIT_FAILURE);
    }
	
	    if (ftruncate(diskfile, DISK_SIZE) < 0) {
			perror("disk resize failed");
			close(diskfile);
			diskfile = -1;
			exit(EXIT_FAILURE);
	    }
}

/* Open an existing flat file used as the emulated disk. */
int dev_open(const char* diskfile_path) {
    if (diskfile >= 0) {
		return 0;
    }
    
    diskfile = open(diskfile_path, O_RDWR, S_IRUSR | S_IWUSR);
    if (diskfile < 0) {
		perror("disk_open failed");
		return -1;
    }
	return 0;
}

/* Close the emulated disk file. */
void dev_close() {
	    if (diskfile >= 0) {
			close(diskfile);
			diskfile = -1;
	    }
}

/* Read exactly one block from the emulated disk. */
int bio_read(const int block_num, void *buf) {
	ssize_t completed = 0;

	if (diskfile < 0 || block_num < 0 || buf == NULL) {
		errno = EINVAL;
		return -1;
	}

	while (completed < BLOCK_SIZE) {
		ssize_t ret = pread(diskfile, (char *)buf + completed,
				    BLOCK_SIZE - completed,
				    (off_t)block_num * BLOCK_SIZE + completed);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			perror("block_read failed");
			return -1;
		}
		if (ret == 0) {
			memset((char *)buf + completed, 0, BLOCK_SIZE - completed);
			break;
		}
		completed += ret;
	}

	return BLOCK_SIZE;
}

/* Write exactly one block to the emulated disk. */
int bio_write(const int block_num, const void *buf) {
	ssize_t completed = 0;

	if (diskfile < 0 || block_num < 0 || buf == NULL) {
		errno = EINVAL;
		return -1;
	}

	while (completed < BLOCK_SIZE) {
		ssize_t ret = pwrite(diskfile, (const char *)buf + completed,
				     BLOCK_SIZE - completed,
				     (off_t)block_num * BLOCK_SIZE + completed);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			perror("block_write failed");
			return -1;
		}
		completed += ret;
	}

	return BLOCK_SIZE;
}
