// usbfs2: READ-ONLY USB mass-storage filesystems for Switch 2 homebrew (libounce / kaiHEN), public API.
// FAT12/16/32 and exFAT volumes of the first USB drive appear as newlib devices "ums0:" .. "ums7:" (fopen, open,
// stat, opendir/readdir, statvfs). Nothing can write to the drive: no write/create/mkdir/unlink/rename path exists
// (EROFS), the FatFs build is read-only, and the USB layer only sends the SCSI opcodes TEST UNIT READY, REQUEST SENSE,
// INQUIRY, READ CAPACITY(10) and READ(10). Thread-safe: one internal mutex serialises every call and every devoptab op.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef USBFS2_LOGFN_DEFINED
#define USBFS2_LOGFN_DEFINED
// level 0 = milestones and errors, 1 = every setup/teardown IPC ("pre <step>"), 2 = every bulk transfer.
typedef void (*Usbfs2LogFn)(int level, const char* line);
#endif

enum { USBFS2_FS_NONE = 0, USBFS2_FS_FAT12, USBFS2_FS_FAT16, USBFS2_FS_FAT32, USBFS2_FS_EXFAT };

typedef struct {
    char device[8];        // "ums0"
    char root[8];          // "ums0:/"
    int fstype;            // USBFS2_FS_*
    const char* fs_name;   // "FAT32", "exFAT", ...
    const char* scheme;    // "MBR", "GPT", "whole-disk"
    int part_index;        // index in its partition table (MBR 1..4 primary, 5.. logical; GPT 1..; whole-disk 0)
    uint64_t start_block, block_count;
    uint32_t block_size;
    uint32_t cluster_bytes;
    uint64_t clusters;
    char label[36];        // v1c: the volume label (UTF-8, read once at mount, read-only); "" = none; bytes < 0x20 -> '_'
} Usbfs2Volume;

typedef struct {
    int attached, dead;
    uint16_t vid, pid;
    uint32_t iface_id;
    char vendor[9], product[17], rev[5];
    uint32_t block_size;
    uint64_t blocks;       // addressable (at most 2^32: READ(10))
    int capped;            // the drive is >= 2 TiB; only the first 2 TiB are addressable
    uint64_t commands, bytes_read, cache_hits, cache_misses;
} Usbfs2Drive;

// Opens the one usb:hs session (process lifetime; mass-storage filter only). log may be NULL.
int usbfs2_init(Usbfs2LogFn log, int verbosity);
// Change the log verbosity (0..2) at any time.
void usbfs2_set_verbosity(int verbosity);
// Attaches the first USB mass-storage interface and mounts its FAT/exFAT volumes. Returns the number of volumes
// mounted (0 = a drive but no supported volume), or < 0 (-1 no drive / USB error).
int usbfs2_mount(void);
// Opt in to attachment-qualified device paths. NULL seals all roots until a key is published.
int usbfs2_set_path_key(const char* key);
int usbfs2_volume_count(void);
int usbfs2_volume(int i, Usbfs2Volume* out);
int usbfs2_drive(Usbfs2Drive* out);
// Poll periodically (e.g. once a second). Mounted drives are checked for readiness even if no event fired.
// An insert arms bounded, spaced discovery queries only while the interface is not yet assigned.
// Returns 1 when the set of mounted volumes changed, else 0.
int usbfs2_poll(void);
// Unmount every volume and release the interface (the usb:hs client session stays open).
void usbfs2_unmount(void);
// usbfs2_unmount, then close the usb:hs session. Call from atexit / before the process exits.
void usbfs2_exit(void);

#ifdef __cplusplus
}
#endif
