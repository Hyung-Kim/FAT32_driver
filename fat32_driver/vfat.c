// vim: noet:ts=4:sts=4:sw=4:et
#define FUSE_USE_VERSION 26
#define _GNU_SOURCE

#include <assert.h>
#include <endian.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <iconv.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdbool.h>

#include "vfat.h"
#include "util.h"
#include "debugfs.h"

#define DEBUG_PRINT(...) printf(__VA_ARGS__)

iconv_t iconv_utf16;
char* DEBUGFS_PATH = "/.debug";

void
vfat_init(const char *dev)
{
    struct fat_boot_header s;
    uint16_t rootDirSectors;
    uint8_t fat_0;
    uint32_t fatSz, dataSec, totSec, countOfClusters, first_fat;
    iconv_utf16 = iconv_open("utf-8", "utf-16"); // from utf-16 to utf-8
    // These are useful so that we can setup correct permissions in the mounted directories
    vfat_info.mount_uid = getuid();
    vfat_info.mount_gid = getgid();

    // Use mount time as mtime and ctime for the filesystem root entry (e.g. "/")
    vfat_info.mount_time = time(NULL);

    vfat_info.fd = open(dev, O_RDONLY);
    if (vfat_info.fd < 0)
        err(1, "open(%s)", dev);
    if (pread(vfat_info.fd, &s, sizeof(s), 0) != sizeof(s))
        err(1, "read super block");

    //check FAT32 filesystem 
    if(s.root_max_entries != 0) {
		err(1,"error: should be 0\n");
	}
	rootDirSectors = ((s.root_max_entries * 32) +
		(s.bytes_per_sector - 1)) / s.bytes_per_sector;

	if(s.sectors_per_fat_small != 0)
		fatSz = s.sectors_per_fat_small;
	else
		fatSz = s.sectors_per_fat;
	

	if(s.total_sectors_small != 0)
		totSec = s.total_sectors_small;
	else 
		totSec = s.total_sectors;
	

	dataSec = totSec - (s.reserved_sectors +
	(s.fat_count * fatSz) + rootDirSectors);
	countOfClusters = dataSec / s.sectors_per_cluster;

	if(countOfClusters < 4085) 
		err(1,"error: Volume is FAT12.\n");
	else if(countOfClusters < 65525) 
		err(1,"error: Volume is FAT16.\n");
	else 
		DEBUG_PRINT("Volume is FAT32.\n");
	

	// Check all other fields
    if(s.signature != 0xAA55)
    	err(1,"Magiic number 0xAA55 not present\n");
	

	if(s.jmp_boot[0] == 0xEB) {
		if(s.jmp_boot[2] != 0x90) {
			err(1, "jmp_boot[2] is wrong\n");
		}
	} else if(s.jmp_boot[0] != 0xE9){
		err(1, "jmp_boot[0] is wrong\n");
	}

	if(s.bytes_per_sector != 512 && s.bytes_per_sector != 1024 && s.bytes_per_sector != 2048 && s.bytes_per_sector != 5096) 

		err(1, "bytes_per_sector is wrong\n");
	

	if(s.sectors_per_cluster != 1 && s.sectors_per_cluster % 2 != 0) 
		err(1, "sectors_per_cluster is wrong\n");
	

	if(s.sectors_per_cluster * s.bytes_per_sector > 32 * 1024) 
		err(1, "bytes_per_sector * sectors_per_cluster is too large\n");
	

	if(s.reserved_sectors == 0) 
		err(1, "reserved_sectors is zero\n");
	

	if(s.fat_count < 2) 
		err(1, "fat count is less than two\n");
	

	if(s.root_max_entries != 0) 
		err(1, "root_max_entries must be zero\n");
	
	if(s.total_sectors_small != 0) 
		err(1, "total_sectors_small must be zero\n");
	
	if(s.media_info != 0xF0 &&s.media_info < 0xF8) 
		err(1, "Wrong media info\n");
	
	
	first_fat = s.reserved_sectors * s.bytes_per_sector;
	vfat_info.fat_begin_offset = lseek(vfat_info.fd, first_fat, SEEK_SET);
	if(vfat_info.fat_begin_offset == -1) 
		err(1, "lseek(%u)", first_fat);
	

	if(read(vfat_info.fd, &fat_0, sizeof(uint8_t)) != sizeof(uint8_t)) 
		err(1, "read(%lu)", sizeof(uint8_t));
	
	
	if(fat_0 != s.media_info) 
		err(1, "Media info is different in FAT[0]\n");
	

	if(s.sectors_per_fat_small != 0) 
		err(1, "sectors per fat small must be zero\n");
	
	if(s.total_sectors == 0) 
		err(1, "total_sectors must be non-zero\n");
	
    //init	
    vfat_info.root_max_entries = s.root_max_entries;
    vfat_info.fat_count = s.fat_count;
    vfat_info.bytes_per_sector = s.bytes_per_sector;
    vfat_info.sectors_per_cluster = s.sectors_per_cluster;
    vfat_info.cluster_size = s.sectors_per_cluster * s.bytes_per_sector;
    vfat_info.reserved_sectors = s.reserved_sectors;
    vfat_info.sectors_per_fat = s.sectors_per_fat;
    vfat_info.fat_entries = s.sectors_per_fat * s.bytes_per_sector /sizeof(uint32_t);
    vfat_info.root_cluster = s.root_cluster;
    vfat_info.root_inode.st_ino = le32toh(s.root_cluster);
    vfat_info.root_inode.st_mode = 0555 | S_IFDIR;
    vfat_info.root_inode.st_nlink = 1;
    vfat_info.root_inode.st_uid = vfat_info.mount_uid;
    vfat_info.root_inode.st_gid = vfat_info.mount_gid;
    vfat_info.root_inode.st_size = 0;
    vfat_info.root_inode.st_atime = vfat_info.root_inode.st_mtime = vfat_info.root_inode.st_ctime = vfat_info.mount_time;
    DEBUG_PRINT("Volume seems really FAT32.\n");
    if(lseek(vfat_info.fd,0,SEEK_SET)==-1)
    	err(1, "lseek(0)");
}


