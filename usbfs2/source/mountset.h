// usbfs2 mountset: pure "drive -> volumes" logic shared by the console build (usbfs2.c) and the host test:
// partition scan (part.c), filesystem probe of each partition, FatFs mount (vfs.c) of every FAT/exFAT partition into
// volume slots 0..VFS_MAX_VOLUMES-1. Reads only; never writes.
#pragma once
#include "blk.h"
#include "part.h"
#include "vfs.h"

typedef struct {
    int vol;          // FatFs / devoptab volume index (umsN)
    int fstype;       // VFS_FS_*
    int part;         // index into MountSet.parts
} MsVolume;

typedef struct {
    int scheme;                  // PART_SCHEME_*
    int nparts;
    PartEntry parts[PART_MAX];
    int part_fs[PART_MAX];       // PART_FS_* from the partition's first 4 KiB
    int nvol;
    MsVolume vol[VFS_MAX_VOLUMES];
} MountSet;

// Returns the number of mounted volumes (>= 0) or < 0 when the partition table could not be read.
int ms_mount(MountSet* m, Blk* b);
void ms_unmount(MountSet* m);
