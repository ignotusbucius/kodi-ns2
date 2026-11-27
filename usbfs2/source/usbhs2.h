// usbfs2 usbhs2: the Switch 2 usb:hs client (domain form), READ-ONLY mass storage. The protocol is usbbotprobe's
// (finding 1538, flown twice) and usbhs2probe v4's (finding 1536), with an IN buffer of 0x2000 (one-page data
// phase + the CSW page) bound once. Callers must serialise (usbfs2.c holds one mutex around every call).
#pragma once
#include <stdint.h>
#include "blk.h"

// Once per process: sm GetService("usb:hs"), ConvertCurrentObjectToDomain, root cmd 0 OpenClientSession (domain,
// mass-storage filter 8/6/0x50), client cmd 0 GetStateChangeEvent. The session stays open until uhs2_close().
int uhs2_open(void);
// QueryAvailableInterface, AcquireInterface, OpenEndpoint OUT + IN, per endpoint: heap pages (svcQueryMemory-checked),
// report tmem (Perm_Rw), ONE BindBuffer, completion event. At most USBHS2_MAX_ATTACHES per process; never again after a
// BindBuffer or memory-guard failure.
int uhs2_attach(void);
// The BlkTransport over the attached interface (send_cbw re-checks the exact OUT bytes with bot_check_cbw).
void uhs2_transport(BlkTransport* t);
// Cancel an outstanding transfer, close the endpoints, THEN tmemClose + free, close the interface and the events.
void uhs2_detach(void);
// uhs2_detach, then close the state event, the client and the root session.
void uhs2_close(void);
int uhs2_attached(void);
// 1 when the client state-change event fired since the last call (and clears it).
int uhs2_state_changed(void);
// Device identity of the attached interface (from the profile).
void uhs2_identity(uint32_t* iface_id, uint16_t* vid, uint16_t* pid);

int uhs2_attach_due(void);
#define USBHS2_MAX_ATTACHES 32
#define USBHS2_ATTACH_GAP_MS 1000
