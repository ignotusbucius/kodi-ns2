// usbfs2 vfs: read-only file/directory API over FatFs (see vfs.h).
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include "ff.h"
#include "vfs.h"
#include "ulog.h"

struct VfsFile {
    FIL fil;
    int vol;
    int open;
    unsigned epoch;
};
struct VfsDir {
    DIR dir;
    int vol;
    int open;
    unsigned epoch;
};
size_t vfs_file_size(void) { return sizeof(VfsFile); }
size_t vfs_dir_size(void) { return sizeof(VfsDir); }

#ifndef ST_RDONLY
#define ST_RDONLY 1
#endif

static char g_key[64];
static int g_qualified;
int vfs_set_key(const char* key) {
    g_qualified = 1;
    g_key[0] = 0;
    if (!key) return 0;
    size_t n = strlen(key);
    if (!n || n >= sizeof(g_key)) return -EINVAL;
    for (size_t i=0;i<n;++i) {
        char c=key[i];
        if (!(c=='-' || (c>='0' && c<='9') || (c>='a' && c<='f') || (c>='A' && c<='F'))) return -EINVAL;
    }
    memcpy(g_key,key,n+1);
    return 0;
}
static unsigned g_epoch[VFS_MAX_VOLUMES];
static FATFS g_fs[VFS_MAX_VOLUMES];
static int g_mounted[VFS_MAX_VOLUMES];
static uint64_t g_start[VFS_MAX_VOLUMES], g_count[VFS_MAX_VOLUMES];
static char g_label[VFS_MAX_VOLUMES][VFS_LABEL_MAX];

static int err(FRESULT r) {
    switch (r) {
    case FR_OK: return 0;
    case FR_NO_FILE:
    case FR_NO_PATH: return -ENOENT;
    case FR_INVALID_NAME: return -EINVAL;
    case FR_DENIED: return -EACCES;
    case FR_EXIST: return -EEXIST;
    case FR_INVALID_OBJECT: return -EBADF;
    case FR_WRITE_PROTECTED: return -EROFS;
    case FR_INVALID_DRIVE:
    case FR_NOT_ENABLED:
    case FR_NO_FILESYSTEM: return -ENODEV;
    case FR_TIMEOUT:
    case FR_LOCKED: return -EBUSY;
    case FR_NOT_ENOUGH_CORE: return -ENOMEM;
    case FR_TOO_MANY_OPEN_FILES: return -EMFILE;
    case FR_INVALID_PARAMETER: return -EINVAL;
    default: return -EIO;    // FR_DISK_ERR, FR_INT_ERR, FR_NOT_READY, FR_MKFS_ABORTED
    }
}

const char* vfs_fs_name(int t) {
    return t == VFS_FS_FAT12 ? "FAT12" : t == VFS_FS_FAT16 ? "FAT16" : t == VFS_FS_FAT32 ? "FAT32" : t == VFS_FS_EXFAT ? "exFAT" : "none";
}

static int vol_ok(int vol) { return vol >= 0 && vol < VFS_MAX_VOLUMES && g_mounted[vol]; }

// "dev:/a/b" or "/a/b" -> "N:/a/b". -ENAMETOOLONG / -EINVAL (a second ':' or a control character).
static int fpath(int vol, const char* path, char* out, size_t outsz) {
    if (!path) return -EINVAL;
    const char* c = strchr(path, ':');
    const char* p = c ? c + 1 : path;
    if (g_qualified) {
        if (*p == '/') ++p;
        size_t n = strlen(g_key);
        if (!n || strncmp(p,g_key,n) || (p[n] && p[n]!='/')) return -ENODEV;
        p += n;
    }
    if (strchr(p, ':')) return -EINVAL;
    for (const char* q = p; *q; q++)
        if ((unsigned char)*q < 0x20) return -EINVAL;
    const int n = snprintf(out, outsz, "%d:%s%s", vol, *p == '/' ? "" : "/", p);
    if (n < 0 || (size_t)n >= outsz) return -ENAMETOOLONG;
    return 0;
}

int vfs_validate(int vol, const char* path) {
    char fp[VFS_PATH_MAX];
    return vol_ok(vol) ? fpath(vol,path,fp,sizeof(fp)) : -ENODEV;
}

static int is_root(const char* fp) { return fp[2] == '/' && fp[3] == 0; }

// The volume label (read-only: f_getlabel reads the root directory's label entry; FatFs has no f_setlabel with
// FF_FS_READONLY 1). f_getlabel writes at most 33 bytes + NUL (vfs.h); the buffer is VFS_LABEL_MAX. A drive's label
// is untrusted text: bytes < 0x20 and 0x7f become '_' (UTF-8 multi-byte sequences are kept as FatFs encoded them).
static void read_label(int vol, const char* drv) {
    char* l = g_label[vol];
    memset(l, 0, VFS_LABEL_MAX);
    const FRESULT r = f_getlabel(drv, l, NULL);
    if (r != FR_OK) {
        memset(l, 0, VFS_LABEL_MAX);
        ulog(1, "vfs: volume %d: f_getlabel rc=%d; no label", vol, (int)r);
        return;
    }
    l[VFS_LABEL_MAX - 1] = 0;
    for (char* p = l; *p; p++)
        if ((unsigned char)*p < 0x20 || (unsigned char)*p == 0x7f) *p = '_';
}

