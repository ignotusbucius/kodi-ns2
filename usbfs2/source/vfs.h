// usbfs2 vfs: READ-ONLY file/directory API over FatFs volumes (pure: no IPC, no libnx). devoptab.c maps newlib's
// devoptab onto it on the console; test/host_test.c calls it directly. Every function returns >= 0 or -errno.
// There is deliberately no write, create, truncate, mkdir, unlink, rename, chmod or utime function here.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include "blk.h"

#define VFS_MAX_VOLUMES 8
#define VFS_PATH_MAX 1024
// Volume label, UTF-8 + NUL: FatFs's f_getlabel stores at most 11 UTF-16 units (exFAT) or 11 OEM bytes (FAT), each at
// most 3 UTF-8 bytes (a surrogate pair: 4 for 2 units), so 34 bytes; 36 keeps the struct 4-byte padded.
#define VFS_LABEL_MAX 36

enum { VFS_FS_NONE = 0, VFS_FS_FAT12, VFS_FS_FAT16, VFS_FS_FAT32, VFS_FS_EXFAT };

// Opaque (they hold FatFs FIL / DIR; FatFs's DIR would clash with POSIX DIR in devoptab.c). Storage of
// vfs_file_size() / vfs_dir_size() bytes, 8-byte aligned, provided by the caller (newlib's fileStruct / dirStruct).
typedef struct VfsFile VfsFile;
typedef struct VfsDir VfsDir;
size_t vfs_file_size(void);
size_t vfs_dir_size(void);

typedef struct {
    int fstype;               // VFS_FS_*
    uint32_t cluster_bytes;
    uint64_t clusters;
    uint64_t start, count;    // partition, in device blocks
    char label[VFS_LABEL_MAX];    // read once at mount (f_getlabel); "" = none; control characters replaced by '_'
} VfsInfo;

// diskio.c: FatFs drive N -> partition of `b`.
void diskio_attach(int pdrv, Blk* b, uint64_t start, uint64_t count);
void diskio_detach(int pdrv);

// Mount volume `vol` (0..7) on a partition. 0 and *fstype set, or -ENODEV when it holds no FAT/exFAT filesystem.
// Also reads the volume label (f_getlabel: root directory, read-only); a label that cannot be read is "".
int vfs_set_key(const char* key);
int vfs_validate(int vol, const char* path);
int vfs_mount(int vol, Blk* b, uint64_t start, uint64_t count, int* fstype);
void vfs_unmount(int vol);
int vfs_info(int vol, VfsInfo* out);
const char* vfs_fs_name(int fstype);

// `path` may carry a "device:" prefix (as devoptab passes it); only the part after the first ':' is used.
// Any write intent (O_WRONLY, O_RDWR, O_CREAT, O_TRUNC, O_APPEND) -> -EROFS, before FatFs is touched.
int vfs_open(VfsFile* f, int vol, const char* path, int flags);
int vfs_close(VfsFile* f);
long long vfs_read(VfsFile* f, void* buf, size_t len);
long long vfs_seek(VfsFile* f, long long off, int whence);
int vfs_fstat(VfsFile* f, struct stat* st);
int vfs_stat(int vol, const char* path, struct stat* st);
int vfs_opendir(VfsDir* d, int vol, const char* path);
// 0 = one entry (name, st), 1 = end of directory, < 0 = error.
int vfs_readdir(VfsDir* d, char* name, size_t namesz, struct stat* st);
int vfs_rewinddir(VfsDir* d);
int vfs_closedir(VfsDir* d);
int vfs_statvfs(int vol, struct statvfs* st);
