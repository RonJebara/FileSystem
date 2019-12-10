/*
 *  Copyright (C) 2019 CS416 Spring 2019
 *	
 *	Tiny File System
 *
 *	File:	tfs.c
 *  Author: Yujie REN
 *	Date:	April 2019
 *
 */

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
#include <libgen.h>
#include <limits.h>
#include "block.h"
#include "tfs.h"


char diskfile_path[PATH_MAX];
struct superblock super;
bitmap_t inode_bitmap;
bitmap_t drive_bitmap;	
//struct inode inode_array[super.max_inum];

/* 
 * Get available number from bitmaps
 */
int get_avail_ino(){
//Iterates through the inode bitmap and checks to see if the inode is not in use. 
//If it is not it sets it to in use and returns the number that the inode is.
//Returns -1 if it does not work.
//Changed the default return to -1 since i can be equal to 0.
	int i = 0;
	for(i = 0; i < super.max_inum; i++){
		if(!get_bitmap(inode_bitmap, i)){
			set_bitmap(inode_bitmap, i);
			return i;
		}
	} 
	return -1;
}

int get_avail_blkno(){
//Does the same thing as the inode function above.
	int i = 0;
	for(i = 0; i < super.max_dnum; i++){
		if(!get_bitmap(drive_bitmap, i)){
			set_bitmap(drive_bitmap, i);
			return i;
		}
	}
	return -1;
}

/* 
 * inode operations
 */
int readi(uint16_t ino, struct inode *inode) {
//Checks the superblock to find the start of the inode and reads the from that
//address and stores it in the inode struct passed to it.
	int block = super.i_start_blk + (ino / INODES);
	struct inode array[INODES];
	bio_read(block, array);
	*inode = array[ino % INODES];
	return 0;
}

int writei(uint16_t ino, struct inode *inode) {
//Does the same thing as the function above but instead of reading it writes the 
//inode to disk instead of reading.
	int block = super.i_start_blk + (ino / INODES);
	struct inode array[INODES];
	bio_read(block, array);
	array[ino % INODES] = *inode;
	bio_write(block, array);
	return 0;
}


/* 
 * directory operations
 */
int dir_find(uint16_t ino, const char *fname, size_t name_len, struct dirent *dirent) {
//Starts at the specified inode and looks for the inode that points to the directory
//where fname matches the directory name found in the array. When its found it copies
//the directory entry into a new dirrent entry. If it ant find the directory it returns
//-1.
	struct inode inode;
	readi(ino, &inode);
	struct dirent directory[DIRENT];
	int i = 0;
	int x = 0;
	for (i = 0; i < DIRECT; i++){
		if(inode.direct_ptr[i] != -1){
			bio_read(inode.direct_ptr[i], directory);
			for(x = 0; x < DIRENT; x++){
				if(directory[x].valid){
					if(!strcmp(directory[x].name, fname)){
						if(dirent){
							*dirent = directory[x];
						}
						return 0;
					}
				}
			}
		}
	}
	return -1;
}

int dir_add(struct inode dir_inode, uint16_t f_ino, const char *fname, size_t name_len) {
//Checks to make sure the directory doesnt already exist. If it doesnt it iterates 
//through direct pointers looking for one that is free. When it finds a free one it 
//sets its block number, marks it in use, gives it an inode and all that jazz
//then writes it to disk.
	if(dir_find(dir_inode.ino, fname, name_len, NULL) != -1){
		return -1;
	}
	int i = 0;
	int x = 0;
	for(i =0; i < DIRECT; i++){
		if(dir_inode.direct_ptr[i] == -1){
			dir_inode.size += sizeof(struct dirent);
			dir_inode.direct_ptr[i] = get_avail_blkno();
			struct dirent dirs[DIRENT];
			dirs[0].valid = 1;
			dirs[0].ino = f_ino;
			strcpy(dirs[0].name, fname);
			bio_write(dir_inode.direct_ptr[i], dirs);
			writei(dir_inode.ino, &dir_inode);
			return 0;
		}
		else{
			struct dirent dir[DIRENT];
			bio_read(dir_inode.direct_ptr[i], dir);
			for(x = 0; x < DIRENT; x++){
				if(!dir[x].valid){
					dir_inode.size += sizeof(struct dirent);
					dir[x].valid = 1;
					dir[x].ino = f_ino;
					strcpy(dir[x].name, fname);
					bio_write(dir_inode.direct_ptr[i], dir);
					writei(dir_inode.ino, &dir_inode);
					return 0;
				}
			}
		}
	}
	return -1;
}