int vfat_next_cluster(uint32_t cluster_no)
{
	
        //DEBUG_PRINT("vfat_next_cluster %d\n", cluster_no);
	uint32_t next_cluster, next_cluster_check;
	uint32_t first_fat = vfat_info.reserved_sectors * vfat_info.bytes_per_sector;

	if(lseek(vfat_info.fd, first_fat + cluster_no * sizeof(uint32_t), SEEK_SET) == -1) 
		err(1, "lseek(%lu)", first_fat + cluster_no * sizeof(uint32_t));
	

	if(read(vfat_info.fd, &next_cluster, sizeof(uint32_t)) != sizeof(uint32_t)) 
		err(1, "read(%lu)",sizeof(uint32_t));
	

	if(lseek(vfat_info.fd, first_fat + vfat_info.sectors_per_fat * vfat_info.bytes_per_sector + cluster_no * sizeof(uint32_t) , SEEK_SET) == -1) 
		err(1, "lseek(%d)", first_fat);
	

	if(read(vfat_info.fd, &next_cluster_check, sizeof(uint32_t)) != sizeof(uint32_t)) 
		err(1, "read(%lu)", sizeof(uint32_t));
	

	if(next_cluster_check == next_cluster) 
		return next_cluster;
	
	err(1, "FAT is corrupted !");
}
void seek_cluster(uint32_t c)
{

    //DEBUG_PRINT("seek_cluster :  %d\n", c);
    if(c < 2)
    	err(1, "cluster number < 2");
    uint32_t firstDataSector = vfat_info.reserved_sectors + (vfat_info.fat_count *vfat_info.sectors_per_fat);
    uint32_t firstSectorOfCluster = ((c-2) * vfat_info.sectors_per_cluster) + firstDataSector;
    if(lseek(vfat_info.fd, firstSectorOfCluster * vfat_info.bytes_per_sector, SEEK_SET) == -1)
    	err(1, "lseek cluster_no %d\n", c);

}
void setStat(struct fat32_direntry dir_entry, char*buffer, fuse_fill_dir_t callback, void *callbackdata, uint32_t cluster_no)
{
   // DEBUG_PRINT("setStat   cluster_no : %d,\n", cluster_no);
   struct tm m_time;
   struct tm a_time;
   struct tm c_time;

   struct stat* stat_str = malloc(sizeof(struct stat));
			memset(stat_str, 0, sizeof(struct stat));
			stat_str->st_dev = 0; // Ignored by FUSE
			stat_str->st_ino = cluster_no; // Ignored by FUSE unless overridden
			if((dir_entry.attr & ATTR_READ_ONLY) == ATTR_READ_ONLY)
				stat_str->st_mode = S_IRUSR | S_IRGRP | S_IROTH;
			else
				stat_str->st_mode = S_IRWXU | S_IRWXG | S_IRWXO;
			
			if((dir_entry.attr & VFAT_ATTR_DIR) == VFAT_ATTR_DIR) 
			{
				stat_str->st_mode |= S_IFDIR;
				int cnt = 0;
				uint32_t next_cluster_no = cluster_no;
				off_t pos = lseek(vfat_info.fd, 0, SEEK_CUR);
				while(next_cluster_no < (uint32_t) 0x0FFFFFF8) {
					cnt++;
					next_cluster_no = vfat_next_cluster(0x0FFFFFFF & next_cluster_no);
				}
				if(lseek(vfat_info.fd, pos, SEEK_SET) == -1) 
					err(1, "Couldn't return to initial position: %lx", pos);
				stat_str->st_size = cnt * vfat_info.sectors_per_cluster*vfat_info.bytes_per_sector;
			}
			else {
				stat_str->st_mode |= S_IFREG;
				stat_str->st_size = dir_entry.size;
			}
			stat_str->st_nlink = 1;
			stat_str->st_uid = vfat_info.mount_uid;
			stat_str->st_gid = vfat_info.mount_gid;
			stat_str->st_rdev = 0;
			stat_str->st_blksize = 0; // Ignored by FUSE
			stat_str->st_blocks = 1;
			
			// Modified
			m_time.tm_hour = (dir_entry.mtime_time	& 0b1111100000000000) >> 11; 
			m_time.tm_min = (dir_entry.mtime_time	& 0b0000011111100000) >> 5; 
			m_time.tm_sec = (dir_entry.mtime_time	& 0b0000000000011111)*2; 
				 
			m_time.tm_year = ((dir_entry.mtime_date	& 0b1111111000000000) >> 9) + (1980 - 1900); 
			m_time.tm_mon = ((dir_entry.mtime_date & 0b0000000111100000) >> 5) -1;
			m_time.tm_mday = dir_entry.mtime_date	& 0b0000000000011111; 
			 	 
			// Accessed 
			a_time.tm_year = ((dir_entry.atime_date	& 0b1111111000000000) >> 9) + (1980 - 1900); 
			a_time.tm_mon = ((dir_entry.atime_date	& 0b0000000111100000) >> 5)-1;
			a_time.tm_mday = dir_entry.atime_date	& 0b0000000000011111; 
		
			// Created 
			c_time.tm_hour = (dir_entry.ctime_time	& 0b1111100000000000) >> 11; 
			c_time.tm_min = (dir_entry.ctime_time	& 0b0000011111100000) >> 5; 
			c_time.tm_sec = (dir_entry.ctime_time	& 0b0000000000011111) * 2; 
				 
			c_time.tm_year = ((dir_entry.ctime_date	& 0b1111111000000000) >> 9) + (1980 - 1900);
			c_time.tm_mon = ((dir_entry.ctime_date	& 0b0000000111100000) >> 5)-1; 
			c_time.tm_mday = (dir_entry.ctime_date	& 0b0000000000011111); 
			
			
			stat_str->st_atime = mktime(&a_time);
			stat_str->st_mtime = mktime(&m_time);
			stat_str->st_ctime = mktime(&c_time);

			callback(callbackdata, buffer, stat_str, 0);
			free(stat_str);
}
//checksum
unsigned char chkSum(unsigned char * pFcbName)
{
   //	 DEBUG_PRINT("chkSum %s\n", pFcbName);
	short fcbNameLen;
	unsigned char sum;

	sum = 0;
	for (fcbNameLen=11; fcbNameLen!=0; fcbNameLen--) {
		// NOTE: The operation is an unsigned char rotate right
		sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + *pFcbName++;
	}
	return (sum);
}
char* getfilename(char* nameext, char* filename)
{
    //DEBUG_PRINT("getfilename,  nameext :  %s,   filename : %s\n", nameext,filename);
	if(nameext[0] == 0x20) 
	    err(1, "filename[0] is 0x20");
	
	uint32_t fileNameCnt = 0;
	bool before_extension = true;
	bool in_spaces = false;
	bool in_extension = false;

	for(int i = 0; i < 11; i++) {
		if(nameext[i] < 0x20 ||
			nameext[i] == 0x22 ||
			nameext[i] == 0x2A ||
			nameext[i] == 0x2B ||
			nameext[i] == 0x2C ||
			nameext[i] == 0x2E ||
			nameext[i] == 0x2F ||
			nameext[i] == 0x3A ||
			nameext[i] == 0x3B ||
			nameext[i] == 0x3C ||
			nameext[i] == 0x3D ||
			nameext[i] == 0x3E ||
			nameext[i] == 0x3F ||
			nameext[i] == 0x5B ||
			nameext[i] == 0x5C ||
			nameext[i] == 0x5D ||
			nameext[i] == 0x7C) {

			err(1, "invalid character in filename %x at %d\n", nameext[i] & 0xFF, i);
		}

		if(before_extension) {
		    if(nameext[i] == 0x20) {
			before_extension = false;
			in_spaces = true;
			filename[fileNameCnt++] = '.';
		    } else if(i == 8) {
			before_extension = false;
			in_spaces = true;
			filename[fileNameCnt++] = '.';
			filename[fileNameCnt++] = nameext[i];
			in_extension = true;
		    } else {
			filename[fileNameCnt++] = nameext[i];
		    }
		} else if(in_spaces) {
			if(nameext[i] != 0x20) {
			    in_spaces = false;
			    in_extension = true;
			    filename[fileNameCnt++] = nameext[i];
			}
		} else if(in_extension) {
			if(nameext[i] == 0x20) {
			    break;
			} else {
			    filename[fileNameCnt++] = nameext[i];
			}
		}
	}

	if(filename[fileNameCnt - 1] == '.') {
	    filename--;
	}
	filename[fileNameCnt] = '\0';
	return filename;
}
int vfat_read_cluster(uint32_t cluster_no, fuse_fill_dir_t callback, void *callbackdata, bool bfirst_cluster)
{
    	//DEBUG_PRINT("vfat_read_cluster, cluster_no : %d, bfirst_cluster :  %d \n", cluster_no, bfirst_cluster);
        uint8_t check_sum = '\0';
	char* buffer = calloc(MAX_NAME_SIZE*2, sizeof(char)); // Max size of name: 13 * 0x14 = 260
	char* char_buffer = calloc(MAX_NAME_SIZE, sizeof(char));
	int i, j, seq_nb = 0;
	size_t in_byte_size = 2 * MAX_NAME_SIZE, out_byte_size = MAX_NAME_SIZE;
	struct fat32_direntry short_entry;
	struct fat32_direntry_long long_entry;
	memset(buffer, 0, 2*MAX_NAME_SIZE);

	seek_cluster(cluster_no);

	for(i = 0; i < vfat_info.sectors_per_cluster*vfat_info.bytes_per_sector; i+=32) {
		if(read(vfat_info.fd, &short_entry, 32) != 32){
			err(1, "read(short_dir)");
		}

		if(i < 64 && bfirst_cluster && cluster_no != 2){
			char* filename = (i == 0) ? "." : "..";
			setStat(short_entry,filename,callback,callbackdata,
				(((uint32_t)short_entry.cluster_hi) << 16) | ((uint32_t)short_entry.cluster_lo)); //. or ..

			continue;
		}

		if(((uint8_t) short_entry.nameext[0]) == 0xE5)
			continue;
		else if(short_entry.nameext[0] == 0x00) 
		{
			free(buffer);
			free(char_buffer);
			return 0;
		} 
		else if(short_entry.nameext[0] == 0x05) 
			continue;

		if((short_entry.attr & ATTR_LONG_NAME) == ATTR_LONG_NAME) {
			long_entry = *((struct fat32_direntry_long *) &short_entry);
			if((long_entry.seq & 0x40) == 0x40) 
			{
				seq_nb = (long_entry.seq & 0x3f) - 1;
				check_sum = long_entry.csum;

				for(j = 0; j < 13; j++) 
				{
					if(j < 5) //0~4
					{
					    if(long_entry.name1[j] == 0xFFFF)
					    	break;
						buffer[j*2] = long_entry.name1[j];
						buffer[j*2+1] = long_entry.name1[j] >> 8;
					} 
					else if(j < 11) 
					{ // 5~10
					    if(long_entry.name2[j-5] == 0xFFFF)
					    	break;
						buffer[j*2] = long_entry.name2[j-5];
						buffer[j*2+1] = long_entry.name2[j-5] >> 8;
					} 
					else if(j < 13) 
					{ //11~12
					    if(long_entry.name3[j-11] == 0xFFFF)
					    	break;
						buffer[j*2] = long_entry.name3[j-11];
						buffer[j*2+1] = long_entry.name3[j-11] >> 8;
					}
				}
			} else if (check_sum == long_entry.csum  && long_entry.seq == seq_nb) {
				seq_nb -= 1;

				char tmp[MAX_NAME_SIZE*2];
				memset(tmp, 0, MAX_NAME_SIZE*2);

				for(j = 0; j < MAX_NAME_SIZE*2; j++) {
					tmp[j] = buffer[j];
				}

				memset(buffer, 0, MAX_NAME_SIZE*2);

				for(j = 0; j < MAX_NAME_SIZE; j++) {
					if(j < 5 && long_entry.name1[j] != 0xFFFF) {
						buffer[j*2] = long_entry.name1[j];
						buffer[j*2+1] = long_entry.name1[j] >> 8;
					} else if(j < 11 && long_entry.name2[j - 5] != 0xFFFF) {
						buffer[j*2] = long_entry.name2[j - 5];
						buffer[j*2+1] = long_entry.name2[j - 5] >> 8;
					} else if(j < 13 && long_entry.name3[j - 11] != 0xFFFF) {
						buffer[j*2] = long_entry.name3[j - 11];
						buffer[j*2+1] = long_entry.name3[j - 11] >> 8;
					} else if(j >= 13 && ((uint16_t*)tmp)[j - 13] != 0xFFFF){
						buffer[j*2] = tmp[(j - 13)*2];
						buffer[j*2+1] = tmp[(j - 13)*2+1];
					}
				}
			} else {
				seq_nb = 0;
				check_sum = '\0';
				memset(buffer, 0, MAX_NAME_SIZE*2);
				err(1, "error: Bad sequence number or checksum\n");
			}
		} else if((short_entry.attr & ATTR_VOLUME_ID) == ATTR_VOLUME_ID) {
			seq_nb = 0;
			check_sum = '\0';
			memset(buffer, 0, MAX_NAME_SIZE*2);
		} else if(check_sum == chkSum((unsigned char *) &(short_entry.nameext)) && seq_nb == 0) {
			char* buffer_pointer = buffer;
			char* char_buffer_pointer = char_buffer;
			iconv(iconv_utf16, &buffer_pointer, &in_byte_size, &char_buffer_pointer, &out_byte_size);
			in_byte_size = MAX_NAME_SIZE*2;
			out_byte_size = MAX_NAME_SIZE;
			char *filename = char_buffer;
			setStat(short_entry,filename,callback,callbackdata,
				(((uint32_t)short_entry.cluster_hi) << 16) | ((uint32_t)short_entry.cluster_lo));
			check_sum = '\0';
			memset(buffer, 0, MAX_NAME_SIZE);
		} else {
			char *filename = char_buffer;
			getfilename(short_entry.nameext, filename);
			setStat(short_entry,filename,callback,callbackdata,
				(((uint32_t)short_entry.cluster_hi) << 16) | ((uint32_t)short_entry.cluster_lo));
		}
	}

	free(buffer);
	free(char_buffer);
	return 1;
}
int vfat_readdir(uint32_t cluster_no, fuse_fill_dir_t callback, void *callbackdata) 
{
      //  DEBUG_PRINT("vfat_readdir, cluster_no : %d\n", cluster_no);
 	struct stat st; // we can reuse same stat entry over and over again
	uint32_t next_cluster_no = cluster_no;
	bool eof = false;
	int end_of_read;

	memset(&st, 0, sizeof(st));
	st.st_uid = vfat_info.mount_uid;
	st.st_gid = vfat_info.mount_gid;
	st.st_nlink = 1;
	bool first_cluster = true;
	while(!eof) {
		end_of_read = vfat_read_cluster(next_cluster_no, callback, callbackdata,first_cluster);
		first_cluster = false;

		if(end_of_read == 0) {
			eof = true;
		} else {
			next_cluster_no = 0x0FFFFFFF & vfat_next_cluster(next_cluster_no);
			if(next_cluster_no >= (uint32_t) 0x0FFFFFF8) {
				eof = true;
			}
		}
	}

	return 0;
}


