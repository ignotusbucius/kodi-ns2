// usbfs2 usbhs2: READ-ONLY USB mass-storage client for Switch 2 usb:hs, domain form (see usbhs2.h).
// Built from homebrew/usbbotprobe/source/main.c v1 (688250e7, flown twice: finding 1538), which is usbhs2probe v4
// (605a54ae, finding 1536) plus the transfer path of finding 1537. Sends, with raw serviceDispatch only (no libounce
// usbHs* code is linked; `make check`), every send in its own noipa wrapper whose statement right before the dispatch
// calls a noipa domain guard (guard_root_domain / guard_obj) and refuses when it returns false:
//   sm GetService("usb:hs")                      -> root session (once per process)
//   control cmd 0 ConvertCurrentObjectToDomain   on the root session; needs rc 0 and object id != 0
//   root   cmd 0 OpenClientSession (domain)      raw 0x28 = {msc filter 8/6/0x50, u64 0}, copy CUR_PROCESS_HANDLE,
//                                                out 1 domain object (IClientSession)             [probe unchanged]
//   client cmd 0 GetStateChangeEvent (once)      out 1 handle; wait <= 2 s, then cleared; polled for unplug
//   client cmd 1 QueryAvailableInterface         Out|HipcPointer|FixedSize 0x128 profile (once per attach attempt)
//   client cmd 2 AcquireInterface                raw {u32 profile id}, out 1 domain object (IInterfaceSession)
//   iface  cmd 4 OpenEndpoint  (OUT, then IN)    raw {u8 addr, 3 x 0, u32 maxUrb 4}, out 1 domain object each
//   ep     cmd 0 BindBuffer (EXACTLY ONCE per ep) raw {u32 dataSize, u32 0x1000, u64 data}, copy handle of a SEPARATE
//                                                report-page transfer memory (Perm_Rw); data: OUT 0x1000, IN 0x2000
//                                                (one-page data phase + CSW page; 0x11000 with -DUSBFS2_XFER_64K); all heap, svcQueryMemory-checked
//                                                right before the send (bind_buffers_ok)
//   ep     cmd 1 completion event (once per ep)  out 1 handle
//   ep     cmd 2 BatchBufferAsync                raw {u32 offset, u32 0}, In|HipcPointer {u32 size} (one URB), out id
//   ep     cmd 3 cancel                          raw {u32 xfer id}; only in teardown, only for an outstanding transfer
//   close: cancel outstanding, domain Close OUT + IN endpoints, THEN tmemClose + free, domain Close interface,
//          close endpoint events; at uhs2_close: state event, domain Close client, root session.
// The client session stays open while a drive is mounted (closing it re-enumerates the drive, finding 1536).
// READ-ONLY: the OUT endpoint only ever carries 31-byte CBWs, and ep_cmd2_batch_out re-checks the exact OUT bytes with
// bot_check_cbw (allow-list {00,03,12,25,28}, data-in only) right before the send. No data-out phase exists. No
// control transfer (iface cmd 2), no SetInterface/ResetDevice, no other command of any object. `make check` pins it.
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <switch.h>
#include "bot.h"
#include "usbhs2.h"
#include "attach_retry.h"
#include "ulog.h"

#define USBHS_NAME "usb:hs"
#define ROOT_CMD_OPEN_CLIENT 0
#define CLIENT_CMD_STATE_EVENT 0
#define CLIENT_CMD_QUERY_PROFILE 1
#define CLIENT_CMD_ACQUIRE 2
#define IFACE_CMD_OPEN_ENDPOINT 4
#define EP_CMD_BIND_BUFFER 0
#define EP_CMD_COMPLETION_EVENT 1
#define EP_CMD_BATCH 2
#define EP_CMD_CANCEL 3
#define MAX_PROFILE_CALLS (4 * USBHS2_MAX_ATTACHES)
#define MAX_BINDS (2 * USBHS2_MAX_ATTACHES)
#define PROFILE_SIZE 0x128
#define EP_PAGE 0x1000
#define OUT_DATA_SIZE 0x1000                     // CBWs only (31 bytes at +0)
#define CSW_OFFSET BOT_MAX_DATA                  // IN: data phase at +0 (<= BOT_MAX_DATA), CSW on the next page
#define IN_DATA_SIZE (BOT_MAX_DATA + EP_PAGE)    // 0x2000 (0x11000 with USBFS2_XFER_64K)
#define EP_MAX_URB 4
#define REPORT_SLOT 0x18
#define STATE_WAIT_NS 2000000000ULL
#define CBW_WAIT_NS 5000000000ULL
#define DATA_WAIT_NS 10000000000ULL
#define CANCEL_WAIT_NS 2000000000ULL
#define LOG_IPC_BYTES 0x50
#define REFUSED_NOT_DOMAIN MAKERESULT(Module_Libnx, LibnxError_BadInput)   // local refusal, nothing sent
#define REFUSED_LOCAL MAKERESULT(Module_Libnx, LibnxError_ShouldNotHappen)   // local refusal, nothing sent
#define REFUSED_SECOND_BIND MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized)   // local refusal, nothing sent
#define REFUSED_MEMORY MAKERESULT(Module_Libnx, LibnxError_OutOfMemory)     // local refusal, nothing sent

_Static_assert(BOT_MEMTYPE_HEAP == MemType_Heap, "bot.h heap type = libnx MemType_Heap");
_Static_assert(BOT_PERM_RW == Perm_Rw, "bot.h RW = libnx Perm_Rw");
_Static_assert(BOT_PAGE == EP_PAGE && BOT_PROFILE_SIZE == PROFILE_SIZE, "page and profile sizes");
_Static_assert(CSW_OFFSET >= BOT_MAX_DATA && CSW_OFFSET + BOT_CSW_LEN <= IN_DATA_SIZE, "data and CSW regions disjoint");
_Static_assert(IN_DATA_SIZE % EP_PAGE == 0 && OUT_DATA_SIZE % EP_PAGE == 0 && BOT_CBW_LEN <= OUT_DATA_SIZE, "4 KiB sizes");
_Static_assert(EP_MAX_URB * REPORT_SLOT <= EP_PAGE, "report ring fits the report page");