int dir_remove(struct inode dir_inode, const char *fname, size_t name_len) {
//Not sure if this will work with a non empty directory. If we have enough time
//will test for that. Checks to make sure the directory actually exists. If it
//does it sets its valid to 0 and removes it from the bitmap. returns -1 if
//it doesn't work
	struct dirent dirs[DIRENT];
	int i = 0;
	int x = 0;
	for(i = 0; i < DIRECT; i++){
		bio_read( dir_inode.direct_ptr[i], dirs);
		for(x = 0; x < DIRENT; x++){
			if(!strcmp(dirs[x].name, fname)){
				dirs[x].valid = 0;
				unset_bitmap(drive_bitmap, dir_inode.direct_ptr[i] - super.d_start_blk);
			writei(dir_inode.ino, &dir_inode);
			bio_write(dir_inode.direct_ptr[i], dirs);
			dir_inode.direct_ptr[i] = -1;
			return 0;
			}
		}
	}
	return -1;
}

/* 
 * namei operation
 */
int get_node_by_path(const char *path, uint16_t ino, struct inode *inode) {
//This took alot of tinkering. I couldn't figure out a way to check to mae sure
//the name was formatted correctly, but after some intense googling I got this.
//So it checks to make sure the name is formatted correctly. If it is it then
//hunts down its inode by comparing the names.
	char *start;
	char *end;
	int next = ino;
	for(start = strchr(path, '/'); start; start = strchr(start + 1, '/'));{
		end = strchr(start + 1, '/');
		if(end - start <= 1){
			return -1;
		}
		else{
			char fname[PATH_MAX];
			memset(fname, 0, PATH_MAX);
			if(end){
				strncpy(fname, start + 1, end - start - 1);
				end = strchr(end + 1, '/');
			}
			else{
				strcpy(fname, start + 1);
			}
			struct dirent d;
			if(dir_find(next, fname, strlen(fname), &d)){
				return -1;
			}
			else{
				next = d.ino;
			}
		}
	}
	readi(next, inode);
	return 0;
}


int rooter(){
//Will only be called once. To find the first free inode so we can use for
//our superblock. At first we assumed it will always be the first inode,
//but we ran into errors when we tried testing our code with that assumption. 
//We talked to previous OS students and they told us that they had ths same
//problem so they needed a function that would set the root inode and keep 
//it there forever, but can't guarantee it will be in the same position
//every time you make a new root. It works by finding a free inode and
//block and then just populating the root inode struct. Then it writes 
//the inode to disk.
	int ino = get_avail_ino();
	int blkno = get_avail_blkno();
	struct inode root = {.ino = ino, .valid = 1, .size = sizeof(struct dirent) * 2, .type = S_IFDIR | 0777, .link = 1, .direct_ptr = {[0] = blkno, [1 ... DIRECT - 1] = -1}, .indirect_ptr = {[0 ... INDIRECT - 1] = -1}, .vstat.st_mode = S_IFDIR | 0777};
writei(ino, &root);
return ino;
}

/* 
 * Make file system
 */