int vfs_mount(int vol, Blk* b, uint64_t start, uint64_t count, int* fstype) {
    *fstype = VFS_FS_NONE;
    if (vol < 0 || vol >= VFS_MAX_VOLUMES || g_mounted[vol] || !b || b->dead) return -EINVAL;
    diskio_attach(vol, b, start, count);
    char drv[4] = {(char)('0' + vol), ':', 0, 0};
    const FRESULT r = f_mount(&g_fs[vol], drv, 1);    // 1 = mount now (reads the boot record only)
    if (r != FR_OK) {
        f_mount(NULL, drv, 0);
        diskio_detach(vol);
        ulog(1, "vfs: volume %d (start %llu): f_mount rc=%d", vol, (unsigned long long)start, (int)r);
        return err(r);
    }
    g_key[0] = 0; // A new mount starts sealed, before it can become visible to other threads.
    ++g_epoch[vol];
    g_mounted[vol] = 1;
    g_start[vol] = start;
    g_count[vol] = count;
    *fstype = g_fs[vol].fs_type;    // FS_FAT12 1, FS_FAT16 2, FS_FAT32 3, FS_EXFAT 4 == VFS_FS_*
    read_label(vol, drv);
    return 0;
}

void vfs_unmount(int vol) {
    if (vol < 0 || vol >= VFS_MAX_VOLUMES || !g_mounted[vol]) return;
    char drv[4] = {(char)('0' + vol), ':', 0, 0};
    f_mount(NULL, drv, 0);    // invalidates every FIL/DIR of the volume (they then fail with -EBADF)
    diskio_detach(vol);
    g_mounted[vol] = 0;
    memset(g_label[vol], 0, VFS_LABEL_MAX);
}

int vfs_info(int vol, VfsInfo* o) {
    if (!vol_ok(vol)) return -ENODEV;
    const FATFS* fs = &g_fs[vol];
    o->fstype = fs->fs_type;
    o->cluster_bytes = (uint32_t)fs->csize * fs->ssize;
    o->clusters = fs->n_fatent >= 2 ? fs->n_fatent - 2 : 0;
    o->start = g_start[vol];
    o->count = g_count[vol];
    memcpy(o->label, g_label[vol], VFS_LABEL_MAX);
    return 0;
}

static time_t fat_time(WORD fdate, WORD ftime) {
    if (!fdate) return 0;
    int y = 1980 + (fdate >> 9), m = (fdate >> 5) & 15, d = fdate & 31;
    if (m < 1 || m > 12 || d < 1) return 0;
    // days from civil (proleptic Gregorian), UTC
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = (long long)era * 146097 + (long long)doe - 719468;
    return (time_t)(days * 86400 + (ftime >> 11) * 3600 + ((ftime >> 5) & 63) * 60 + (ftime & 31) * 2);
}

static void fill_stat(struct stat* st, const FILINFO* fi, uint32_t blksize) {
    memset(st, 0, sizeof(*st));
    st->st_mode = (fi->fattrib & AM_DIR) ? (S_IFDIR | 0555) : (S_IFREG | 0444);
    st->st_nlink = 1;
    st->st_size = (off_t)((fi->fattrib & AM_DIR) ? 0 : fi->fsize);
    st->st_blksize = blksize;
    st->st_blocks = (st->st_size + 511) / 512;
    st->st_mtime = st->st_atime = st->st_ctime = fat_time(fi->fdate, fi->ftime);
}

static uint32_t blksize(int vol) { return (uint32_t)g_fs[vol].csize * g_fs[vol].ssize; }

int vfs_open(VfsFile* f, int vol, const char* path, int flags) {
    memset(f, 0, sizeof(*f));
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC | O_APPEND))) return -EROFS;    // read-only
    if (!vol_ok(vol)) return -ENODEV;
    char fp[VFS_PATH_MAX];
    int rc = fpath(vol, path, fp, sizeof(fp));
    if (rc) return rc;
    if (is_root(fp)) return -EISDIR;
    const FRESULT r = f_open(&f->fil, fp, FA_READ | FA_OPEN_EXISTING);
    if (r != FR_OK) return err(r);
    f->vol = vol;
    f->epoch = g_epoch[vol];
    f->open = 1;
    return 0;
}

int vfs_close(VfsFile* f) {
    if (!f->open) return -EBADF;
    f->open = 0;
    if (!vol_ok(f->vol) || f->epoch != g_epoch[f->vol]) return 0;
    (void)f_close(&f->fil);    // read-only: nothing to flush; after an unmount FatFs says FR_INVALID_OBJECT, still closed
    return 0;
}

