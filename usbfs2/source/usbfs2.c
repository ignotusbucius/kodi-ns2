// usbfs2: orchestration (see include/usbfs2.h): usb:hs session -> interface -> blk -> partitions -> FatFs volumes ->
// devoptab devices, all under one mutex.
#include <string.h>
#include <switch.h>
#include "usbfs2.h"
#include "usbhs2.h"
#include "blk.h"
#include "mountset.h"
#include "vfs.h"
#include "ulog.h"
#include "usbfs2_dev.h"

_Static_assert((int)USBFS2_FS_FAT12 == (int)VFS_FS_FAT12 && (int)USBFS2_FS_EXFAT == (int)VFS_FS_EXFAT, "fs type numbers");
_Static_assert(sizeof(((Usbfs2Volume*)0)->label) == VFS_LABEL_MAX, "label buffer size");

static Mutex g_lock;
static int g_inited;
static int g_mounted;
static unsigned g_slots;  // dispatch slots retained for open descriptors until exit
static Blk g_blk;
static MountSet g_ms;
static uint8_t g_cache[BLK_LINES * BLK_LINE_BYTES] __attribute__((aligned(64)));

void usbfs2_lock(void) { mutexLock(&g_lock); }
void usbfs2_unlock(void) { mutexUnlock(&g_lock); }

int usbfs2_init(Usbfs2LogFn log, int verbosity) {
    usbfs2_lock();
    if (log) ulog_set(log, verbosity);
    int rc = 0;
    if (!g_inited) {
        rc = uhs2_open();
        if (rc == 0) g_inited = 1;
        ulog(0, "usbfs2: usb:hs session %s", rc == 0 ? "open" : "NOT open");
    }
    usbfs2_unlock();
    return rc;
}

static void unmount_locked(const char* why) {
    if (!g_mounted) return;
    ulog(0, "usbfs2: unmount (%s)", why);
    // Keep device dispatch slots stable for open descriptors across replacement.
    ms_unmount(&g_ms);
    blk_kill(&g_blk, why);
    uhs2_detach();
    g_mounted = 0;
}

static int mount_locked(void) {
    if (!g_inited) return -1;
    if (g_mounted) return g_ms.nvol;
    if (uhs2_attach() != 0) return -1;
    BlkTransport t;
    uhs2_transport(&t);
    if (blk_init(&g_blk, &t, g_cache) != BLK_OK) {
        ulog(0, "usbfs2: the drive did not become ready; interface released");
        uhs2_detach();
        return -1;
    }
    const int n = ms_mount(&g_ms, &g_blk);
    if (n <= 0 || g_blk.dead) {
        const int result = (n == 0 && !g_blk.dead) ? 0 : -1;
        ms_unmount(&g_ms);
        blk_kill(&g_blk, result == 0 ? "no supported volume" : "partition scan failed");
        uhs2_detach();
        return result;
    }
    g_mounted = 1;
    unsigned added = 0;
    for (int i = 0; i < g_ms.nvol; i++) {
        const int rc = usbfs2_dev_add(g_ms.vol[i].vol);
        if (rc != 0) {
            for (int vol = 0; vol < VFS_MAX_VOLUMES; ++vol)
                if (added & (1u << vol)) usbfs2_dev_remove(vol);
            unmount_locked("device registration failed");
            return -1;
        }
        added |= (1u << g_ms.vol[i].vol) & ~g_slots;
        ulog(0, "usbfs2: ums%d: mounted (%s)", g_ms.vol[i].vol,
             vfs_fs_name(g_ms.vol[i].fstype));
    }
    g_slots |= added;
    return g_ms.nvol;
}

void usbfs2_set_verbosity(int verbosity) {
    usbfs2_lock();
    ulog_set_verbosity(verbosity);
    usbfs2_unlock();
}

int usbfs2_set_path_key(const char* key) {
    usbfs2_lock();
    int rc = vfs_set_key(key);
    usbfs2_unlock();
    return rc;
}

int usbfs2_mount(void) {
    usbfs2_lock();
    const int rc = mount_locked();
    usbfs2_unlock();
    return rc;
}

int usbfs2_volume_count(void) {
    usbfs2_lock();
    const int n = g_mounted ? g_ms.nvol : 0;
    usbfs2_unlock();
    return n;
}

int usbfs2_volume(int i, Usbfs2Volume* o) {
    usbfs2_lock();
    int rc = -1;
    if (g_mounted && i >= 0 && i < g_ms.nvol) {
        const MsVolume* v = &g_ms.vol[i];
        const PartEntry* p = &g_ms.parts[v->part];
        VfsInfo vi = {0};
        vfs_info(v->vol, &vi);
        memset(o, 0, sizeof(*o));
        o->device[0] = 'u', o->device[1] = 'm', o->device[2] = 's', o->device[3] = (char)('0' + v->vol);
        memcpy(o->root, o->device, 4);
        o->root[4] = ':', o->root[5] = '/';
        o->fstype = v->fstype;
        o->fs_name = vfs_fs_name(v->fstype);
        o->scheme = part_scheme_name(p->scheme);
        o->part_index = p->index;
        o->start_block = p->start;
        o->block_count = p->count;
        o->block_size = g_blk.block;
        o->cluster_bytes = vi.cluster_bytes;
        o->clusters = vi.clusters;
        memcpy(o->label, vi.label, sizeof(o->label));
        rc = 0;
    }
    usbfs2_unlock();
    return rc;
}

int usbfs2_drive(Usbfs2Drive* o) {
    usbfs2_lock();
    memset(o, 0, sizeof(*o));
    o->attached = uhs2_attached();
    o->dead = g_blk.dead;
    uhs2_identity(&o->iface_id, &o->vid, &o->pid);
    memcpy(o->vendor, g_blk.vendor, sizeof(o->vendor));
    memcpy(o->product, g_blk.product, sizeof(o->product));
    memcpy(o->rev, g_blk.rev, sizeof(o->rev));
    o->block_size = g_blk.block;
    o->blocks = g_blk.blocks;
    o->capped = g_blk.capped;
    o->commands = g_blk.cmds;
    o->bytes_read = g_blk.bytes_read;
    o->cache_hits = g_blk.cache_hits;
    o->cache_misses = g_blk.cache_misses;
    usbfs2_unlock();
    return 0;
}

int usbfs2_poll(void) {
    usbfs2_lock();
    int changed = 0;
    if (g_mounted) {
        // The client event alone does not report idle unplug. Use the existing
        // read-only readiness check; fail closed on transfer or medium errors.
        if (!g_blk.dead) (void)blk_check(&g_blk);
        if (g_blk.dead) {
            (void)unmount_locked("drive gone or failed");
            changed = 1;
        }
    } else if (g_inited && uhs2_attach_due()) {
        ulog(0, "usbfs2: discovery due; checking for a drive");
        changed = mount_locked() > 0;
    }
    usbfs2_unlock();
    return changed;
}

void usbfs2_unmount(void) {
    usbfs2_lock();
    unmount_locked("requested");
    usbfs2_unlock();
}

void usbfs2_exit(void) {
    usbfs2_lock();
    unmount_locked("exit");
    for (int i = 0; i < VFS_MAX_VOLUMES; ++i) usbfs2_dev_remove(i);
    g_slots = 0;
    if (g_inited) uhs2_close();
    g_inited = 0;
    ulog(0, "usbfs2: usb:hs session closed");
    usbfs2_unlock();
}