int tfs_mkfs() {
//Just copies all important info into super block. and writes it to disk. Needed
//to add a new entry to super block that just stores the inode number for the 
//superblock. Was not done at first, but ran into many problemms with
//get_node_by_path. We just assumed that the inode for superblock was zero,
//but I guess not.
	dev_init(diskfile_path);
	super.max_dnum = MAX_DNUM;
	super.max_inum = MAX_INUM;
	super.magic_num = MAGIC_NUM;
	super.i_bitmap_blk = 1;
	super.d_bitmap_blk = super.i_bitmap_blk + ((int) ceil(((double) MAX_INUM / 8) /BLOCK_SIZE));
	super.i_start_blk = super.d_bitmap_blk + ((int) ceil(((double) MAX_DNUM / 8) /BLOCK_SIZE));
	super.d_start_blk = super.i_start_blk + ((int) ceil((double) MAX_INUM / INODES));
	inode_bitmap = malloc(sizeof(unsigned char) * MAX_INUM / 8);
	drive_bitmap = malloc(sizeof(unsigned char) * MAX_DNUM / 8);
	super.root_node = rooter(); 
	bio_write(0, &super);
//	super.root_node = (uint16_t) make_root(); MAY DELETE
	// Call dev_init() to initialize (Create) Diskfile
	// write superblock information
	// initialize inode bitmap
	// initialize data block bitmap
	// update bitmap information for root directory
	// update inode for root directory
	return 0;
}


/* 
 * FUSE file operations
 */
static void *tfs_init(struct fuse_conn_info *conn) {
//Tries to open the path, if it can't it calls mkfs. If not found
//it mallocs the bitmaps and reads them from the superblock.
	int fd = dev_open(diskfile_path);
	int ibytes = MAX_INUM / 8;
	int dbytes = MAX_DNUM / 8;
	if(fd){
		tfs_mkfs();
		fd = dev_open(diskfile_path);
	}
	else{
		drive_bitmap = 	malloc(sizeof(unsigned char) * dbytes);
		inode_bitmap = malloc(sizeof(unsigned char) * ibytes);
		reader(0, sizeof(struct superblock), &super);
		reader(super.i_bitmap_blk, ibytes, inode_bitmap);
		reader(super.d_bitmap_blk, dbytes, drive_bitmap);
	}
	return NULL;
}

static void tfs_destroy(void *userdata) {
//Writes the bitmaps to make sure they are up to date on disk then
//frees the bitmaps and closes the fd.
	writer(0, sizeof(struct superblock), &super);
	writer(super.i_bitmap_blk, MAX_INUM / 8, inode_bitmap);
	writer(super.d_bitmap_blk, MAX_DNUM / 8, drive_bitmap);
	free(inode_bitmap);
	free(drive_bitmap);
	dev_close();
}

static int tfs_getattr(const char *path, struct stat *stbuf) {
//Makes an inode struct so we can populate it if we can find the inode
//for the path. If we cant find the inode return -1. If we do find it
//populate the given stat node. Didn't fill in all of it as I felt 
//the only things we needed were those I filled.
	struct inode inode;
	if(get_node_by_path(path, super.root_node, &inode)){
		return -1;
	}
	else{
		stbuf->st_size = inode.size;
		stbuf->st_nlink = inode.link;
		stbuf->st_mode = inode.vstat.st_mode;
		stbuf->st_uid = getuid();
		stbuf->st_gid = getgid();
//		stbuf->st_mode   = S_IFDIR | 0755; Maybe delete?
//		stbuf->st_nlink  = 2;
//		time(&stbuf->st_mtime);
	}
	return 0;
}

static int tfs_opendir(const char *path, struct fuse_file_info *fi){
//checks to make sure the path is valid when trying to change directories.
	struct inode inode;
	if(get_node_by_path(path, super.root_node, &inode)){
		return -1;
	}
    return 0;
}

static int tfs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
//Again checks the path is valid. If it is it makes a struct that will store
//all the names of files and stuff down that path. 
	struct inode inode;
        if(get_node_by_path(path, super.root_node, &inode)){
                 return -1;
        }
	struct dirent dirs[DIRENT];
	int i = 0;
	int x = 0;
	for(i = 0; i < DIRECT; i++){
		if(inode.direct_ptr[i] != -1){
			bio_read(inode.direct_ptr[i], dirs);
			for(x = 0; x < DIRENT; x++){
				if(dirs[x].valid){
					filler(buffer, dirs[i].name, NULL, 0);
				}
			}
		}
	}
	return 0;
}