// The 0x20-byte filter, field order as in finding 1533. A criterion is used only when its `present` byte is 1.
typedef struct { u8 present; u8 pad; u16 value; } FilterU16;
typedef struct { u8 present; u8 value; } FilterU8;
typedef struct {
    FilterU16 idVendor, idProduct, bcdDevice_Min, bcdDevice_Max;          // 0x00..0x0f
    FilterU8 bDeviceClass, bDeviceSubClass, bDeviceProtocol;              // 0x10..0x15
    FilterU8 bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol;     // 0x16..0x1b
    u8 tail[4];                                                           // 0x1c..0x1f, always 0
} ProbeFilter;
typedef struct { ProbeFilter filter; u64 arg; } OpenClientIn;             // root cmd 0 in raw data (1533 corrected)
_Static_assert(sizeof(ProbeFilter) == 0x20, "1533: 0x20-byte filter");
_Static_assert(offsetof(ProbeFilter, bInterfaceClass) == 0x16, "filter layout");
_Static_assert(offsetof(ProbeFilter, bInterfaceProtocol) == 0x1a, "filter layout");
_Static_assert(sizeof(OpenClientIn) == 0x28 && offsetof(OpenClientIn, filter) == 0 && offsetof(OpenClientIn, arg) == 0x20,
               "1533 corrected: filter at +0, u64 at +0x20, in raw 0x28 (0x38 with the CMIF header)");

static const OpenClientIn kOpenIn = {
    .filter = {
        .bInterfaceClass = {.present = 1, .value = 0x08},     // mass storage
        .bInterfaceSubClass = {.present = 1, .value = 0x06},  // SCSI transparent command set
        .bInterfaceProtocol = {.present = 1, .value = 0x50},  // bulk-only transport
    },
    .arg = 0,
};

// 1537 raw layouts.
typedef struct { u8 addr; u8 pad[3]; u32 max_urb; } OpenEpIn;
typedef struct { u32 data_size; u32 report_size; u64 data_addr; } BindIn;
typedef struct { u32 offset; u32 unk; } BatchIn;
_Static_assert(sizeof(OpenEpIn) == 8 && offsetof(OpenEpIn, max_urb) == 4, "1537 OpenEndpoint raw");
_Static_assert(sizeof(BindIn) == 0x10 && offsetof(BindIn, data_addr) == 8, "1537 BindBuffer raw");
_Static_assert(sizeof(BatchIn) == 8, "1537 BatchBufferAsync raw");

typedef struct {
    const char* name;
    u8 addr;
    u32 dsize;             // bound data size: OUT_DATA_SIZE or IN_DATA_SIZE
    Service s;             // IEndpointSession: a domain object of the root session
    u8* dpage;             // data buffer: heap (aligned_alloc), device-mapped by the server at BindBuffer
    u8* rpage;             // report page: a SEPARATE heap page, shared as transfer memory (report ring)
    TransferMemory tm;
    bool open_sent;
    bool tm_made;
    bool bind_sent;        // set before the one BindBuffer send and never cleared: no second bind, not even a retry
    bool bound;
    bool event_sent;
    Handle event;
    bool outstanding;      // a BatchBufferAsync was accepted and no report for it has been read yet
    u32 xfer_id;
    u32 attach_no;         // the attach (g_attaches) whose OpenEndpoint created this object; a bind needs the current one
} Ep;

static u8 g_profile[PROFILE_SIZE];
static u8 g_tls[0x100];                // the IPC buffer right after a dispatch (LOG_IPC_BYTES are logged)
static u32 g_copies, g_moves;          // handle counts in that reply (valid only when the dispatch succeeded)
static Handle g_copy0, g_move0;
static BotIface g_pick;
static Service g_root, g_client, g_iface;
static Ep g_out = {.name = "OUT", .event = INVALID_HANDLE};
static Ep g_in = {.name = "IN", .event = INVALID_HANDLE};
static Handle g_state_event = INVALID_HANDLE;
static bool g_open_tried, g_open, g_attached, g_latched;
static u32 g_bind_sends, g_attaches;
static bool g_state_blk;    // state indication pending for the block layer
static u32 g_last_detached_id;         // the interface id released last: never acquired again (same attachment)
static AttachRetry g_retry; // query credits and minimum gap; separate from the interface-acquisition cap
static u16 g_vid, g_pid;

// Call right after a dispatch returns and before logging (logging may do IPC and overwrite the buffer). If the
// dispatch failed in the kernel, the buffer still holds our request, so the handle fields are used only on rc 0.
static void capture(void) {
    memcpy(g_tls, armGetTls(), sizeof(g_tls));
    HipcResponse r = hipcParseResponse(armGetTls());
    g_copies = r.num_copy_handles;
    g_moves = r.num_move_handles;
    g_copy0 = g_copies ? r.copy_handles[0] : INVALID_HANDLE;
    g_move0 = g_moves ? r.move_handles[0] : INVALID_HANDLE;
}

static bool refused(Result rc) {
    return rc == REFUSED_NOT_DOMAIN || rc == REFUSED_LOCAL || rc == REFUSED_SECOND_BIND || rc == REFUSED_MEMORY;
}

static void log_reply(int level, const char* step, Result rc) {
    if (refused(rc)) {
        ulog(0, "%s NOT SENT: local refusal rc=0x%08x", step, rc);
        return;
    }
    ulog(R_FAILED(rc) ? 0 : level, "%s rc=0x%08x copies=%u moves=%u copy0=0x%08x move0=0x%08x", step, rc, g_copies,
         g_moves, g_copy0, g_move0);
    ulog_hex(R_FAILED(rc) ? 0 : level + 1, "  ipcbuf", g_tls, LOG_IPC_BYTES);
}

static u16 rd16(const u8* p) { u16 v; memcpy(&v, p, sizeof(v)); return v; }
static u32 rd32(const u8* p) { u32 v; memcpy(&v, p, sizeof(v)); return v; }
static u64 rd64(const u8* p) { u64 v; memcpy(&v, p, sizeof(v)); return v; }

// A domain reply as libounce cmifParseResponse(is_domain) reads it: at the 16-aligned data start CmifDomainOutHeader
// {u32 num_out_objects, 3 x u32 padding}, then CmifOutHeader {"SFCO", version, result, token}, then the out raw data
// (none for the object-returning commands), then the out object ids. Decoded from the captured copy.
typedef struct { bool ok; u32 num_out_objects, magic, result, object0; } DomainReply;
static DomainReply domain_reply(void) {
    DomainReply d = {0};
    HipcResponse r = hipcParseResponse(g_tls);
    size_t start = (((size_t)((const u8*)r.data_words - g_tls)) + 15) & ~(size_t)15;
    if (start + 0x24 > sizeof(g_tls)) return d;
    d.ok = true;
    d.num_out_objects = rd32(g_tls + start);
    d.magic = rd32(g_tls + start + 0x10);
    d.result = rd32(g_tls + start + 0x18);
    d.object0 = rd32(g_tls + start + 0x20);
    return d;
}

