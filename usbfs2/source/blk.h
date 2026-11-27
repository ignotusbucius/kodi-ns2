// usbfs2 blk: pure SCSI-over-BOT block layer, READ ONLY. Runs on any BlkTransport: usbhs2.c on the console, the BOT
// device simulator in test/host_test.c on the build machine. Every CBW comes from bot.c (allow-list {00,03,12,25,28},
// data-in only). A transport or protocol failure (timeout, bad report, short CBW, bad/mismatched CSW, phase error)
// marks the device DEAD: no further command is ever sent to it (no reset, no Clear-Halt, no retry).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "bot.h"

enum { BLK_OK = 0, BLK_EIO = -5, BLK_DEAD = -19, BLK_RANGE = -22 };

typedef struct {
    void* ctx;
    // Bulk OUT: the 31-byte CBW. The transport re-checks the exact bytes it sends with bot_check_cbw(cbw, block).
    // 0 = sent and all 31 bytes transferred; anything else = transport failure.
    int (*send_cbw)(void* ctx, const uint8_t* cbw, uint32_t block);
    // Bulk IN, data phase of at most BOT_MAX_DATA bytes into the transport's buffer. 0 = completed (*got may be < len:
    // a short packet ends the phase); anything else = transport failure.
    int (*recv_data)(void* ctx, uint32_t len, const uint8_t** data, uint32_t* got);
    // Bulk IN, the 13-byte CSW. 0 = completed; anything else = transport failure.
    int (*recv_csw)(void* ctx, const uint8_t** csw, uint32_t* got);
    // Optional: 1 when the USB state-change event fired since the last call (possible unplug / re-enumeration).
    int (*changed)(void* ctx);
    void (*sleep_ms)(void* ctx, uint32_t ms);
} BlkTransport;

#define BLK_MAX_TUR_ATTEMPTS 8           // usbbotprobe's rule (retry only on UNIT ATTENTION / NOT READY ASC 04), 500 ms apart
#define BLK_TUR_RETRY_MS 500
#define BLK_MAX_CONSEC_ERRORS 8          // this many CSW-status-1 failures in a row also mark the device dead
// Every READ(10) moves at most BLK_MAX_XFER bytes (one page by default, see BOT_MAX_DATA in bot.h).
#define BLK_MAX_XFER BOT_MAX_DATA
#ifdef USBFS2_XFER_64K
#define BLK_LINE_BYTES 0x4000            // cache line: 16 KiB of consecutive blocks
#define BLK_LINES 16                     // 256 KiB cache
#else
#define BLK_LINE_BYTES 0x1000            // cache line: one page (= BLK_MAX_XFER)
#define BLK_LINES 64                     // 256 KiB cache
#endif
_Static_assert(BLK_LINE_BYTES <= BLK_MAX_XFER, "a cache line is one READ(10)");
#define BLK_MAX_BLOCKS_10 0x100000000ULL // READ(10) addresses at most 2^32 blocks (READ CAPACITY(16)/READ(16) not used)

typedef struct {
    uint64_t first;    // first LBA of the line (multiple of the line's block count)
    uint32_t count;    // valid blocks (< full line only at the end of the disk)
    uint32_t age;
    int valid;
} BlkLine;

typedef struct {
    BlkTransport t;
    int dead;
    uint32_t tag;
    uint32_t block;           // bytes per block (512/1024/2048/4096)
    uint64_t blocks;          // addressable blocks (last LBA + 1, at most 2^32)
    int capped;               // READ CAPACITY(10) returned 0xffffffff: the drive is >= 2 TiB, only 2^32 blocks used
    char vendor[9], product[17], rev[5];
    uint8_t type, rmb;
    uint32_t consec_errors;
    uint64_t cmds, bytes_read, cache_hits, cache_misses;
    uint32_t clock;
    BlkLine line[BLK_LINES];
    uint8_t* cache;           // BLK_LINES * BLK_LINE_BYTES, from the allocator given to blk_init
} Blk;

// TEST UNIT READY (sense-retry rule), INQUIRY, READ CAPACITY(10). `cache` must hold BLK_LINES * BLK_LINE_BYTES.
int blk_init(Blk* b, const BlkTransport* t, uint8_t* cache);
// Read `count` blocks at `lba` into dst. BLK_RANGE for anything past the end; BLK_DEAD once the device is dead;
// BLK_EIO for a command the device failed (CSW status 1, residue) while the protocol stayed in step.
int blk_read(Blk* b, uint64_t lba, uint32_t count, void* dst);
// Check mounted media: bounded retry only for becoming-ready (02/04/01); on failure invalidate the device.
int blk_check(Blk* b);
// Forget the cache and refuse every further command (unmount / unplug).
void blk_kill(Blk* b, const char* why);
