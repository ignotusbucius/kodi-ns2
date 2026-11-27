// usbfs2 devoptab: newlib devices "ums0:" .. "ums7:" over vfs.c, READ ONLY. Every operation takes the usbfs2 lock.
// Every writing operation is defined and refuses: open with any write intent -> EROFS (vfs_open), write -> EBADF,
// link/unlink/rename/mkdir/rmdir/chmod/fchmod/utimes/ftruncate/symlink -> EROFS. None of them reaches FatFs or USB.
#include <errno.h>
#include <string.h>
#include <sys/iosupport.h>
#include <sys/syslimits.h>
#include "vfs.h"
#include "usbfs2_dev.h"

static devoptab_t g_dot[VFS_MAX_VOLUMES];
static char g_name[VFS_MAX_VOLUMES][8];
static int g_volidx[VFS_MAX_VOLUMES];
static int g_added[VFS_MAX_VOLUMES];

// The volume of a path op: from its "umsN:" prefix, else (no prefix: this is the default device) from the deviceData
// newlib passes in r->deviceData.
static int vol_of(struct _reent* r, const char* path) {
    if (path && path[0] == 'u' && path[1] == 'm' && path[2] == 's' && path[3] >= '0' && path[3] < '0' + VFS_MAX_VOLUMES &&
        path[4] == ':')
        return path[3] - '0';
    if (path && strchr(path, ':')) return -1;    // another device's prefix: never ours
    const int* p = (const int*)r->deviceData;
    return p ? *p : -1;
}

static int ret(struct _reent* r, long long rc) {
    if (rc < 0) {
        r->_errno = (int)-rc;
        return -1;
    }
    return 0;
}

static int erofs(struct _reent* r) {
    r->_errno = EROFS;
    return -1;
}

static int d_open(struct _reent* r, void* fs, const char* path, int flags, int mode) {
    (void)mode;
    usbfs2_lock();
    const int rc = vfs_open((VfsFile*)fs, vol_of(r, path), path, flags);
    usbfs2_unlock();
    return ret(r, rc);
}

static int d_close(struct _reent* r, void* fd) {
    usbfs2_lock();
    const int rc = vfs_close((VfsFile*)fd);
    usbfs2_unlock();
    return ret(r, rc);
}

static ssize_t d_write(struct _reent* r, void* fd, const char* ptr, size_t len) {
    (void)fd, (void)ptr, (void)len;
    r->_errno = EBADF;    // every file is open read-only
    return -1;
}

static ssize_t d_read(struct _reent* r, void* fd, char* ptr, size_t len) {
    usbfs2_lock();
    const long long n = vfs_read((VfsFile*)fd, ptr, len);
    usbfs2_unlock();
    if (n < 0) {
        r->_errno = (int)-n;
        return -1;
    }
    return (ssize_t)n;
}

static off_t d_seek(struct _reent* r, void* fd, off_t pos, int dir) {
    usbfs2_lock();
    const long long n = vfs_seek((VfsFile*)fd, (long long)pos, dir);
    usbfs2_unlock();
    if (n < 0) {
        r->_errno = (int)-n;
        return -1;
    }
    return (off_t)n;
}

static int d_fstat(struct _reent* r, void* fd, struct stat* st) {
    usbfs2_lock();
    const int rc = vfs_fstat((VfsFile*)fd, st);
    usbfs2_unlock();
    return ret(r, rc);
}

static int d_stat(struct _reent* r, const char* file, struct stat* st) {
    usbfs2_lock();
    const int rc = vfs_stat(vol_of(r, file), file, st);
    usbfs2_unlock();
    return ret(r, rc);
}