static __attribute__((noipa)) void close_obj(const char* what, Service* s);

// An object-returning reply is used only as exactly one domain object of the root session, with no handle in it.
static bool out_object_ok(const char* step, Result rc, Service* obj) {
    DomainReply d = domain_reply();
    ulog(1, "%s domain out: decoded=%u num_out_objects=%u magic=0x%08x result=0x%08x object0=%u object=%u "
         "session=0x%08x own=%u", step, d.ok ? 1 : 0, d.num_out_objects, d.magic, d.result, d.object0, obj->object_id,
         obj->session, obj->own_handle);
    if (R_SUCCEEDED(rc) && d.ok && d.num_out_objects == 1 && d.object0 == obj->object_id &&
        serviceIsDomainSubservice(obj) && obj->session == g_root.session && g_copies == 0 && g_moves == 0)
        return true;
    ulog(0, "%s reply is not exactly one domain object of the root session; object not used", step);
    // the server did create one object (a successful reply naming exactly one nonzero id): close it, so it is not
    // leaked server-side (its kernel events: HOTPLUG.md). close_obj never comes back here.
    if (R_SUCCEEDED(rc) && d.ok && d.result == 0 && d.num_out_objects == 1 && d.object0 != 0 && serviceIsDomain(&g_root)) {
        Service stray = {.session = g_root.session, .own_handle = 0, .object_id = d.object0,
                         .pointer_buffer_size = g_root.pointer_buffer_size};
        close_obj(step, &stray);
    }
    *obj = (Service){0};
    return false;
}

static Handle one_handle(void) {
    return (g_copies + g_moves == 1) ? (g_copies ? g_copy0 : g_move0) : INVALID_HANDLE;
}

// Profile fields per 1533 r5 / 1536: +0 id, +0x10 port, +0x18 device descriptor, +0x2a interface descriptor,
// +0x33 speed, +0x34 u16 endpoint count, +0x36 30 x {u8 present, 7-byte endpoint descriptor}.
static void decode_profile(const u8* p) {
    ulog(0, "profile id=%u q04=0x%016llx port=%u w14=0x%08x speed=%u h34=0x%04x", rd32(p),
         (unsigned long long)rd64(p + 0x04), rd32(p + 0x10), rd32(p + 0x14), p[0x33], rd16(p + 0x34));
    const u8* d = p + 0x18;
    ulog(0, "  dev len=%u type=%u bcdUSB=%04x class=%02x sub=%02x proto=%02x mps0=%u vid=%04x pid=%04x bcd=%04x "
         "nconf=%u", d[0], d[1], rd16(d + 2), d[4], d[5], d[6], d[7], rd16(d + 8), rd16(d + 10), rd16(d + 12), d[17]);
    const u8* c = p + 0x2a;
    ulog(0, "  if len=%u type=%u num=%u alt=%u neps=%u class=%02x sub=%02x proto=%02x istr=%u", c[0], c[1], c[2], c[3],
         c[4], c[5], c[6], c[7], c[8]);
    for (int i = 0; i < 30; i++) {
        const u8* e = p + 0x36 + 8 * i;
        if (e[0])
            ulog(1, "  ep[%d] present=%u len=%u type=%u addr=%02x attr=%02x mps=%u interval=%u", i, e[0], e[1], e[2],
                 e[3], e[4], rd16(e + 5), e[7]);
    }
}

// The only client: never with an empty or shifted filter, never one that could match HID (class 3). Checks the raw
// 0x28 bytes that root cmd 0 sends: 0x00..0x15 zero, 0x16..0x1b = 01 08 01 06 01 50, 0x1c..0x27 zero.
static bool raw_is_msc(const OpenClientIn* in) {
    static const u8 want[6] = {0x01, 0x08, 0x01, 0x06, 0x01, 0x50};
    const volatile u8* b = (const volatile u8*)in;
    for (size_t i = 0; i < sizeof(*in); i++)
        if (b[i] != ((i >= 0x16 && i < 0x1c) ? want[i - 0x16] : 0)) return false;
    return true;
}

// ---- the two domain guards. noipa: each is a real call in the machine code, so `make check` can prove from usbhs2.o
// that every send is unreachable without the guard's `true` edge.
// The root session after conversion: our one usb:hs session, a domain (own handle + object id).
static __attribute__((noipa)) bool guard_root_domain(Service* s) { return s == &g_root && serviceIsDomain(s); }
// Every other object: a domain subservice (no own handle, object id != 0) of that same root session.
static __attribute__((noipa)) bool guard_obj(Service* s) {
    return serviceIsDomainSubservice(s) && s->session == g_root.session && serviceIsDomain(&g_root);
}

// The ONLY domain conversion: CMIF control cmd 0 ConvertCurrentObjectToDomain on our own root session. Refuses a session
// we do not own (serviceConvertToDomain would first clone it, a second usb:hs session) or one that already is a domain.
static __attribute__((noipa)) Result root_convert_domain(Service* root) {
    if (root != &g_root || !serviceIsActive(root) || !root->own_handle || root->object_id) return REFUSED_NOT_DOMAIN;
    Result rc = serviceConvertToDomain(root);
    capture();
    return rc;
}

// The ONLY root cmd 0 send. `root` must be a domain, so serviceDispatchIn builds the domain in-header and reads the
// reply's out object as a domain object id. A non-domain out object (a new usb:hs session, finding 1534 r2) is never
// asked for.
static __attribute__((noipa)) Result root_cmd0_domain(Service* root, const OpenClientIn* in, Service* client) {
    if (!guard_root_domain(root) || !serviceIsDomain(root)) return REFUSED_NOT_DOMAIN;
    Result rc = serviceDispatchIn(root, ROOT_CMD_OPEN_CLIENT, *in, .in_num_handles = 1,
                                  .in_handles = {CUR_PROCESS_HANDLE}, .out_num_objects = 1, .out_objects = client, );
    capture();
    return rc;
}

// ---- the only sends to the client / interface / endpoint objects: one noipa wrapper per command id. In each, the
// statement right before the dispatch refuses unless guard_obj() accepts the object; the repeated inline
// serviceIsDomainSubservice() lets the compiler drop libounce's non-domain request form, so only the domain form is
// compiled (`make check` check 5) ----

