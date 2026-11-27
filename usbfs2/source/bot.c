// usbfs2 bot: pure BOT/SCSI rules (see bot.h). No IPC, no libnx, no global state.
#include <string.h>
#include "bot.h"

// The SCSI opcode allow-list: TEST UNIT READY, REQUEST SENSE, INQUIRY, READ CAPACITY(10), READ(10). Nothing else.
// No WRITE / FORMAT / MODE SELECT / SYNCHRONIZE CACHE / ATA PASS-THROUGH / vendor opcode can pass bot_check_cbw.
static const uint8_t kScsiAllowed[5] = {0x00, 0x03, 0x12, 0x25, 0x28};

const uint8_t bot_cb_test_unit_ready[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const uint8_t bot_cb_request_sense[6] = {0x03, 0x00, 0x00, 0x00, 0x12, 0x00};
const uint8_t bot_cb_inquiry[6] = {0x12, 0x00, 0x00, 0x00, 0x24, 0x00};
const uint8_t bot_cb_read_capacity10[10] = {0x25, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const uint32_t bot_max_xfer = BOT_MAX_DATA;

static const char* const kReason[] = {
    "ok", "bad arguments", "bad CBW signature", "opcode not allow-listed", "LUN not 0", "wrong CDB length",
    "nonzero CDB tail", "wrong data length", "wrong direction flag", "CDB field outside the allowed form",
    "opcode has no allowed form", "READ(10) without a known block size", "bad CSW signature", "CSW tag mismatch",
    "buffer not 4 KiB aligned", "buffer not inside one memory block", "memory type is not heap",
    "memory permission is not RW", "memory attribute not 0", "buffers overlap", "bad interface id",
    "interface is not mass storage 8/6/0x50", "endpoints are not exactly one bulk IN + one bulk OUT",
    "block length not 512/1024/2048/4096",
};

_Static_assert(sizeof(kReason) / sizeof(kReason[0]) == BOT_RC_COUNT, "one reason string per code");
_Static_assert(BOT_MAX_DATA % 4096 == 0 && BOT_MAX_DATA / 512 <= 0xffff, "READ(10) count fits 16 bits");

const char* bot_reason(int rc) { return (rc >= 0 && rc < BOT_RC_COUNT) ? kReason[rc] : "?"; }

static uint32_t rd32le(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint32_t rd32be(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}
static void wr32le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static int all_zero(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (p[i]) return 0;
    return 1;
}

int bot_block_ok(uint32_t block) { return block == 512 || block == 1024 || block == 2048 || block == 4096; }

// The list is read from .rodata.kScsiAllowed at run time (the barrier stops the compiler folding it into compares),
// so the 5 bytes `make check` pins in bot.o are the bytes the check uses.
static int opcode_allowed(uint8_t op) {
    const uint8_t* list = kScsiAllowed;
    __asm__ volatile("" : "+r"(list));
    for (size_t i = 0; i < sizeof(kScsiAllowed); i++)
        if (list[i] == op) return 1;
    return 0;
}

// noipa: one out-of-line copy of the rules, called by bot_build_cbw and by the OUT send in usbhs2.c.
__attribute__((noipa)) int bot_check_cbw(const uint8_t* cbw, uint32_t block) {
    if (!cbw) return BOT_REFUSED_ARGS;
    const uint8_t* cb = cbw + 15;
    if (!opcode_allowed(cb[0])) return BOT_REFUSED_OPCODE;
    if (rd32le(cbw) != BOT_CBW_SIGNATURE) return BOT_REFUSED_SIGNATURE;
    const uint32_t len = rd32le(cbw + 8);
    const uint8_t flags = cbw[12], lun = cbw[13], cb_len = cbw[14];
    if (lun != 0) return BOT_REFUSED_LUN;
    if (cb_len != (cb[0] < 0x20 ? 6 : 10)) return BOT_REFUSED_CB_LENGTH;
    if (!all_zero(cb + cb_len, 16 - cb_len)) return BOT_REFUSED_CB_TAIL;
    if (len > BOT_MAX_DATA) return BOT_REFUSED_DATA_LENGTH;
    if (flags != (len ? BOT_FLAG_DATA_IN : BOT_FLAG_NO_DATA)) return BOT_REFUSED_DIRECTION;
    switch (cb[0]) {
    case 0x00:    // TEST UNIT READY: no data, every field zero
        if (len != 0) return BOT_REFUSED_DATA_LENGTH;
        return all_zero(cb + 1, 5) ? BOT_OK : BOT_REFUSED_FIELDS;
    case 0x03:    // REQUEST SENSE: fixed format, allocation length 18
        if (len != 18) return BOT_REFUSED_DATA_LENGTH;
        return (all_zero(cb + 1, 3) && cb[4] == 18 && cb[5] == 0) ? BOT_OK : BOT_REFUSED_FIELDS;
    case 0x12:    // INQUIRY: standard data (EVPD 0, page 0), allocation length 36
        if (len != 36) return BOT_REFUSED_DATA_LENGTH;
        return (all_zero(cb + 1, 3) && cb[4] == 36 && cb[5] == 0) ? BOT_OK : BOT_REFUSED_FIELDS;
    case 0x25:    // READ CAPACITY(10): LBA 0, PMI 0, 8 bytes back
        if (len != 8) return BOT_REFUSED_DATA_LENGTH;
        return all_zero(cb + 1, 9) ? BOT_OK : BOT_REFUSED_FIELDS;
    case 0x28: {  // READ(10): byte 1 (RDPROTECT/DPO/FUA/RARC) 0, any LBA (cb[2..5]), group 0, count >= 1, control 0
        if (!bot_block_ok(block)) return BOT_REFUSED_BLOCK;
        const uint32_t count = (uint32_t)cb[7] << 8 | cb[8];
        if (count == 0 || len != count * block) return BOT_REFUSED_DATA_LENGTH;
        return (cb[1] == 0 && cb[6] == 0 && cb[9] == 0) ? BOT_OK : BOT_REFUSED_FIELDS;
    }
    default:    // the allow-list above already refused it; a distinct code so the host test sees which layer refused
        return BOT_REFUSED_NO_FORM;
    }
}

int bot_build_cbw(uint8_t* out, uint32_t tag, uint32_t data_len, uint8_t flags, const uint8_t* cb, uint8_t cb_len,
                  uint32_t block) {
    if (!out) return BOT_REFUSED_ARGS;
    memset(out, 0, BOT_CBW_LEN);
    if (!cb || cb_len == 0 || cb_len > 16) return BOT_REFUSED_ARGS;
    uint8_t c[BOT_CBW_LEN] = {0};
    wr32le(c, BOT_CBW_SIGNATURE);
    wr32le(c + 4, tag);
    wr32le(c + 8, data_len);
    c[12] = flags;
    c[13] = 0;    // LUN 0
    c[14] = cb_len;
    memcpy(c + 15, cb, cb_len);
    const int rc = bot_check_cbw(c, block);
    if (rc != BOT_OK) return rc;
    memcpy(out, c, BOT_CBW_LEN);
    return BOT_OK;
}

int bot_build_read10(uint8_t* out, uint32_t tag, uint32_t lba, uint16_t count, uint32_t block) {
    const uint8_t cb[10] = {0x28, 0x00, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
                            0x00, (uint8_t)(count >> 8), (uint8_t)count, 0x00};
    return bot_build_cbw(out, tag, (uint32_t)count * block, BOT_FLAG_DATA_IN, cb, sizeof(cb), block);
}

int bot_check_csw(const uint8_t* csw, uint32_t tag, uint32_t* residue, uint8_t* status) {
    if (!csw || !residue || !status) return BOT_REFUSED_ARGS;
    *residue = rd32le(csw + 8);
    *status = csw[12];
    if (rd32le(csw) != BOT_CSW_SIGNATURE) return BOT_CSW_BAD_SIGNATURE;
    if (rd32le(csw + 4) != tag) return BOT_CSW_TAG_MISMATCH;
    return BOT_OK;
}

int bot_sense_retryable(const uint8_t* s, size_t n) {
    if (!s || n < 18) return 0;
    if ((s[0] & 0x7f) != 0x70 || s[7] < 6) return 0;    // fixed format, current error, ASC/ASCQ present
    const uint8_t key = s[2] & 0x0f, asc = s[12];
    if (key == 0x6) return 1;                     // UNIT ATTENTION (power on / reset / medium changed)
    if (key == 0x2 && asc == 0x04) return 1;      // NOT READY, logical unit becoming ready / initializing
    return 0;
}

int bot_parse_capacity10(const uint8_t* d, uint32_t* last_lba, uint32_t* block) {
    if (!d || !last_lba || !block) return BOT_REFUSED_ARGS;
    *last_lba = rd32be(d);
    *block = rd32be(d + 4);
    return bot_block_ok(*block) ? BOT_OK : BOT_CAPACITY_BLOCK;
}

int bot_mem_check(const BotMemInfo* mi, uint64_t buf, uint64_t len, int require_attr0) {
    if (!mi) return BOT_REFUSED_ARGS;
    if (len == 0 || (buf & (BOT_PAGE - 1)) || (len & (BOT_PAGE - 1))) return BOT_MEM_MISALIGNED;
    // mi->addr <= buf && buf + len <= mi->addr + mi->size, without overflow
    if (buf < mi->addr || buf - mi->addr > mi->size || len > mi->size - (buf - mi->addr)) return BOT_MEM_OUTSIDE_BLOCK;
    if ((mi->type & 0xff) != BOT_MEMTYPE_HEAP) return BOT_MEM_NOT_HEAP;
    if (mi->perm != BOT_PERM_RW) return BOT_MEM_NOT_RW;
    if (require_attr0 && mi->attr != 0) return BOT_MEM_ATTR;
    return BOT_OK;
}

int bot_ranges_overlap(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen) {
    const uint64_t m = ~(uint64_t)(BOT_PAGE - 1);
    if (alen == 0 || blen == 0 || a + alen < a || b + blen < b) return 1;
    const uint64_t pa = a & m, pb = b & m;
    const uint64_t ea = (a + alen + BOT_PAGE - 1) & m, eb = (b + blen + BOT_PAGE - 1) & m;
    return pa < eb && pb < ea;
}

int bot_profile_pick(const uint8_t* p, size_t n, BotIface* out) {
    if (!p || !out || n < BOT_PROFILE_SIZE) return BOT_REFUSED_ARGS;
    memset(out, 0, sizeof(*out));
    const uint32_t id = rd32le(p);
    if (id == 0 || id == 0xa5a5a5a5u) return BOT_PROFILE_ID;
    const uint8_t* ifd = p + 0x2a;    // interface descriptor (finding 1536)
    if (ifd[0] != 9 || ifd[1] != 4 || ifd[5] != 0x08 || ifd[6] != 0x06 || ifd[7] != 0x50) return BOT_PROFILE_NOT_MSC;
    if (ifd[4] != 2) return BOT_PROFILE_ENDPOINTS;
    BotIface f = {.id = id};
    int present = 0, nin = 0, nout = 0;
    for (int i = 0; i < 30; i++) {
        const uint8_t* e = p + 0x36 + 8 * i;    // {u8 present, 7-byte endpoint descriptor}
        if (e[0] == 0) continue;
        present++;
        const uint8_t addr = e[3], attr = e[4];
        const uint16_t mps = (uint16_t)(e[5] | e[6] << 8);
        if (e[0] != 1 || e[1] != 7 || e[2] != 5 || (attr & 3) != 2 || (addr & 0x0f) == 0 || (addr & 0x70) || mps == 0)
            return BOT_PROFILE_ENDPOINTS;
        if (addr & 0x80) {
            nin++;
            f.in_addr = addr;
            f.in_mps = mps;
        } else {
            nout++;
            f.out_addr = addr;
            f.out_mps = mps;
        }
    }
    if (present != 2 || nin != 1 || nout != 1) return BOT_PROFILE_ENDPOINTS;
    *out = f;
    return BOT_OK;
}
