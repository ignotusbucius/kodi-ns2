/*---------------------------------------------------------------------------/
/  usbfs2: FatFs R0.16 configuration -- READ-ONLY (third_party/fatfs/ff.h includes this file through the include path;
/  third_party/fatfs/ holds no ffconf.h of its own, so this is the only configuration the NRO / libusbfs2 build sees).
/  Differences from the upstream file (third_party/fatfs/ffconf.h.upstream) are marked [usbfs2].
/  `make check` pins FF_FS_READONLY 1 / FF_USE_MKFS 0 / FF_USE_EXPAND 0 / FF_USE_CHMOD 0 / FF_USE_LABEL 1 /
/  FF_USE_STRFUNC 0 / FF_USE_FORWARD 0 here, and that ff.o defines no write function and references no disk_write.
/---------------------------------------------------------------------------*/

#define FFCONF_DEF	80386	/* Revision ID */

/* [usbfs2] READ-ONLY: removes f_write, f_sync, f_unlink, f_mkdir, f_chmod, f_rename, f_truncate, f_getfree and every
   other writing function, and every disk_write call inside ff.c. Never change this. */
#define FF_FS_READONLY	1
#define FF_FS_MINIMIZE	0
#define FF_USE_FIND		0
#define FF_USE_MKFS		0
#define FF_USE_FASTSEEK	0
#define FF_USE_EXPAND	0
#define FF_USE_CHMOD	0
/* [usbfs2] v1c: f_getlabel only (the volume label shown by Kodi). ff.c compiles f_setlabel only under
   `#if !FF_FS_READONLY` (inside `#if FF_USE_LABEL`), so with FF_FS_READONLY 1 it does not exist; `make check` pins that
   ff.c block and that no object defines or references f_setlabel. */
#define FF_USE_LABEL	1
#define FF_USE_FORWARD	0
#define FF_USE_STRFUNC	0
#define FF_PRINT_LLI	0
#define FF_PRINT_FLOAT	0
#define FF_STRF_ENCODE	0

/* [usbfs2] US code page for short names; long file names on the heap (ff_memalloc), UTF-8 API strings. */
#define FF_CODE_PAGE	437
#define FF_USE_LFN		3
#define FF_MAX_LFN		255
#define FF_LFN_UNICODE	2
#define FF_LFN_BUF		765
#define FF_SFN_BUF		12
#define FF_FS_RPATH		0
#define FF_PATH_DEPTH	10

/* [usbfs2] one FatFs volume per mounted partition (ums0: .. ums7:), volume N = physical drive N in diskio.c, which adds
   the partition's start LBA itself (the partition table is parsed by part.c, not by FatFs). */
#define FF_VOLUMES		8
#define FF_STR_VOLUME_ID	0
#define FF_VOLUME_STRS		"RAM","NAND","CF","SD","SD2","USB","USB2","USB3"
#define FF_MULTI_PARTITION	0

/* [usbfs2] 512-byte and 4 KiB-sector drives (GET_SECTOR_SIZE from READ CAPACITY(10)). */
#define FF_MIN_SS		512
#define FF_MAX_SS		4096
#define FF_LBA64		0
#define FF_MIN_GPT		0x10000000
#define FF_USE_TRIM		0
#define FF_FS_TINY		0

/* [usbfs2] exFAT on. */
#define FF_FS_EXFAT		1

/* [usbfs2] no RTC (read-only: no timestamps are ever written). */
#define FF_FS_NORTC		1
#define FF_NORTC_MON	1
#define FF_NORTC_MDAY	1
#define FF_NORTC_YEAR	2025
#define FF_FS_CRTIME	0
#define FF_FS_NOFSINFO	0
#define FF_FS_LOCK		0

/* [usbfs2] not reentrant: usbfs2.c serialises every FatFs / block / USB call under one mutex. */
#define FF_FS_REENTRANT	0
#define FF_FS_TIMEOUT	1000