static __attribute__((noipa)) Result client_cmd0_event(Service* client) {
    static bool sent;
    if (sent) return REFUSED_LOCAL;
    sent = true;
    ulog(1, "pre client cmd0 GetStateChangeEvent");
    if (!guard_obj(client) || !serviceIsDomainSubservice(client)) return REFUSED_NOT_DOMAIN;
    Result rc = serviceDispatch(client, CLIENT_CMD_STATE_EVENT);
    capture();
    return rc;
}

static __attribute__((noipa)) Result client_cmd1_profile(Service* client) {
    static u32 calls;
    if (calls >= MAX_PROFILE_CALLS) return REFUSED_LOCAL;
    calls++;
    memset(g_profile, 0xa5, sizeof(g_profile));    // bytes the server did not write stay a5
    ulog(1, "pre client cmd1 QueryAvailableInterface out ptr fixed 0x%x", PROFILE_SIZE);
    if (!guard_obj(client) || !serviceIsDomainSubservice(client)) return REFUSED_NOT_DOMAIN;
    Result rc = serviceDispatch(client, CLIENT_CMD_QUERY_PROFILE,
                                .buffer_attrs = {SfBufferAttr_Out | SfBufferAttr_HipcPointer | SfBufferAttr_FixedSize},
                                .buffers = {{g_profile, sizeof(g_profile)}}, );
    capture();
    return rc;
}

static __attribute__((noipa)) Result client_cmd2_acquire(Service* client, Service* iface) {
    static u32 sends;
    if (sends >= USBHS2_MAX_ATTACHES || g_pick.id == 0 || serviceIsActive(iface)) return REFUSED_LOCAL;
    sends++;
    const u32 id = g_pick.id;
    ulog(1, "pre client cmd2 AcquireInterface id=%u out_objects=1", id);
    if (!guard_obj(client) || !serviceIsDomainSubservice(client)) return REFUSED_NOT_DOMAIN;
    Result rc = serviceDispatchIn(client, CLIENT_CMD_ACQUIRE, id, .out_num_objects = 1, .out_objects = iface, );
    capture();
    return rc;
}

static __attribute__((noipa)) Result iface_cmd4_open_ep(Service* iface, Ep* e) {
    if (e->open_sent || serviceIsActive(&e->s)) return REFUSED_LOCAL;
    if (!(e == &g_out && e->addr == g_pick.out_addr && !(e->addr & 0x80)) &&
        !(e == &g_in && e->addr == g_pick.in_addr && (e->addr & 0x80)))
        return REFUSED_LOCAL;
    e->open_sent = true;
    const OpenEpIn in = {.addr = e->addr, .max_urb = EP_MAX_URB};
    ulog(1, "pre iface cmd4 OpenEndpoint %s addr=0x%02x maxUrb=%u out_objects=1", e->name, in.addr, in.max_urb);
    if (!guard_obj(iface) || !serviceIsDomainSubservice(iface)) return REFUSED_NOT_DOMAIN;
    Result rc = serviceDispatchIn(iface, IFACE_CMD_OPEN_ENDPOINT, in, .out_num_objects = 1, .out_objects = &e->s, );
    capture();
    return rc;
}

// svcQueryMemory one page and log it. false when the query itself fails.
static bool query_page(const char* ep, const char* what, const void* p, BotMemInfo* out) {
    MemoryInfo mi = {0};
    u32 pi = 0;
    Result rc = svcQueryMemory(&mi, &pi, (u64)(uintptr_t)p);
    ulog(1, "%s %s page %p: query rc=0x%08x block addr=0x%llx size=0x%llx type=0x%x attr=0x%x perm=0x%x ipc=%u dev=%u",
         ep, what, p, rc, (unsigned long long)mi.addr, (unsigned long long)mi.size, mi.type, mi.attr, mi.perm,
         mi.ipc_refcount, mi.device_refcount);
    *out = (BotMemInfo){.addr = mi.addr, .size = mi.size, .type = mi.type, .attr = mi.attr, .perm = mi.perm};
    return R_SUCCEEDED(rc);
}

// Before tmemCreateFromMemory: data buffer and report page heap, RW, attribute 0, inside their blocks, 4 KiB aligned,
// no shared page.
static __attribute__((noipa)) bool pages_ok_before_tmem(Ep* e) {
    BotMemInfo d, r;
    if (!query_page(e->name, "data", e->dpage, &d) || !query_page(e->name, "report", e->rpage, &r)) return false;
    const int dv = bot_mem_check(&d, (uintptr_t)e->dpage, e->dsize, 1);
    const int rv = bot_mem_check(&r, (uintptr_t)e->rpage, EP_PAGE, 1);
    const int ov = bot_ranges_overlap((uintptr_t)e->dpage, e->dsize, (uintptr_t)e->rpage, EP_PAGE);
    ulog(1, "%s pages before tmem: data %s, report %s, overlap=%d", e->name, bot_reason(dv), bot_reason(rv), ov);
    return dv == BOT_OK && rv == BOT_OK && !ov;
}

// The BindBuffer memory guard, called by ep_cmd0_bind immediately before the send: the data buffer heap, RW,
// attribute 0 (not borrowed / IPC / device-shared / uncached), wholly inside its block; the report page heap, RW, inside
// its block (borrowed is expected now that it is transfer memory); both 4 KiB aligned; no shared page; the transfer
// memory made over exactly the report page.
static __attribute__((noipa)) bool bind_buffers_ok(Ep* e) {
    BotMemInfo d, r;
    if (!e->tm_made || e->tm.handle == INVALID_HANDLE || e->tm.size != EP_PAGE || e->tm.perm != Perm_Rw) return false;
    if (e->dsize != (e == &g_in ? IN_DATA_SIZE : OUT_DATA_SIZE)) return false;
    if (!query_page(e->name, "data", e->dpage, &d) || !query_page(e->name, "report", e->rpage, &r)) return false;
    const int dv = bot_mem_check(&d, (uintptr_t)e->dpage, e->dsize, 1);
    const int rv = bot_mem_check(&r, (uintptr_t)e->rpage, EP_PAGE, 0);
    const int ov = bot_ranges_overlap((uintptr_t)e->dpage, e->dsize, (uintptr_t)e->rpage, EP_PAGE);
    ulog(1, "%s bind guard: data %s, report %s (attr=0x%x, borrowed expected), overlap=%d", e->name, bot_reason(dv),
         bot_reason(rv), r.attr, ov);
    return dv == BOT_OK && rv == BOT_OK && !ov;
}

