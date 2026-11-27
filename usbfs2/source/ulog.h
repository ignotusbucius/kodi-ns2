// usbfs2 log: one hook for every layer (pure). Level 0 = milestones and errors, 1 = every setup/teardown IPC
// ("pre <step>" lines), 2 = every bulk transfer. Lines at a level above the verbosity are not formatted.
#pragma once
#include <stdarg.h>

#ifndef USBFS2_LOGFN_DEFINED
#define USBFS2_LOGFN_DEFINED
typedef void (*Usbfs2LogFn)(int level, const char* line);
#endif

void ulog_set(Usbfs2LogFn fn, int verbosity);
void ulog_set_verbosity(int verbosity);
int ulog_verbosity(void);
void ulog(int level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void ulog_hex(int level, const char* tag, const void* p, unsigned n);