// Used by vfat_search_entry()
struct vfat_search_data {
    const char*  name;
    int          found;
    struct stat* st;
};


// You can use this in vfat_resolve as a callback function for vfat_readdir
// This way you can get the struct stat of the subdirectory/file.
int vfat_search_entry(void *data, const char *name, const struct stat *st, off_t offs)
{
    //DEBUG_PRINT("vfat_search_entry, name :  %s\n", name);
    struct vfat_search_data *sd = data;

    if (strcmp(sd->name, name) != 0) 
    	return 0;

    sd->found = 1;
    *sd->st = *st;

    return 1;
}

/**
 * Fills in stat info for a file/directory given the path
 * @path full path to a file, directories separated by slash
 * @st file stat structure
 * @returns 0 iff operation completed succesfully -errno on error
*/
int vfat_resolve(const char *path, struct stat *st)
{
   // DEBUG_PRINT("vfat_resolve,  path : %s\n", path);
        struct vfat_search_data sd;
	int i;
	const char *final_name;
	char *token = NULL, *path_copy;

	path_copy = malloc(strlen(path) + 1);
	strncpy(path_copy, path, strlen(path) + 1);

	memset(&sd, 0, sizeof(struct vfat_search_data));
	sd.st = st;

	for(i = strlen(path); path[i] != '/'; i--);
	final_name = path + i + 1;

	token = strtok(path_copy, "/");
	sd.name = token;

	vfat_readdir(vfat_info.root_cluster, vfat_search_entry, &sd);

	if(sd.found == 1) {
		while(strcmp(sd.name, final_name) != 0) {
			token = strtok(NULL, "/");
			if(token == NULL) {
				free(path_copy);
				return -ENOENT;
			}
			sd.name = token;
			sd.found = 0;
			vfat_readdir(((uint32_t) (sd.st)->st_ino), vfat_search_entry, &sd);
			if(sd.found != 1) {
				free(path_copy);
				return -ENOENT;
			}
		}
		free(path_copy);
		return 0;
	} else {
		free(path_copy);
		return -ENOENT;
	}
}