// The ONLY BindBuffer send. At most once per endpoint object and at most MAX_BINDS per process: a second bind unmaps
// the old buffer and aborts the usb sysmodule if that fails (1537). bind_sent is set before the send, so a failed bind
// is never retried. The memory guard runs right before the send; when it returns false nothing is sent.
static __attribute__((noipa)) Result ep_cmd0_bind(Ep* e) {
    if (e->bind_sent || g_bind_sends >= MAX_BINDS) return REFUSED_SECOND_BIND;
    if (!e->open_sent || e->attach_no != g_attaches || !serviceIsActive(&e->s)) return REFUSED_SECOND_BIND;
    if (!bind_buffers_ok(e)) return REFUSED_MEMORY;
    e->bind_sent = true;
    g_bind_sends++;
    const BindIn in = {.data_size = e->dsize, .report_size = EP_PAGE, .data_addr = (u64)(uintptr_t)e->dpage};
    ulog(1, "pre %s ep cmd0 BindBuffer data=0x%llx size=0x%x report=0x%x copy=0x%08x", e->name,
         (unsigned long long)in.data_addr, in.data_size, in.report_size, e->tm.handle);
    if (!guard_obj(&e->s) || !serviceIsDomainSubservice(&e->s)) return REFUSED_NOT_DOMAIN;
    Result rc = serviceDispatchIn(&e->s, EP_CMD_BIND_BUFFER, in, .in_num_handles = 1, .in_handles = {e->tm.handle}, );
    capture();
    return rc;
}

static __attribute__((noipa)) Result ep_cmd1_event(Ep* e) {
    if (!e->bound || e->event_sent || e->event != INVALID_HANDLE) return REFUSED_LOCAL;
    e->event_sent = true;
    ulog(1, "pre %s ep cmd1 completion event", e->name);
    if (!guard_obj(&e->s) || !serviceIsDomainSubservice(&e->s)) return REFUSED_NOT_DOMAIN;
    Result rc = serviceDispatch(&e->s, EP_CMD_COMPLETION_EVENT);
    capture();
    return rc;
}

// The ONLY two BatchBufferAsync sends, one URB each, one transfer per endpoint at a time.
// OUT (ep_cmd2_batch_out): exactly the 31 CBW bytes at +0, re-checked here with bot_check_cbw on the bytes that go out
// (with the block length the CBW was built for); the send is unreachable unless bot_check_cbw returned BOT_OK. There is
// no other OUT send, so nothing but an allow-listed, data-in-or-no-data CBW ever goes to the device.
static __attribute__((noipa)) Result ep_cmd2_batch_out(u32 block) {
    Ep* e = &g_out;
    if (!e->bound || e->event == INVALID_HANDLE || e->outstanding) return REFUSED_LOCAL;
    const int v = bot_check_cbw(g_out.dpage, block);
    if (v != BOT_OK) return REFUSED_LOCAL;
    armDCacheFlush(g_out.dpage, BOT_CBW_LEN);
    const BatchIn in = {.offset = 0, .unk = 0};
    const u32 sizes[1] = {BOT_CBW_LEN};
    u32 xid = 0;
    ulog(2, "pre OUT ep cmd2 BatchBufferAsync offset=0 unk=0 sizes={%u}", sizes[0]);
    if (!guard_obj(&e->s) || !serviceIsDomainSubservice(&e->s)) return REFUSED_NOT_DOMAIN;
    Result rc = serviceDispatchInOut(&e->s, EP_CMD_BATCH, in, xid, .buffer_attrs = {SfBufferAttr_In | SfBufferAttr_HipcPointer},
                                     .buffers = {{sizes, sizeof(sizes)}}, );
    capture();
    if (R_SUCCEEDED(rc)) {
        e->outstanding = true;
        e->xfer_id = xid;
    }
    return rc;
}

// IN (ep_cmd2_batch_in): a data phase at +0 of at most BOT_MAX_DATA bytes, or the 13-byte CSW at +CSW_OFFSET; the
// region is filled with a5 first so bytes the device did not write show.
static __attribute__((noipa)) Result ep_cmd2_batch_in(u32 offset, u32 size) {
    Ep* e = &g_in;
    if (!e->bound || e->event == INVALID_HANDLE || e->outstanding) return REFUSED_LOCAL;
    if (!(offset == 0 && size > 0 && size <= BOT_MAX_DATA) && !(offset == CSW_OFFSET && size == BOT_CSW_LEN))
        return REFUSED_LOCAL;
    memset(g_in.dpage + offset, 0xa5, size);
    armDCacheFlush(g_in.dpage + offset, size);
    const BatchIn in = {.offset = offset, .unk = 0};
    const u32 sizes[1] = {size};
    u32 xid = 0;
    ulog(2, "pre IN ep cmd2 BatchBufferAsync offset=0x%x unk=0 sizes={%u}", in.offset, sizes[0]);
    if (!guard_obj(&e->s) || !serviceIsDomainSubservice(&e->s)) return REFUSED_NOT_DOMAIN;
    Result rc = serviceDispatchInOut(&e->s, EP_CMD_BATCH, in, xid, .buffer_attrs = {SfBufferAttr_In | SfBufferAttr_HipcPointer},
                                     .buffers = {{sizes, sizeof(sizes)}}, );
    capture();
    if (R_SUCCEEDED(rc)) {
        e->outstanding = true;
        e->xfer_id = xid;
    }
    return rc;
}

static __attribute__((noipa)) Result ep_cmd3_cancel(Ep* e) {
    if (!e->outstanding) return REFUSED_LOCAL;
    const u32 in = e->xfer_id;
    ulog(1, "pre %s ep cmd3 cancel xfer_id=%u", e->name, in);
    if (!guard_obj(&e->s) || !serviceIsDomainSubservice(&e->s)) return REFUSED_NOT_DOMAIN;
    Result rc = serviceDispatchIn(&e->s, EP_CMD_CANCEL, in);
    capture();
    return rc;
}

static __attribute__((noipa)) void close_obj(const char* what, Service* s) {
    if (!guard_obj(s) || !serviceIsDomainSubservice(s)) return;
    ulog(1, "pre close %s object %u (domain Close)", what, s->object_id);
    serviceClose(s);
    ulog(1, "close %s done", what);
}

