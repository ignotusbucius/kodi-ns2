// usbfs2 internal: the newlib devoptab devices (devoptab.c) and the one lock (usbfs2.c).
#pragma once
void usbfs2_lock(void);
void usbfs2_unlock(void);
// Register "umsN:" for vfs volume N (0 = ok). Remove it again (no new opens; open files then fail with EBADF/EIO).
int usbfs2_dev_add(int vol);
void usbfs2_dev_remove(int vol);
