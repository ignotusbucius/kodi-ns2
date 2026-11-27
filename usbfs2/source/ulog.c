// usbfs2 log hook (see ulog.h). No I/O of its own: the hook decides where a line goes.
#include <stdio.h>
#include "ulog.h"

static Usbfs2LogFn g_fn;
static int g_verbosity;

void ulog_set(Usbfs2LogFn fn, int verbosity) {
    g_fn = fn;
    g_verbosity = verbosity;
}

void ulog_set_verbosity(int verbosity) { g_verbosity = verbosity; }

int ulog_verbosity(void) { return g_fn ? g_verbosity : -1; }

void ulog(int level, const char* fmt, ...) {
    if (!g_fn || level > g_verbosity) return;
    char line[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    g_fn(level, line);
}

void ulog_hex(int level, const char* tag, const void* p, unsigned n) {
    if (!g_fn || level > g_verbosity) return;
    const unsigned char* b = (const unsigned char*)p;
    for (unsigned off = 0; off < n; off += 16) {
        char hex[16 * 3 + 1] = "";
        for (unsigned i = off; i < n && i < off + 16; i++) snprintf(hex + 3 * (i - off), 4, "%02x ", b[i]);
        ulog(level, "%s +%03x: %s", tag, off, hex);
    }
}