static __attribute__((noipa)) void close_root(Service* root) {
    if (root != &g_root || !serviceIsActive(root)) return;
    ulog(1, "pre close root session handle=0x%08x", root->session);
    serviceClose(root);
    ulog(1, "close root session done");
}

static void close_event(const char* what, Handle* h) {
    if (*h == INVALID_HANDLE) return;
    ulog(1, "pre close %s event 0x%08x", what, *h);
    Result rc = svcCloseHandle(*h);
    ulog(1, "close %s event rc=0x%08x", what, rc);
    *h = INVALID_HANDLE;
}

// ---- transfers ----

// Read every report slot with a nonzero word 0 and zero its word 0 so the server can reuse it. Returns the number of
// such slots; *res gets word +4, *id word 0 and *n the q10 (transferred bytes, finding 1538) of the last one.
static int reap(Ep* e, const char* what, u32* res, u32* id, u64* n) {
    volatile u32* ring = (volatile u32*)e->rpage;
    int found = 0;
    for (int i = 0; i < EP_MAX_URB; i++) {
        if (ring[i * (REPORT_SLOT / 4)] == 0) continue;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);    // word 0 is written last by the server
        u8 raw[REPORT_SLOT];
        for (int k = 0; k < REPORT_SLOT; k++) raw[k] = ((volatile u8*)e->rpage)[i * REPORT_SLOT + k];
        ulog(2, "%s %s report[%d] id=%u (xfer_id=%u) w04=0x%08x q08=0x%llx q10=%llu", e->name, what, i, rd32(raw),
             e->xfer_id, rd32(raw + 4), (unsigned long long)rd64(raw + 8), (unsigned long long)rd64(raw + 0x10));
        *res = rd32(raw + 4);
        *id = rd32(raw);
        *n = rd64(raw + 0x10);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        ring[i * (REPORT_SLOT / 4)] = 0;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        found++;
    }
    return found;
}

// One BatchBufferAsync on one endpoint: submit, wait, read the report. true only when exactly one report came back,
// its id is the returned transfer id, its word +4 (the S1 analogue: Result) is 0 and it moved <= size bytes. false =
// the caller stops all SCSI (blk marks the device dead); a transfer whose report never came stays `outstanding` and
// teardown cancels it.
static __attribute__((noipa)) bool xfer(Ep* e, u32 offset, u32 size, u32 block, u64 wait_ns, const char* what, u32* got) {
    u32 res = 0, id = 0;
    u64 n = 0;
    *got = 0;
    if (reap(e, "stale", &res, &id, &n) != 0) {
        ulog(0, "stop: %s report ring held a report before %s", e->name, what);
        return false;
    }
    Result rc = svcResetSignal(e->event);
    ulog(2, "%s %s pre-clear completion event rc=0x%08x (0xfa01 = was not signalled)", e->name, what, rc);
    rc = (e == &g_out && offset == 0 && size == BOT_CBW_LEN) ? ep_cmd2_batch_out(block)
         : (e == &g_in) ? ep_cmd2_batch_in(offset, size) : REFUSED_LOCAL;
    if (R_FAILED(rc)) {
        log_reply(0, "ep cmd2 BatchBufferAsync", rc);
        ulog(0, "stop: %s %s not submitted", e->name, what);
        return false;
    }
    ulog(2, "%s %s BatchBufferAsync rc=0x%08x xfer_id=%u", e->name, what, rc, e->xfer_id);
    rc = svcWaitSynchronizationSingle(e->event, wait_ns);
    if (R_FAILED(rc)) {
        ulog(0, "stop: %s %s wait rc=0x%08x: did not complete (left outstanding; teardown cancels)", e->name, what, rc);
        return false;
    }
    svcResetSignal(e->event);
    armDCacheFlush(e->dpage + offset, size);
    const int k = reap(e, what, &res, &id, &n);
    if (k == 0) {
        ulog(0, "stop: %s %s event signalled but no report (left outstanding; teardown cancels)", e->name, what);
        return false;
    }
    e->outstanding = false;
    if (k != 1 || res != 0 || id != e->xfer_id || n > size) {
        ulog(0, "stop: %s %s reports=%d id=%u (xfer %u) w04=0x%08x n=%llu (want 1 report, w04 0, n <= %u)", e->name, what,
             k, id, e->xfer_id, res, (unsigned long long)n, size);
        return false;
    }
    *got = (u32)n;
    return true;
}

static u32 g_cbw_block;

static int t_send_cbw(void* ctx, const uint8_t* cbw, uint32_t block) {
    (void)ctx;
    if (!g_attached || !g_out.bound || !g_out.dpage) return -1;
    memcpy(g_out.dpage, cbw, BOT_CBW_LEN);
    g_cbw_block = block;
    u32 got = 0;
    return xfer(&g_out, 0, BOT_CBW_LEN, block, CBW_WAIT_NS, "CBW", &got) && got == BOT_CBW_LEN ? 0 : -1;
}

static int t_recv_data(void* ctx, uint32_t len, const uint8_t** data, uint32_t* got) {
    (void)ctx;
    if (!g_attached || !g_in.bound || len == 0 || len > BOT_MAX_DATA) return -1;
    if (!xfer(&g_in, 0, len, g_cbw_block, DATA_WAIT_NS, "DATA", got)) return -1;
    *data = g_in.dpage;
    return 0;
}

static int t_recv_csw(void* ctx, const uint8_t** csw, uint32_t* got) {
    (void)ctx;
    if (!g_attached || !g_in.bound) return -1;
    if (!xfer(&g_in, CSW_OFFSET, BOT_CSW_LEN, g_cbw_block, DATA_WAIT_NS, "CSW", got)) return -1;
    *csw = g_in.dpage + CSW_OFFSET;
    return 0;
}

static int t_changed(void* ctx) {
    (void)ctx;
    return uhs2_state_changed();
}

static void t_sleep(void* ctx, uint32_t ms) {
    (void)ctx;
    svcSleepThread((s64)ms * 1000000);
}

void uhs2_transport(BlkTransport* t) {
    *t = (BlkTransport){.ctx = NULL, .send_cbw = t_send_cbw, .recv_data = t_recv_data, .recv_csw = t_recv_csw,
                        .changed = t_changed, .sleep_ms = t_sleep};
}

static void state_poll(void) {
    if (g_state_event == INVALID_HANDLE) return;
    if (R_FAILED(svcWaitSynchronizationSingle(g_state_event, 0))) return;
    svcResetSignal(g_state_event);
    g_state_blk = true;
    attach_event(&g_retry);
}