static int tfs_mkdir(const char *path, mode_t mode) {
//Dont really get how dirname/basename work, but hey you told us to use it and it 
//works. Gets the name for the parent and target. Again gets the inode using
//get_node_by_path. If its valid it then finds an availble inode and calls dir_add
//to actually make the directory, and then we make the inode for that directory 
//and write it to disk.
	char* dirs;
	strcpy(dirs, path);
	char* base;
	strcpy(base, path);
	char* paths = dirname(dirs);
	char* fname = basename(base);
	struct inode inode;
	if(get_node_by_path(paths, super.root_node, &inode)){
		printf("%s\n", "In get_node");
		return -1;
	}
	int new = get_avail_ino();
	if(dir_add(inode, new, fname, strlen(fname))){
		printf("%s\n", "In dir_add");
		return -1;
	}
	struct inode new_inode = {.ino = new, .valid = 1, .size = sizeof(struct dirent) * 2, .type = mode, .link = 1, .direct_ptr = { [1 ... DIRECT - 1] = -1}, .indirect_ptr = { [0 ... INDIRECT - 1] = -1}, .vstat.st_mode = S_IFDIR | 0755};
	new_inode.direct_ptr[0] = get_avail_blkno();
	writei(new, &new_inode);
	return 0;
}

static int tfs_rmdir(const char *path) {
//Works almost exactly the same way as tfs_mkdir, except this removes it from
//the bitmap and then checks the path again and if it is a good path it then
//calls dir_remove instead of dir_add and removes the directory with the
//given name.
	char* dirs;
	strcpy(dirs, path);
	char* base;
	strcpy(base, path);
	char* paths = dirname(dirs);
	char* fname = basename(base);
	struct inode rminode;
	struct inode leadinode;
	if(get_node_by_path(path, super.root_node, &rminode)){
		return -1;
	}
	int i = 0;
	for(i = 0; i < DIRECT; i++){
		if(rminode.direct_ptr[i] != -1){
			unset_bitmap(drive_bitmap, rminode.direct_ptr[i] - super.d_start_blk);
		rminode.direct_ptr[i] = -1;
		}
	}
	unset_bitmap(inode_bitmap, rminode.ino);
	if(get_node_by_path(paths, super.root_node, &leadinode)){
		return -1;
	}
	dir_remove(leadinode, fname, strlen(fname));
	return 0;	
}

static int tfs_releasedir(const char *path, struct fuse_file_info *fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}

static int tfs_create(const char *path, mode_t mode, struct fuse_file_info *fi){
//This is for files. Does the getting name stuff. Checks the path. finds ino,
//and then calls dir_add to create the file and fills the inode and writes it
//to disk.
	printf("%s\n", "In tfs_create");
	char* dirs;
        strcpy(dirs, path);
        char* base;
        strcpy(base, path);
        char* paths = dirname(dirs);
        char* fname = basename(base);
	struct inode inode;
	if(get_node_by_path(paths, super.root_node, &inode)){
		return -1;
	}
	int new_ino = get_avail_ino();
	dir_add(inode, new_ino, fname, strlen(fname));
	struct inode new_inode = {
		.ino = new_ino,
		.valid = 1,
		.size = 0,
		.type = mode,
		.link = 1,
		.direct_ptr = {[0 ... DIRECT - 1] = -1 },
		.indirect_ptr = {[0 ... INDIRECT - 1] = -1},
		.vstat.st_mode = S_IFREG | 0777};
	writei(new_ino, &new_inode);
	return 0;
}

static int tfs_open(const char *path, struct fuse_file_info *fi) {
//Literally just makes sure the path is good.
	struct inode inode;
	if(get_node_by_path(path, super.root_node, &inode)){
        	return -1;
	}
	return 0;
}

static int tfs_read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {
//Makes inode checks path yadda yadda. It then uses offset and block size to 
//know where to go to start reading size bytes from direct and indirect blocks.
//Then it returns the amount of bytes read.
	int copier = size;
	struct inode inode;
	if(get_node_by_path(path, super.root_node, &inode)){
		return -1;
	}
	int i = 0;
	char block[BLOCK_SIZE];
	for(i = (offset / BLOCK_SIZE); i < TOTAL && copier; i++){
		if(i >= DIRECT){
			int indirect_ptr[BLOCK_SIZE / sizeof(int)];
			bio_read(inode.indirect_ptr[(i - DIRECT) / BLOCK_SIZE], indirect_ptr);
			bio_read(indirect_ptr[(i - DIRECT) % BLOCK_SIZE], block);

		}
		else{
			if(inode.direct_ptr[i] == -1){
				return -1;
			}
			else{
				bio_read(inode.direct_ptr[i], block);
			}
		}
		int copy = min(copier, BLOCK_SIZE - (offset % BLOCK_SIZE));
		memcpy(buffer, block + (offset % BLOCK_SIZE), copy);
		copier -= copy;
		buffer += copy;
	}
	return size - copier; 
}