// Get file attributes
int vfat_fuse_getattr(const char *path, struct stat *st)
{
    if(strncmp(path, DEBUGFS_PATH, strlen(DEBUGFS_PATH))==0)
    	return debugfs_fuse_getattr(path+strlen(DEBUGFS_PATH),st);
    //DEBUG_PRINT("vfat_fuse_getattr, path :  %s\n",path);
    if (strcmp(path, "/") == 0) {
    		//DEBUG_PRINT("internal vfat_fuse_getattr\n");
		st->st_dev = 0; // Ignored by FUSE
		st->st_ino = 0; // Ignored by FUSE unless overridden
		st->st_mode = S_IRWXU | S_IRWXG | S_IRWXO | S_IFDIR;
		st->st_nlink = 1;
		st->st_uid = vfat_info.mount_uid;
		st->st_gid = vfat_info.mount_gid;
		st->st_rdev = 0;
		int cnt = 0;
		uint32_t next_cluster_no = vfat_info.root_cluster;
		off_t pos = lseek(vfat_info.fd, 0, SEEK_CUR);
		while(next_cluster_no < (uint32_t) 0x0FFFFFF8) {
			cnt++;
			next_cluster_no = vfat_next_cluster(0x0FFFFFFF & next_cluster_no);
		}
		if(lseek(vfat_info.fd, pos, SEEK_SET) == -1) {
			err(1, "Couldn't return to initial position: %lx", pos);
		}
		st->st_size = cnt * vfat_info.sectors_per_cluster*vfat_info.bytes_per_sector;
		st->st_blksize = 0; // Ignored by FUSE
		st->st_blocks = 1;
		return 0;
	}
	if(vfat_resolve(path + 1, st) != 0) {
		return -ENOENT;
	} else {
		return 0;
	}
}