int uhs2_state_changed(void) {
    state_poll();
    const bool c = g_state_blk;
    g_state_blk = false;
    return c;
}

static u64 attach_gap_ticks(void) { return armNsToTicks(USBHS2_ATTACH_GAP_MS * 1000000ULL); }

int uhs2_attach_due(void) {
    state_poll();
    return attach_due(&g_retry, armGetSystemTick(), attach_gap_ticks(),
                      g_open && !g_attached && !g_latched && g_attaches < USBHS2_MAX_ATTACHES);
}

uint32_t uhs2_attach_count(void) { return g_attaches; }

int uhs2_attached(void) { return g_attached; }

void uhs2_identity(uint32_t* iface_id, uint16_t* vid, uint16_t* pid) {
    *iface_id = g_pick.id;
    *vid = g_vid;
    *pid = g_pid;
}

// ---- setup / teardown ----

// Per endpoint, after OpenEndpoint: a heap data buffer and a SEPARATE heap report page, the report page as Perm_Rw
// transfer memory, the one BindBuffer, the completion event.
static __attribute__((noipa)) bool ep_setup(Ep* e, u32 dsize) {
    e->dsize = dsize;
    e->dpage = aligned_alloc(EP_PAGE, dsize);
    e->rpage = aligned_alloc(EP_PAGE, EP_PAGE);
    ulog(1, "%s aligned_alloc data=%p (0x%x) report=%p", e->name, e->dpage, dsize, e->rpage);
    if (!e->dpage || !e->rpage) return false;
    memset(e->dpage, 0, dsize);
    memset(e->rpage, 0, EP_PAGE);
    if (!pages_ok_before_tmem(e)) {
        ulog(0, "stop: %s pages refused before tmem; no tmem, no bind", e->name);
        g_latched = true;
        return false;
    }
    Result rc = tmemCreateFromMemory(&e->tm, e->rpage, EP_PAGE, Perm_Rw);
    ulog(1, "%s tmemCreateFromMemory rc=0x%08x handle=0x%08x", e->name, rc, e->tm.handle);
    if (R_FAILED(rc)) {
        g_latched = true;
        return false;
    }
    e->tm_made = true;
    rc = ep_cmd0_bind(e);
    log_reply(1, "ep cmd0 BindBuffer", rc);
    if (R_FAILED(rc)) {
        g_latched = true;    // never another bind in this process, not even on a new endpoint object
        return false;
    }
    e->bound = true;
    rc = ep_cmd1_event(e);
    log_reply(1, "ep cmd1 completion event", rc);
    if (R_SUCCEEDED(rc)) e->event = one_handle();
    ulog(1, "%s completion event=0x%08x", e->name, e->event);
    return e->event != INVALID_HANDLE;
}

// Cancel a transfer still outstanding and wait for its report; buffers are freed only after the endpoint's domain
// Close and only when no transfer is outstanding (otherwise kept until process exit).
static __attribute__((noipa)) void ep_cancel_outstanding(Ep* e) {
    if (!e->outstanding) return;
    Result rc = ep_cmd3_cancel(e);
    log_reply(1, "ep cmd3 cancel", rc);
    if (R_FAILED(rc)) return;
    rc = svcWaitSynchronizationSingle(e->event, CANCEL_WAIT_NS);
    ulog(1, "%s cancel wait rc=0x%08x", e->name, rc);
    u32 res = 0, id = 0;
    u64 n = 0;
    if (R_SUCCEEDED(rc) && reap(e, "cancel", &res, &id, &n) > 0) e->outstanding = false;
}

static __attribute__((noipa)) void ep_release(Ep* e) {
    if (!e->dpage && !e->rpage) return;
    if (e->tm_made) {
        Result rc = tmemClose(&e->tm);
        ulog(1, "%s tmemClose rc=0x%08x", e->name, rc);
        e->tm_made = false;
    }
    if (e->outstanding) {
        ulog(0, "%s buffers kept: a transfer was not confirmed finished", e->name);
        e->dpage = e->rpage = NULL;
        return;
    }
    free(e->rpage);
    free(e->dpage);
    e->dpage = e->rpage = NULL;
    ulog(1, "%s buffers freed", e->name);
}

// A fresh Ep for the next attach. Refuses (false) while the previous endpoint object is still open: its Service, its
// bind_sent and its buffers are never forgotten while the server holds them.
static bool ep_reset(Ep* e, const char* name) {
    if (serviceIsActive(&e->s) || e->event != INVALID_HANDLE || e->tm_made) return false;
    *e = (Ep){.name = name, .event = INVALID_HANDLE};
    return true;
}

static __attribute__((noipa)) void teardown_iface(void) {
    ulog(1, "teardown: interface");
    ep_cancel_outstanding(&g_out);
    ep_cancel_outstanding(&g_in);
    close_obj("OUT endpoint", &g_out.s);
    close_obj("IN endpoint", &g_in.s);
    ep_release(&g_out);
    ep_release(&g_in);
    close_obj("interface", &g_iface);
    close_event("OUT completion", &g_out.event);
    close_event("IN completion", &g_in.event);
}

static __attribute__((noipa)) void teardown_session(void) {
    ulog(1, "teardown: session");
    close_event("client state", &g_state_event);
    close_obj("client", &g_client);
    close_root(&g_root);
}

