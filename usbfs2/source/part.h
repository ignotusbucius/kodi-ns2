// usbfs2 part: pure, read-only partition table parsing (whole-disk VBR, MBR + extended/logical, GPT with CRC32
// checks) and filesystem identification from a partition's first 4 KiB. Reads only through the PartReadFn it is given.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { PART_SCHEME_NONE = 0, PART_SCHEME_WHOLE, PART_SCHEME_MBR, PART_SCHEME_GPT };
enum { PART_FS_UNKNOWN = 0, PART_FS_FAT, PART_FS_EXFAT, PART_FS_NTFS, PART_FS_EXT, PART_FS_HFSPLUS, PART_FS_APFS,
       PART_FS_BTRFS, PART_FS_COUNT };

#define PART_MAX 16
#define PART_MAX_EBR 64
#define PART_GPT_MAX_ENTRIES 1024
#define PART_PROBE_BYTES 4096

typedef struct {
    uint64_t start, count;     // in device blocks
    int scheme;                // PART_SCHEME_*
    int index;                 // 1-based index in its table (MBR: 1..4 primary, 5.. logical)
    uint8_t mbr_type;          // MBR partition type (0 for GPT)
    uint8_t guid[16];          // GPT partition type GUID (on-disk byte order)
    char name[37];             // GPT partition name (ASCII subset of the UTF-16 name)
} PartEntry;

// Reads `count` device blocks at `lba` into buf; 0 = ok.
typedef int (*PartReadFn)(void* ctx, uint64_t lba, uint32_t count, void* buf);

// Scans the disk (`blocks` blocks of `block` bytes; block <= 4096). Fills at most `max` entries, returns how many
// (>= 0) or < 0 when sector 0 itself could not be read. *scheme gets PART_SCHEME_*. Every entry lies inside the disk.
int part_scan(PartReadFn rd, void* ctx, uint32_t block, uint64_t blocks, PartEntry* out, int max, int* scheme);
// Filesystem from the first bytes of a partition (n >= PART_PROBE_BYTES recommended: ext/HFS+ superblocks are at 1 KiB).
int part_probe_fs(const uint8_t* p, size_t n);
const char* part_fs_name(int fs);
const char* part_scheme_name(int scheme);
// 1 when the sector looks like a FAT12/16/32 or exFAT volume boot record (the whole-disk / "superfloppy" case).
int part_is_fat_vbr(const uint8_t* s, uint32_t block);
// CRC-32 (IEEE 802.3, as GPT uses), incremental: crc = part_crc32(crc, p, n), start with 0.
uint32_t part_crc32(uint32_t crc, const void* p, size_t n);
// GPT type GUID formatted as text (mixed-endian as usual), buf >= 37 bytes.
void part_guid_str(const uint8_t* g, char* buf);
