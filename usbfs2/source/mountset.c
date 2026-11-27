// usbfs2 mountset (see mountset.h).
#include <string.h>
#include "mountset.h"
#include "ulog.h"

static int rd_blk(void* ctx, uint64_t lba, uint32_t count, void* buf) {
    return blk_read((Blk*)ctx, lba, count, buf) == BLK_OK ? 0 : -1;
}

int ms_mount(MountSet* m, Blk* b) {
    memset(m, 0, sizeof(*m));
    if (!b || b->dead) return -1;
    const int n = part_scan(rd_blk, b, b->block, b->blocks, m->parts, PART_MAX, &m->scheme);
    ulog(0, "partitions: scheme %s, %d usable entr%s", part_scheme_name(m->scheme), n < 0 ? 0 : n, n == 1 ? "y" : "ies");
    if (n < 0) return -1;
    m->nparts = n;
    static uint8_t probe[PART_PROBE_BYTES];
    for (int i = 0; i < n && !b->dead; i++) {
        PartEntry* p = &m->parts[i];
        char guid[37] = "";
        if (p->scheme == PART_SCHEME_GPT) part_guid_str(p->guid, guid);
        uint32_t nb = PART_PROBE_BYTES / b->block;
        if (nb == 0) nb = 1;
        if (nb > p->count) nb = (uint32_t)p->count;
        memset(probe, 0, sizeof(probe));
        m->part_fs[i] = blk_read(b, p->start, nb, probe) == BLK_OK ? part_probe_fs(probe, (size_t)nb * b->block)
                                                                   : PART_FS_UNKNOWN;
        ulog(0, "  part %d: %s #%d start=%llu count=%llu (%llu MiB) mbr_type=0x%02x guid=%s name='%s' fs=%s", i,
             part_scheme_name(p->scheme), p->index, (unsigned long long)p->start, (unsigned long long)p->count,
             (unsigned long long)(p->count * b->block >> 20), p->mbr_type, guid[0] ? guid : "-", p->name,
             part_fs_name(m->part_fs[i]));
        if (m->part_fs[i] != PART_FS_FAT && m->part_fs[i] != PART_FS_EXFAT) {
            if (m->part_fs[i] != PART_FS_UNKNOWN) ulog(0, "  part %d: %s is not supported (read-only FAT/exFAT only)", i, part_fs_name(m->part_fs[i]));
            continue;
        }
        if (m->nvol >= VFS_MAX_VOLUMES) {
            ulog(0, "  part %d: all %d volume slots in use; not mounted", i, VFS_MAX_VOLUMES);
            continue;
        }
        const int vol = m->nvol;
        int fstype = VFS_FS_NONE;
        const int rc = vfs_mount(vol, b, p->start, p->count, &fstype);
        if (rc != 0) {
            ulog(0, "  part %d: FatFs found no usable FAT/exFAT filesystem (rc %d); not mounted", i, rc);
            continue;
        }
        m->vol[m->nvol++] = (MsVolume){.vol = vol, .fstype = fstype, .part = i};
        VfsInfo vi;
        vfs_info(vol, &vi);
        ulog(0, "  part %d -> volume %d: %s, cluster %u bytes, %llu clusters, label '%s'", i, vol, vfs_fs_name(fstype),
             vi.cluster_bytes, (unsigned long long)vi.clusters, vi.label);
    }
    return m->nvol;
}

void ms_unmount(MountSet* m) {
    for (int i = 0; i < m->nvol; i++) vfs_unmount(m->vol[i].vol);
    m->nvol = 0;
}