// Extended attributes useful for debugging, no need!
int vfat_fuse_getxattr(const char *path, const char* name, char* buf, size_t size)
{
    struct stat st;
    int ret = vfat_resolve(path, &st);
    if (ret != 0) return ret;
    if (strcmp(name, "debug.cluster") != 0) return -ENODATA;

    if (buf == NULL) {
        ret = snprintf(NULL, 0, "%u", (unsigned int) st.st_ino);
        if (ret < 0) err(1, "WTF?");
        return ret + 1;
    } else {
        ret = snprintf(buf, size, "%u", (unsigned int) st.st_ino);
        if (ret >= size) return -ERANGE;
        return ret;
    }
}

int vfat_fuse_readdir(
        const char *path, void *callback_data,
        fuse_fill_dir_t callback, off_t unused_offs, struct fuse_file_info *unused_fi)
{
    
    if(strncmp(path, DEBUGFS_PATH, strlen(DEBUGFS_PATH)) ==0)
    	return debugfs_fuse_readdir(path + strlen(DEBUGFS_PATH), callback_data, callback, unused_offs, unused_fi);
    //DEBUG_PRINT("vfat_fuse_readdir, path : %s\n", path);
	struct stat st;
	if(strcmp(path, "/") != 0) {
		vfat_resolve(path+1, &st);
		vfat_readdir((uint32_t)st.st_ino, callback, callback_data);
	} else {
	    vfat_readdir(vfat_info.root_cluster, callback, callback_data);
	}
	return 0;

}

