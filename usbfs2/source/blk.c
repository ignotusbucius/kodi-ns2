// usbfs2 blk: SCSI-over-BOT block layer (see blk.h). Pure: no IPC, no libnx; the transport does the USB.
#include <string.h>
#include "blk.h"
#include "ulog.h"

enum { SCSI_PASS, SCSI_CHECK, SCSI_STOP };

static uint32_t rd32le(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void blk_kill(Blk* b, const char* why) {
    if (!b->dead) ulog(0, "blk: device marked dead (%s); no further SCSI command will be sent", why);
    b->dead = 1;
    for (int i = 0; i < BLK_LINES; i++) b->line[i].valid = 0;
}

static void ascii(char* dst, const uint8_t* src, size_t n) {
    for (size_t i = 0; i < n; i++) dst[i] = (src[i] >= 0x20 && src[i] < 0x7f) ? (char)src[i] : '.';
    dst[n] = 0;
    for (size_t i = n; i > 0 && dst[i - 1] == ' '; i--) dst[i - 1] = 0;
}

// One SCSI command through BOT: CBW (already built by bot.c) on OUT, data phase of `len` bytes on IN (copied to dst),
// CSW on IN. Any transport failure or protocol violation kills the device (STOP). CSW status 1 = CHECK.
static int scsi_exec(Blk* b, const char* name, const uint8_t* cbw, uint32_t len, uint8_t* dst, uint32_t* got) {
    if (b->dead) return SCSI_STOP;
    const uint32_t tag = rd32le(cbw + 4);
    *got = 0;
    b->cmds++;
    ulog(2, "scsi %s tag=%u len=%u", name, tag, len);
    if (b->t.send_cbw(b->t.ctx, cbw, b->block) != 0) {
        blk_kill(b, "CBW transfer failed");
        return SCSI_STOP;
    }
    const uint8_t* csw = NULL;
    uint32_t n = 0;
    if (len) {
        const uint8_t* d = NULL;
        if (b->t.recv_data(b->t.ctx, len, &d, &n) != 0 || n > len || !d) {
            blk_kill(b, "data transfer failed");
            return SCSI_STOP;
        }
        uint32_t r0;
        uint8_t s0;
        if (n == BOT_CSW_LEN && len != BOT_CSW_LEN && bot_check_csw(d, tag, &r0, &s0) == BOT_OK) {
            // The device skipped the data phase and sent its CSW (seen on some bridges after an error).
            ulog(0, "scsi %s: CSW arrived in the data phase (status %u residue %u)", name, s0, r0);
            csw = d;
            n = 0;
        } else {
            if (dst && n) memcpy(dst, d, n);
            *got = n;
        }
    }
    uint8_t cswbuf[BOT_CSW_LEN];
    if (csw) {
        memcpy(cswbuf, csw, BOT_CSW_LEN);
    } else {
        uint32_t cn = 0;
        if (b->t.recv_csw(b->t.ctx, &csw, &cn) != 0 || cn != BOT_CSW_LEN || !csw) {
            blk_kill(b, "CSW transfer failed");
            return SCSI_STOP;
        }
        memcpy(cswbuf, csw, BOT_CSW_LEN);
    }
    uint32_t residue = 0;
    uint8_t status = 0xff;
    const int c = bot_check_csw(cswbuf, tag, &residue, &status);
    ulog(2, "scsi %s CSW %s residue=%u status=%u got=%u", name, bot_reason(c), residue, status, *got);
    if (c != BOT_OK) {
        blk_kill(b, bot_reason(c));
        return SCSI_STOP;
    }
    if (status == 0) {
        if (residue != len - *got) ulog(1, "scsi %s: residue %u, received %u of %u bytes", name, residue, *got, len);
        return SCSI_PASS;
    }
    if (status == 1) return SCSI_CHECK;
    blk_kill(b, "CSW phase error / bad status");
    return SCSI_STOP;
}

// REQUEST SENSE after a CSW status 1. Returns 1 when TEST UNIT READY may be retried, 0 when not, -1 with no sense data.
static int request_sense(Blk* b, int mounted) {
    uint8_t cbw[BOT_CBW_LEN], s[18];
    if (bot_build_cbw(cbw, b->tag++, 18, BOT_FLAG_DATA_IN, bot_cb_request_sense, 6, b->block) != BOT_OK) return -1;
    memset(s, 0, sizeof(s));
    uint32_t got = 0;
    if (scsi_exec(b, "REQUEST SENSE", cbw, 18, s, &got) != SCSI_PASS || got < 18) return -1;
    ulog(0, "sense: response=0x%02x key=0x%x asc=0x%02x ascq=0x%02x", s[0] & 0x7f, s[2] & 0x0f, s[12], s[13]);
    // A mounted medium may have changed on UNIT ATTENTION; never reuse its cache.
    if (mounted && ((s[2] & 0xf) != 2 || s[12] != 4 || s[13] != 1)) return 0;
    return bot_sense_retryable(s, got);
}

static int simple(Blk* b, const char* name, const uint8_t* cb, uint8_t cb_len, uint32_t len, uint8_t* dst,
                  uint32_t* got) {
    uint8_t cbw[BOT_CBW_LEN];
    const int v = bot_build_cbw(cbw, b->tag++, len, len ? BOT_FLAG_DATA_IN : BOT_FLAG_NO_DATA, cb, cb_len, b->block);
    if (v != BOT_OK) {
        ulog(0, "REFUSED: %s CBW not built (%s); nothing sent", name, bot_reason(v));
        return SCSI_STOP;
    }
    return scsi_exec(b, name, cbw, len, dst, got);
}

// TEST UNIT READY, retried only while the sense says UNIT ATTENTION or NOT READY / becoming ready (usbbotprobe rule).
static int tur_until_ready(Blk* b, uint32_t attempts, int mounted) {
    for (uint32_t a = 1; a <= attempts && !b->dead; a++) {
        uint32_t got;
        const int r = simple(b, "TEST UNIT READY", bot_cb_test_unit_ready, 6, 0, NULL, &got);
        if (r == SCSI_PASS) return 1;
        if (r != SCSI_CHECK) break;
        const int s = request_sense(b, mounted);
        ulog(0, "TEST UNIT READY attempt %u: sense %s", a, s == 1 ? "retryable" : s == 0 ? "not retryable" : "none");
        if (s != 1 || a == attempts) break;
        if (b->t.sleep_ms) b->t.sleep_ms(b->t.ctx, BLK_TUR_RETRY_MS);
    }
    return 0;
}

int blk_init(Blk* b, const BlkTransport* t, uint8_t* cache) {
    memset(b, 0, sizeof(*b));
    b->t = *t;
    b->tag = 1;
    b->cache = cache;
    if (!cache || !t->send_cbw || !t->recv_data || !t->recv_csw) {
        b->dead = 1;
        return BLK_DEAD;
    }
    if (!tur_until_ready(b, BLK_MAX_TUR_ATTEMPTS, 0)) {
        blk_kill(b, "TEST UNIT READY did not pass");
        return BLK_DEAD;
    }
    uint8_t q[36];
    uint32_t got = 0;
    memset(q, 0, sizeof(q));
    int r = simple(b, "INQUIRY", bot_cb_inquiry, 6, 36, q, &got);
    if (r == SCSI_CHECK) (void)request_sense(b, 0);
    if (r != SCSI_PASS || got < 36) {
        blk_kill(b, "INQUIRY did not pass");
        return BLK_DEAD;
    }
    b->type = q[0] & 0x1f;
    b->rmb = q[1] >> 7;
    ascii(b->vendor, q + 8, 8);
    ascii(b->product, q + 16, 16);
    ascii(b->rev, q + 32, 4);
    ulog(0, "inquiry: type=0x%02x rmb=%u vendor='%s' product='%s' rev='%s'", b->type, b->rmb, b->vendor, b->product,
         b->rev);
    if ((q[0] >> 5) != 0 || (b->type != 0x00 && b->type != 0x0e)) {
        blk_kill(b, "not a direct-access block device");
        return BLK_DEAD;
    }
    uint8_t c[8];
    for (int attempt = 0; attempt < 2; attempt++) {
        memset(c, 0, sizeof(c));
        r = simple(b, "READ CAPACITY(10)", bot_cb_read_capacity10, 10, 8, c, &got);
        if (r != SCSI_CHECK || request_sense(b, 0) != 1) break;
    }
    uint32_t last = 0, block = 0;
    if (r != SCSI_PASS || got < 8 || bot_parse_capacity10(c, &last, &block) != BOT_OK) {
        ulog(0, "capacity: rc=%d got=%u last=%u block=%u", r, got, last, block);
        blk_kill(b, "READ CAPACITY(10) did not pass or block length unsupported");
        return BLK_DEAD;
    }
    b->block = block;
    b->capped = last == 0xffffffffu;
    b->blocks = (uint64_t)last + 1;
    ulog(0, "capacity: last_lba=%u block=%u bytes=%llu%s", last, block, (unsigned long long)(b->blocks * block),
         b->capped ? " (>= 2 TiB: only the first 2^32 blocks are addressable with READ(10))" : "");
    if (BLK_LINE_BYTES % block) {
        blk_kill(b, "block larger than a cache line");
        return BLK_DEAD;
    }
    return BLK_OK;
}

// One READ(10) of `count` blocks (count * block <= BLK_MAX_XFER) at `lba` into dst.
static int read10(Blk* b, uint64_t lba, uint32_t count, uint8_t* dst) {
    if (b->dead) return BLK_DEAD;
    if (count == 0 || count * b->block > BLK_MAX_XFER || lba + count > b->blocks || lba + count > BLK_MAX_BLOCKS_10)
        return BLK_RANGE;
    uint8_t cbw[BOT_CBW_LEN];
    const int v = bot_build_read10(cbw, b->tag++, (uint32_t)lba, (uint16_t)count, b->block);
    if (v != BOT_OK) {
        ulog(0, "REFUSED: READ(10) CBW not built (%s); nothing sent", bot_reason(v));
        return BLK_EIO;
    }
    uint32_t got = 0;
    const uint32_t len = count * b->block;
    const int r = scsi_exec(b, "READ(10)", cbw, len, dst, &got);
    if (r == SCSI_STOP) return BLK_DEAD;
    if (r == SCSI_PASS && got == len) {
        b->consec_errors = 0;
        b->bytes_read += len;
        return BLK_OK;
    }
    if (r == SCSI_CHECK) (void)request_sense(b, 0);
    ulog(0, "READ(10) lba=%llu count=%u failed (%s, %u of %u bytes)", (unsigned long long)lba, count,
         r == SCSI_CHECK ? "CSW status 1" : "short data", got, len);
    if (++b->consec_errors >= BLK_MAX_CONSEC_ERRORS) blk_kill(b, "too many consecutive read errors");
    return b->dead ? BLK_DEAD : BLK_EIO;
}

static BlkLine* line_get(Blk* b, uint64_t lba, int* rc) {
    const uint32_t per = BLK_LINE_BYTES / b->block;
    const uint64_t first = lba - lba % per;
    BlkLine* victim = &b->line[0];
    for (int i = 0; i < BLK_LINES; i++) {
        BlkLine* l = &b->line[i];
        if (l->valid && l->first == first) {
            l->age = ++b->clock;
            b->cache_hits++;
            *rc = BLK_OK;
            return l;
        }
        if (!l->valid || (victim->valid && l->age < victim->age)) victim = l;
    }
    b->cache_misses++;
    uint64_t count = b->blocks - first;
    if (count > per) count = per;
    if (first + count > BLK_MAX_BLOCKS_10) count = BLK_MAX_BLOCKS_10 - first;
    victim->valid = 0;
    *rc = read10(b, first, (uint32_t)count, b->cache + (size_t)(victim - b->line) * BLK_LINE_BYTES);
    if (*rc != BLK_OK) return NULL;
    victim->first = first;
    victim->count = (uint32_t)count;
    victim->age = ++b->clock;
    victim->valid = 1;
    return victim;
}

int blk_check(Blk* b) {
    if (b->dead) return BLK_DEAD;
    if (tur_until_ready(b, BLK_MAX_TUR_ATTEMPTS, 1)) return BLK_OK;
    blk_kill(b, "TEST UNIT READY after a state change did not pass (unplug or medium change)");
    return BLK_DEAD;
}

int blk_read(Blk* b, uint64_t lba, uint32_t count, void* dst) {
    if (b->dead) return BLK_DEAD;
    if (lba >= b->blocks || count > b->blocks - lba || lba + count > BLK_MAX_BLOCKS_10) return BLK_RANGE;
    if (count == 0) return BLK_OK;
    if (b->t.changed && b->t.changed(b->t.ctx)) {
        ulog(0, "blk: USB state changed; check readiness before the next read");
        if (blk_check(b) != BLK_OK) return BLK_DEAD;
    }
    uint8_t* out = (uint8_t*)dst;
    const uint32_t bs = b->block;
    if ((uint64_t)count * bs > BLK_LINE_BYTES) {    // large read: straight into dst, at most BLK_MAX_XFER per command
        const uint32_t max = BLK_MAX_XFER / bs;
        while (count) {
            const uint32_t n = count > max ? max : count;
            const int rc = read10(b, lba, n, out);
            if (rc != BLK_OK) return rc;
            lba += n;
            count -= n;
            out += (size_t)n * bs;
        }
        return BLK_OK;
    }
    while (count) {    // small read: through the cache
        int rc;
        BlkLine* l = line_get(b, lba, &rc);
        if (!l) return rc;
        const uint32_t off = (uint32_t)(lba - l->first);
        uint32_t n = l->count - off;
        if (n > count) n = count;
        memcpy(out, b->cache + (size_t)(l - b->line) * BLK_LINE_BYTES + (size_t)off * bs, (size_t)n * bs);
        lba += n;
        count -= n;
        out += (size_t)n * bs;
    }
    return BLK_OK;
}
