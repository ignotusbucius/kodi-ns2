// usbfs2: FatFs OS hooks. FF_USE_LFN 3 keeps the long-name buffer on the heap; FF_FS_REENTRANT 0 (usbfs2.c holds one
// lock around every FatFs call), so no ff_mutex_* hooks are needed.
#include <stdlib.h>
#include "ff.h"

void* ff_memalloc(UINT msize) { return malloc((size_t)msize); }
void ff_memfree(void* mblock) { free(mblock); }