int vfat_fuse_read(
        const char *path, char *buf, size_t size, off_t offs,
        struct fuse_file_info *unused)
{
    //DEBUG_PRINT("vfat_fuse_read, path :  %s\n",path);   
    if(strncmp(path, DEBUGFS_PATH, strlen(DEBUGFS_PATH))==0)
    	return debugfs_fuse_read(path + strlen(DEBUGFS_PATH), buf, size, offs, unused);
    //DEBUG_PRINT("fuse read %s\n",path);
    assert(size>1);
    struct stat st;
    vfat_resolve(path+1, &st);
    if(!(st.st_mode & S_IFREG)) //if not regular file
    {
	DEBUG_PRINT("Trying to read a directory or not regular file\n");
	return -1;
    }
    size_t cnt =0;
    size_t cluster_size = vfat_info.sectors_per_cluster * vfat_info.bytes_per_sector;
    uint32_t c = (uint32_t)st.st_ino;
    
    if(offs >= st.st_size)
    {
	memset(buf, 0,size);
	return 0;
    }
    
    while(offs >= cluster_size)
    {
	c = vfat_next_cluster(c);
	offs -= cluster_size;
    }
    seek_cluster(c);
    if(lseek(vfat_info.fd,offs,SEEK_CUR) == -1)
    	err(1,"seek last part of offset failed\n");
    if(cluster_size - offs > size)
    {
	if(read(vfat_info.fd, buf+cnt, size) != size)
	    err(1,"read cluster-offs > size failed\n");
	return 0; 
    }else{
	if(read(vfat_info.fd, buf+cnt, cluster_size-offs) != cluster_size-offs)
	    err(1,"read cluster-offs <= size failed\n");
	cnt+= cluster_size - offs;
    }
    while(size - cnt > cluster_size)
    {
    	c = vfat_next_cluster(c);
	seek_cluster(c);
	//DEBUG_PRINT("Read cluster_no %x\n",c);
	if((c & 0x0FFFFFFF) >= 0x0FFFFFF8)
	{
	    memset(buf+cnt, 0, size-cnt);
	    return cnt;
	}
	if(read(vfat_info.fd, buf+cnt, cluster_size) != cluster_size)
	    err(1, "read cluster_size failed\n");
	cnt+= cluster_size;
    }
    c = vfat_next_cluster(c);
    seek_cluster(c);
    if((c & 0x0FFFFFFF) >= 0x0FFFFFF8)
    {
	memset(buf+cnt, 0, size-cnt);
	return cnt;
    }
    if(cnt < size)
    {
	if(read(vfat_info.fd, buf+cnt, size-cnt) != size-cnt)
	    err(1, "read cluster_size failed\n");
	cnt += size-cnt;
    }
    return cnt; // number of bytes read from the file
    		// must be size unless EOF reached, negative for an error
}

////////////// No need to modify anything below this point
int
vfat_opt_args(void *data, const char *arg, int key, struct fuse_args *oargs)
{
    if (key == FUSE_OPT_KEY_NONOPT && !vfat_info.dev) {
        vfat_info.dev = strdup(arg);
        return (0);
    }
    return (1);
}

struct fuse_operations vfat_available_ops = {
    .getattr = vfat_fuse_getattr,
    .getxattr = vfat_fuse_getxattr,
    .readdir = vfat_fuse_readdir,
    .read = vfat_fuse_read,
};

int main(int argc, char **argv)
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    fuse_opt_parse(&args, NULL, NULL, vfat_opt_args);

    if (!vfat_info.dev)
        errx(1, "missing file system parameter");
    vfat_init(vfat_info.dev);
    return (fuse_main(args.argc, args.argv, &vfat_available_ops, NULL));
}
