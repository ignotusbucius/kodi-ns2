// usbfs2 bot: pure (no IPC, no libnx) USB mass-storage Bulk-Only Transport / SCSI rules, READ ONLY.
// Adapted from homebrew/usbbotprobe/source/bot.[ch] (flown twice, finding 1538): the allow-list, the CBW/CSW rules, the
// TUR sense-retry rule, the svcQueryMemory guard and the profile pick are the probe's; READ(10) is widened from
// "LBA 0, count 1" to any 32-bit LBA and 1..(BOT_MAX_DATA / block) blocks, data-in only. Compiled unchanged into the
// NRO / libusbfs2 and into the host test (build/host_test) that `make check` runs.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define BOT_CBW_LEN 31
#define BOT_CSW_LEN 13
#define BOT_CBW_SIGNATURE 0x43425355u      // "USBC"
#define BOT_CSW_SIGNATURE 0x53425355u      // "USBS"
// The largest data phase, one URB into the bound IN buffer. Default (v1b, reviewer b1): ONE PAGE, so no transfer can
// cross a 64 KiB boundary (the server's xHCI enqueue at 0xa3d80 splits at 64 KiB chunks, not 64 KiB address
// boundaries). -DUSBFS2_XFER_64K builds the 64 KiB variant (usbfs2test 4cfdd338), which needs its own flights.
#ifdef USBFS2_XFER_64K
#define BOT_MAX_DATA 0x10000
#else
#define BOT_MAX_DATA 0x1000
#endif
#define BOT_FLAG_NO_DATA 0x00
#define BOT_FLAG_DATA_IN 0x80              // every data phase is device-to-host; there is no data-out form at all
#define BOT_PROFILE_SIZE 0x128             // usb:hs interface profile (findings 1533 r5, 1536)
#define BOT_PAGE 0x1000
#define BOT_MEMTYPE_HEAP 0x05              // MemType_Heap (svcQueryMemory), static-asserted against libnx in usbhs2.c
#define BOT_PERM_RW 0x3                    // Perm_Rw, static-asserted against libnx in usbhs2.c

enum {
    BOT_OK = 0,
    BOT_REFUSED_ARGS,
    BOT_REFUSED_SIGNATURE,
    BOT_REFUSED_OPCODE,       // opcode not in the allow-list {0x00, 0x03, 0x12, 0x25, 0x28}
    BOT_REFUSED_LUN,
    BOT_REFUSED_CB_LENGTH,
    BOT_REFUSED_CB_TAIL,
    BOT_REFUSED_DATA_LENGTH,
    BOT_REFUSED_DIRECTION,
    BOT_REFUSED_FIELDS,       // a CDB field outside the one allowed form
    BOT_REFUSED_NO_FORM,      // allow-listed opcode with no form below (unreachable while the list is intact)
    BOT_REFUSED_BLOCK,        // READ(10) without a known 512/1024/2048/4096 block size
    BOT_CSW_BAD_SIGNATURE,
    BOT_CSW_TAG_MISMATCH,
    BOT_MEM_MISALIGNED,
    BOT_MEM_OUTSIDE_BLOCK,
    BOT_MEM_NOT_HEAP,
    BOT_MEM_NOT_RW,
    BOT_MEM_ATTR,
    BOT_MEM_OVERLAP,
    BOT_PROFILE_ID,
    BOT_PROFILE_NOT_MSC,
    BOT_PROFILE_ENDPOINTS,
    BOT_CAPACITY_BLOCK,       // READ CAPACITY(10) block length not 512/1024/2048/4096
    BOT_RC_COUNT
};
const char* bot_reason(int rc);

// The fixed CDBs (LUN 0). READ(10) CDBs are made only by bot_build_read10.
extern const uint8_t bot_cb_test_unit_ready[6];
extern const uint8_t bot_cb_request_sense[6];
extern const uint8_t bot_cb_inquiry[6];
extern const uint8_t bot_cb_read_capacity10[10];
// = BOT_MAX_DATA, in .rodata.bot_max_xfer of bot.o so `make check` can pin the compiled cap.
extern const uint32_t bot_max_xfer;

// 1 for 512, 1024, 2048, 4096; 0 otherwise.
int bot_block_ok(uint32_t block);

// Rules on 31 raw CBW bytes. BOT_OK only for: signature USBC, LUN 0, an allow-listed opcode with its exact CDB length,
// zero CDB tail, data length <= BOT_MAX_DATA, flags 0x00 with no data and 0x80 with data, and the one allowed form per
// opcode. READ(10) needs `block` (the device block length; 0 = unknown -> refused): flags/RDPROTECT/DPO/FUA byte 0,
// group 0, control 0, count >= 1 and data length == count * block.
int bot_check_cbw(const uint8_t* cbw, uint32_t block);
// The single CBW builder. Writes `out` only when bot_check_cbw() accepts the assembled bytes; otherwise zeroes it.
int bot_build_cbw(uint8_t* out, uint32_t tag, uint32_t data_len, uint8_t flags, const uint8_t* cb, uint8_t cb_len,
                  uint32_t block);
// READ(10) of `count` blocks at `lba`, through bot_build_cbw.
int bot_build_read10(uint8_t* out, uint32_t tag, uint32_t lba, uint16_t count, uint32_t block);
// CSW: signature USBS and tag; residue and status are returned for the caller to judge.
int bot_check_csw(const uint8_t* csw, uint32_t tag, uint32_t* residue, uint8_t* status);

// REQUEST SENSE data (fixed format, current error, >= 18 bytes, ASC present): 1 when TEST UNIT READY may be retried,
// i.e. sense key 6 UNIT ATTENTION (any ASC) or sense key 2 NOT READY with ASC 0x04 (becoming ready / initializing);
// 0 for anything else (other keys, other ASC, descriptor or deferred format, short or unwritten data).
int bot_sense_retryable(const uint8_t* s, size_t n);

// READ CAPACITY(10) data: last LBA and block length (big-endian). BOT_OK only for an allowed block length.
int bot_parse_capacity10(const uint8_t* d, uint32_t* last_lba, uint32_t* block);

// svcQueryMemory result, copied field by field from libnx MemoryInfo by usbhs2.c.
typedef struct { uint64_t addr, size; uint32_t type, attr, perm; } BotMemInfo;
// BOT_OK only when [buf, buf+len) is 4 KiB aligned, inside the block, (type & 0xff) == Heap, perm == Rw and, when
// require_attr0, attr == 0 (not borrowed / IPC / device-shared / uncached / permission-locked).
int bot_mem_check(const BotMemInfo* mi, uint64_t buf, uint64_t len, int require_attr0);
// 1 when the page-rounded ranges [a, a+alen) and [b, b+blen) share a page (or a length is 0).
int bot_ranges_overlap(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen);

typedef struct { uint32_t id; uint8_t in_addr, out_addr; uint16_t in_mps, out_mps; } BotIface;
// The interface to acquire, from the 0x128-byte profile: interface descriptor at +0x2a must be class 8/6/0x50 with
// 2 endpoints, and the +0x36 entries must hold exactly one bulk IN and one bulk OUT and nothing else.
int bot_profile_pick(const uint8_t* p, size_t n, BotIface* out);