static int d_link(struct _reent* r, const char* a, const char* b) { (void)a, (void)b; return erofs(r); }
static int d_unlink(struct _reent* r, const char* a) { (void)a; return erofs(r); }
static int d_rename(struct _reent* r, const char* a, const char* b) { (void)a, (void)b; return erofs(r); }
static int d_mkdir(struct _reent* r, const char* a, int m) { (void)a, (void)m; return erofs(r); }
static int d_rmdir(struct _reent* r, const char* a) { (void)a; return erofs(r); }
static int d_chmod(struct _reent* r, const char* a, mode_t m) { (void)a, (void)m; return erofs(r); }
static int d_fchmod(struct _reent* r, void* fd, mode_t m) { (void)fd, (void)m; return erofs(r); }
static int d_utimes(struct _reent* r, const char* a, const struct timeval t[2]) { (void)a, (void)t; return erofs(r); }
static int d_ftruncate(struct _reent* r, void* fd, off_t l) { (void)fd, (void)l; return erofs(r); }
static int d_symlink(struct _reent* r, const char* a, const char* b) { (void)a, (void)b; return erofs(r); }
static int d_fsync(struct _reent* r, void* fd) { (void)r, (void)fd; return 0; }    // nothing is ever dirty

static DIR_ITER* d_diropen(struct _reent* r, DIR_ITER* ds, const char* path) {
    usbfs2_lock();
    const int rc = vfs_opendir((VfsDir*)ds->dirStruct, vol_of(r, path), path);
    usbfs2_unlock();
    return ret(r, rc) ? NULL : ds;
}

static int d_dirreset(struct _reent* r, DIR_ITER* ds) {
    usbfs2_lock();
    const int rc = vfs_rewinddir((VfsDir*)ds->dirStruct);
    usbfs2_unlock();
    return ret(r, rc);
}

static int d_dirnext(struct _reent* r, DIR_ITER* ds, char* name, struct stat* st) {
    usbfs2_lock();
    const int rc = vfs_readdir((VfsDir*)ds->dirStruct, name, NAME_MAX + 1, st);
    usbfs2_unlock();
    if (rc == 1) {
        r->_errno = ENOENT;    // end of directory (newlib's readdir returns NULL)
        return -1;
    }
    return ret(r, rc);
}

static int d_dirclose(struct _reent* r, DIR_ITER* ds) {
    usbfs2_lock();
    const int rc = vfs_closedir((VfsDir*)ds->dirStruct);
    usbfs2_unlock();
    return ret(r, rc);
}

static int d_statvfs(struct _reent* r, const char* path, struct statvfs* st) {
    usbfs2_lock();
    const int vol = vol_of(r, path);
    int rc = vfs_validate(vol, path);
    if (!rc) rc = vfs_statvfs(vol, st);
    usbfs2_unlock();
    return ret(r, rc);
}

int usbfs2_dev_add(int vol) {
    if (vol < 0 || vol >= VFS_MAX_VOLUMES) return -1;
    if (g_added[vol]) return 0;
    g_name[vol][0] = 'u', g_name[vol][1] = 'm', g_name[vol][2] = 's', g_name[vol][3] = (char)('0' + vol);
    g_name[vol][4] = 0;
    g_volidx[vol] = vol;
    g_dot[vol] = (devoptab_t){
        .name = g_name[vol],
        .structSize = vfs_file_size(),
        .open_r = d_open,
        .close_r = d_close,
        .write_r = d_write,
        .read_r = d_read,
        .seek_r = d_seek,
        .fstat_r = d_fstat,
        .stat_r = d_stat,
        .link_r = d_link,
        .unlink_r = d_unlink,
        .rename_r = d_rename,
        .mkdir_r = d_mkdir,
        .dirStateSize = vfs_dir_size(),
        .diropen_r = d_diropen,
        .dirreset_r = d_dirreset,
        .dirnext_r = d_dirnext,
        .dirclose_r = d_dirclose,
        .statvfs_r = d_statvfs,
        .ftruncate_r = d_ftruncate,
        .fsync_r = d_fsync,
        .deviceData = &g_volidx[vol],
        .chmod_r = d_chmod,
        .fchmod_r = d_fchmod,
        .rmdir_r = d_rmdir,
        .lstat_r = d_stat,
        .utimes_r = d_utimes,
        .symlink_r = d_symlink,
    };
    if (AddDevice(&g_dot[vol]) < 0) return -1;
    g_added[vol] = 1;
    return 0;
}

void usbfs2_dev_remove(int vol) {
    if (vol < 0 || vol >= VFS_MAX_VOLUMES || !g_added[vol]) return;
    char n[8];
    memcpy(n, g_name[vol], 5);
    n[4] = ':', n[5] = 0;
    RemoveDevice(n);
    g_added[vol] = 0;
}