int uhs2_open(void) {
    if (g_open) return 0;
    if (g_open_tried) return -1;    // one usb:hs session per process: a failed open is not retried
    g_open_tried = true;
    // `in` is loaded from .rodata.kOpenIn (the barrier stops the compiler folding it), guarded, and sent unchanged.
    const OpenClientIn* src = &kOpenIn;
    __asm__ volatile("" : "+r"(src));
    OpenClientIn in;
    memcpy(&in, src, sizeof(in));
    ulog_hex(1, "  in raw", &in, sizeof(in));
    if (!raw_is_msc(&in)) {
        ulog(0, "REFUSED: root cmd 0 raw bytes are not the mass-storage filter; no usb:hs IPC");
        return -1;
    }
    ulog(1, "pre smGetService " USBHS_NAME);
    Result rc = smGetService(&g_root, USBHS_NAME);
    ulog(R_FAILED(rc) ? 0 : 1, "smGetService rc=0x%08x handle=0x%08x own=%u object=%u ptrbuf=%u", rc, g_root.session,
         g_root.own_handle, g_root.object_id, g_root.pointer_buffer_size);
    if (R_FAILED(rc)) return -1;

    ulog(1, "pre control cmd0 ConvertCurrentObjectToDomain handle=0x%08x own=%u", g_root.session, g_root.own_handle);
    rc = root_convert_domain(&g_root);
    log_reply(1, "control cmd0 ConvertCurrentObjectToDomain", rc);
    if (R_FAILED(rc) || g_root.object_id == 0) {
        ulog(0, "stop: no domain (rc=0x%08x object_id=%u); root cmd 0 not sent", rc, g_root.object_id);
        close_root(&g_root);
        return -1;
    }

    ulog(1, "pre root cmd0 OpenClientSession domain object=%u in=0x%x copy=0x%08x out_objects=1", g_root.object_id,
         (unsigned)sizeof(in), CUR_PROCESS_HANDLE);
    rc = root_cmd0_domain(&g_root, &in, &g_client);
    log_reply(1, "root cmd0 OpenClientSession", rc);
    if (rc == REFUSED_NOT_DOMAIN || !out_object_ok("root cmd0", rc, &g_client)) {
        close_root(&g_root);
        return -1;
    }
    rc = client_cmd0_event(&g_client);
    log_reply(1, "client cmd0 GetStateChangeEvent", rc);
    if (R_SUCCEEDED(rc)) g_state_event = one_handle();
    if (g_state_event != INVALID_HANDLE) {
        rc = svcWaitSynchronizationSingle(g_state_event, STATE_WAIT_NS);
        ulog(1, "wait state event rc=0x%08x (0 = signalled, 0xea01 = timeout)", rc);
        // Cleared only when it fired. With no drive at startup the wait times out and the event stays armed: a later
        // plug signals it (0x53b80) and usbfs2_poll attaches, exactly as after a detach. (4.0.2 reset it anyway, got
        // 0xfa01 "not signalled" and CLOSED the event: a drive plugged in after an empty start was never picked up.)
        if (R_SUCCEEDED(rc)) {
            attach_event(&g_retry);
            rc = svcResetSignal(g_state_event);
            ulog(1, "clear state event rc=0x%08x", rc);
        }
    }
    g_open = true;
    return 0;
}

int uhs2_attach(void) {
    if (!g_open || g_attached) return -1;
    if (g_latched || g_attaches >= USBHS2_MAX_ATTACHES) {
        ulog(0, "attach refused: %s", g_latched ? "an earlier bind/memory/tmem step failed" : "attach limit reached");
        return -1;
    }
    const u64 now = armGetSystemTick();
    if (!attach_gap(&g_retry, now, attach_gap_ticks())) {
        ulog(0, "attach refused: less than %u ms since the previous attempt", USBHS2_ATTACH_GAP_MS);
        return -1;
    }
    const unsigned remaining = attach_begin(&g_retry, now);
    if (!ep_reset(&g_out, "OUT") || !ep_reset(&g_in, "IN") || serviceIsActive(&g_iface)) {
        ulog(0, "stop: an earlier interface / endpoint object is still open; no further attach");
        g_latched = true;
        return -1;
    }
    g_iface = (Service){0};
    g_pick = (BotIface){0};

    Result rc = client_cmd1_profile(&g_client);
    log_reply(1, "client cmd1 QueryAvailableInterface", rc);
    if (R_FAILED(rc)) {
        attach_query_result(&g_retry, rc, remaining);
        ulog(0, "no drive: client cmd1 rc=0x%08x (0x725 = no interface assigned)", rc);
        return -1;
    }
    ulog_hex(2, "  profile", g_profile, sizeof(g_profile));
    decode_profile(g_profile);
    g_vid = rd16(g_profile + 0x18 + 8);
    g_pid = rd16(g_profile + 0x18 + 10);
    const int pv = bot_profile_pick(g_profile, sizeof(g_profile), &g_pick);
    ulog(0, "profile pick: %s id=%u in=0x%02x/%u out=0x%02x/%u", bot_reason(pv), g_pick.id, g_pick.in_addr,
         g_pick.in_mps, g_pick.out_addr, g_pick.out_mps);
    if (pv != BOT_OK) {
        ulog(0, "stop: profile is not one BOT interface with one bulk IN and one bulk OUT; no acquire");
        return -1;
    }
    if (g_pick.id == g_last_detached_id) {
        ulog(0, "attach refused: interface %u is the one released last (the same attachment of a drive that stopped or "
             "failed); unplug it and plug it in again", g_pick.id);
        return -1;
    }

    rc = client_cmd2_acquire(&g_client, &g_iface);
    log_reply(1, "client cmd2 AcquireInterface", rc);
    if (!out_object_ok("client cmd2", rc, &g_iface)) return -1;
    g_attaches++;    // counted once an interface is held (reviewer c1): a "no drive" attempt does not use one up

    g_attached = true;    // from here on, every failure path goes through teardown_iface
    g_out.attach_no = g_in.attach_no = g_attaches;
    g_out.addr = g_pick.out_addr;
    g_in.addr = g_pick.in_addr;
    rc = iface_cmd4_open_ep(&g_iface, &g_out);
    log_reply(1, "iface cmd4 OpenEndpoint OUT", rc);
    bool ok = out_object_ok("iface cmd4 OUT", rc, &g_out.s);
    if (ok) {
        rc = iface_cmd4_open_ep(&g_iface, &g_in);
        log_reply(1, "iface cmd4 OpenEndpoint IN", rc);
        ok = out_object_ok("iface cmd4 IN", rc, &g_in.s);
    }
    if (ok) ok = ep_setup(&g_out, OUT_DATA_SIZE) && ep_setup(&g_in, IN_DATA_SIZE);
    if (!ok) {
        ulog(0, "stop: endpoint setup incomplete; interface released");
        uhs2_detach();
        return -1;
    }
    ulog(0, "usb: interface %u (vid %04x pid %04x) attached (%u of at most %u this run), OUT 0x%02x IN 0x%02x, OUT buffer "
         "0x%x IN buffer 0x%x", g_pick.id, g_vid, g_pid, g_attaches, USBHS2_MAX_ATTACHES, g_pick.out_addr, g_pick.in_addr,
         OUT_DATA_SIZE, IN_DATA_SIZE);
    return 0;
}

void uhs2_detach(void) {
    if (!g_attached) return;
    teardown_iface();
    g_last_detached_id = g_pick.id;
    g_attached = false;
}

void uhs2_close(void) {
    uhs2_detach();
    if (!g_open) return;
    teardown_session();
    g_open = false;
}
