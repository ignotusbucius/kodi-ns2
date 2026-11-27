// usbfs2 diskio: FatFs media access over blk.c, READ ONLY. FatFs physical drive N = mounted volume N = one partition
// (start/count in device blocks) of one Blk. disk_write is deliberately NOT defined: with FF_FS_READONLY 1 ff.c never
// calls it, and if any object referenced it the link would fail (`make check` also asserts no disk_write symbol).
#include <string.h>
#include "ff.h"
#include "diskio.h"
#include "blk.h"
#include "vfs.h"

_Static_assert(FF_FS_READONLY == 1, "usbfs2: FatFs must be configured read-only (source/ffconf.h)");
_Static_assert(FF_USE_MKFS == 0 && FF_USE_EXPAND == 0 && FF_USE_CHMOD == 0 && FF_USE_STRFUNC == 0 && FF_USE_FORWARD == 0,
               "usbfs2: no FatFs writing or extra function");
// v1c: FF_USE_LABEL 1 adds f_getlabel only; ff.c builds f_setlabel only when !FF_FS_READONLY (asserted above).
_Static_assert(FF_USE_LABEL == 1 && FF_FS_READONLY == 1, "usbfs2: volume labels are read-only (f_getlabel, no f_setlabel)");
_Static_assert(FF_VOLUMES == VFS_MAX_VOLUMES, "one FatFs volume per vfs volume");

typedef struct {
    Blk* blk;
    uint64_t start, count;
} Drive;
static Drive g_drive[FF_VOLUMES];

void diskio_attach(int pdrv, Blk* b, uint64_t start, uint64_t count) {
    if (pdrv < 0 || pdrv >= FF_VOLUMES) return;
    g_drive[pdrv] = (Drive){.blk = b, .start = start, .count = count};
}

void diskio_detach(int pdrv) {
    if (pdrv < 0 || pdrv >= FF_VOLUMES) return;
    g_drive[pdrv] = (Drive){0};
}

static Drive* drive(BYTE pdrv) {
    if (pdrv >= FF_VOLUMES || !g_drive[pdrv].blk || g_drive[pdrv].blk->dead) return NULL;
    return &g_drive[pdrv];
}

DSTATUS disk_status(BYTE pdrv) { return drive(pdrv) ? STA_PROTECT : (STA_NOINIT | STA_PROTECT); }

DSTATUS disk_initialize(BYTE pdrv) { return disk_status(pdrv); }

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
    Drive* d = drive(pdrv);
    if (!d) return RES_NOTRDY;
    if (!buff || count == 0 || (uint64_t)sector >= d->count || count > d->count - sector) return RES_PARERR;
    const int rc = blk_read(d->blk, d->start + sector, count, buff);
    return rc == BLK_OK ? RES_OK : rc == BLK_DEAD ? RES_NOTRDY : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
    Drive* d = drive(pdrv);
    if (!d) return RES_NOTRDY;
    switch (cmd) {
    case GET_SECTOR_COUNT:
        if (d->count > (LBA_t)-1) return RES_PARERR;
        *(LBA_t*)buff = (LBA_t)d->count;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD*)buff = (WORD)d->blk->block;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD*)buff = 1;
        return RES_OK;
    default:    // CTRL_SYNC / CTRL_TRIM and everything else: never needed read-only, refused
        return RES_PARERR;
    }
}