static int tfs_write(const char *path, const char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) { 
	struct inode inode;
	if(get_node_by_path(path, super.root_node, &inode)){
		return -1;
	}
	int copier = size;
	char block[BLOCK_SIZE];
	int i = 0;
	int blk;
	for(i = (offset / BLOCK_SIZE); i < TOTAL && copier; i++){
		if(i >= DIRECT){
			int indirect_ptr[BLOCK_SIZE / sizeof(int)];
			if(inode.indirect_ptr[(i - DIRECT) / BLOCK_SIZE] == -1){
				inode.indirect_ptr[(i - DIRECT) / BLOCK_SIZE] = get_avail_blkno();
			}
			else{
				bio_read(inode.indirect_ptr[(i - DIRECT) / BLOCK_SIZE], indirect_ptr);
			}
			if(indirect_ptr[(i - DIRECT) % BLOCK_SIZE] == -1){		
				indirect_ptr[(i - DIRECT) % BLOCK_SIZE] = get_avail_blkno();
				bio_write(inode.indirect_ptr[(i - DIRECT) / BLOCK_SIZE], indirect_ptr);
			}
			blk = indirect_ptr[(i - DIRECT) % BLOCK_SIZE];
		}
		else{
			if(inode.direct_ptr[i] == -1){
				inode.direct_ptr[i] = get_avail_blkno();
			}
			blk = inode.direct_ptr[i];
		}
	
		bio_read(blk, block);
		int copy = min(copier, BLOCK_SIZE - (offset % BLOCK_SIZE));
		memcpy(block + (offset % BLOCK_SIZE), buffer, copy);
		bio_write(blk, block);
		copier -= copy;
		buffer += copy;
	}
	inode.size += size - copier;
	writei(inode.ino, &inode);
	return size - copier;
}

static int tfs_unlink(const char *path) {
	char* dirs;
	strcpy(dirs, path);
	char* base;
	strcpy(base, path);
	char* paths = dirname(dirs);
	char* fname = basename(base);
	struct inode inode;
	if(get_node_by_path(path, super.root_node, &inode)){
		return -1;
	}
	int i = 0;
	int x = 0;
	for(i = 0; i < DIRECT; i++){
		unset_bitmap(drive_bitmap, inode.direct_ptr[i] - super.d_start_blk);
	}
	for(i = 0; i < INDIRECT; i++){
		int indirect_ptr[BLOCK_SIZE / sizeof(int)];
		bio_read(inode.indirect_ptr[i], indirect_ptr);
		for(x = 0; x < (BLOCK_SIZE / sizeof(int)); x++){
			if(indirect_ptr[x] != -1){
				unset_bitmap(drive_bitmap, indirect_ptr[x] - super.d_start_blk);
			}
		}
		unset_bitmap(drive_bitmap, inode.indirect_ptr[i] - super.d_start_blk);
	}
	unset_bitmap(inode_bitmap, inode.ino);
	inode.valid = 0;
	writei(inode.ino, &inode);
	struct inode p_node;
	if(get_node_by_path(paths, super.root_node, &p_node)){
		return -1;
	}
	dir_remove(p_node, fname, strlen(fname));

	return 0;
	// Step 1: Use dirname() and basename() to separate parent directory path and target file name
	// Step 2: Call get_node_by_path() to get inode of target file
	// Step 3: Clear data block bitmap of target file
	// Step 4: Clear inode bitmap and its data block
	// Step 5: Call get_node_by_path() to get inode of parent directory
	// Step 6: Call dir_remove() to remove directory entry of target file in its parent directory
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