long long vfs_read(VfsFile* f, void* buf, size_t len) {
    if (!f->open || !vol_ok(f->vol) || f->epoch != g_epoch[f->vol]) return -EBADF;
    size_t done = 0;
    while (done < len) {
        const UINT want = (len - done) > 0x40000000u ? 0x40000000u : (UINT)(len - done);
        UINT br = 0;
        const FRESULT r = f_read(&f->fil, (uint8_t*)buf + done, want, &br);
        if (r != FR_OK) return done ? (long long)done : err(r);
        done += br;
        if (br < want) break;    // end of file
    }
    return (long long)done;
}

long long vfs_seek(VfsFile* f, long long off, int whence) {
    if (!f->open || !vol_ok(f->vol) || f->epoch != g_epoch[f->vol]) return -EBADF;
    long long base;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = (long long)f_tell(&f->fil); break;
    case SEEK_END: base = (long long)f_size(&f->fil); break;
    default: return -EINVAL;
    }
    if ((off < 0 && -off > base) || (off > 0 && base > (long long)0x7fffffffffffffffLL - off)) return -EINVAL;
    const FRESULT r = f_lseek(&f->fil, (FSIZE_t)(base + off));    // read-only: clamped to the file size
    if (r != FR_OK) return err(r);
    return (long long)f_tell(&f->fil);
}

int vfs_fstat(VfsFile* f, struct stat* st) {
    if (!f->open || !vol_ok(f->vol) || f->epoch != g_epoch[f->vol]) return -EBADF;
    FILINFO fi;
    memset(&fi, 0, sizeof(fi));
    fi.fsize = f_size(&f->fil);
    fill_stat(st, &fi, blksize(f->vol));
    return 0;
}

int vfs_stat(int vol, const char* path, struct stat* st) {
    if (!vol_ok(vol)) return -ENODEV;
    char fp[VFS_PATH_MAX];
    int rc = fpath(vol, path, fp, sizeof(fp));
    if (rc) return rc;
    FILINFO fi;
    memset(&fi, 0, sizeof(fi));
    if (is_root(fp)) {
        fi.fattrib = AM_DIR;
    } else {
        const FRESULT r = f_stat(fp, &fi);
        if (r != FR_OK) return err(r);
    }
    fill_stat(st, &fi, blksize(vol));
    return 0;
}

int vfs_opendir(VfsDir* d, int vol, const char* path) {
    memset(d, 0, sizeof(*d));
    if (!vol_ok(vol)) return -ENODEV;
    char fp[VFS_PATH_MAX];
    int rc = fpath(vol, path, fp, sizeof(fp));
    if (rc) return rc;
    const FRESULT r = f_opendir(&d->dir, fp);
    if (r != FR_OK) return err(r);
    d->vol = vol;
    d->epoch = g_epoch[vol];
    d->open = 1;
    return 0;
}

int vfs_readdir(VfsDir* d, char* name, size_t namesz, struct stat* st) {
    if (!d->open || !vol_ok(d->vol) || d->epoch != g_epoch[d->vol]) return -EBADF;
    for (;;) {
        FILINFO fi;
        const FRESULT r = f_readdir(&d->dir, &fi);
        if (r != FR_OK) return err(r);
        if (fi.fname[0] == 0) return 1;
        if (!strcmp(fi.fname, ".") || !strcmp(fi.fname, "..")) continue;
        const size_t n = strlen(fi.fname);
        if (n + 1 > namesz) {
            ulog(0, "vfs: a %u-byte name does not fit the %u-byte directory entry buffer; skipped", (unsigned)n,
                 (unsigned)namesz);
            continue;
        }
        memcpy(name, fi.fname, n + 1);
        if (st) fill_stat(st, &fi, blksize(d->vol));
        return 0;
    }
}

int vfs_rewinddir(VfsDir* d) {
    if (!d->open || !vol_ok(d->vol) || d->epoch != g_epoch[d->vol]) return -EBADF;
    return err(f_rewinddir(&d->dir));
}

int vfs_closedir(VfsDir* d) {
    if (!d->open) return -EBADF;
    d->open = 0;
    if (!vol_ok(d->vol) || d->epoch != g_epoch[d->vol]) return 0;
    (void)f_closedir(&d->dir);
    return 0;
}

int vfs_statvfs(int vol, struct statvfs* st) {
    if (!vol_ok(vol)) return -ENODEV;
    memset(st, 0, sizeof(*st));
    const FATFS* fs = &g_fs[vol];
    st->f_bsize = st->f_frsize = (unsigned long)fs->csize * fs->ssize;
    st->f_blocks = fs->n_fatent >= 2 ? fs->n_fatent - 2 : 0;
    st->f_bfree = st->f_bavail = 0;    // f_getfree is a writing-side function in FatFs (removed read-only)
    st->f_namemax = 255;
    st->f_flag = ST_RDONLY;
    return 0;
}
