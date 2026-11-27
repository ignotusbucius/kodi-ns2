// Read-only discovery after one insert indication. Only "not assigned yet" keeps credits.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define ATTACH_PROBES 5u

typedef struct { unsigned left; bool tried; uint64_t last; } AttachRetry;
static inline void attach_event(AttachRetry *s) { s->left = ATTACH_PROBES; }
static inline bool attach_gap(const AttachRetry *s, uint64_t now, uint64_t gap) {
    return !s->tried || now - s->last >= gap;
}
static inline bool attach_due(const AttachRetry *s, uint64_t now, uint64_t gap, bool eligible) {
    return eligible && s->left && attach_gap(s, now, gap);
}
static inline unsigned attach_begin(AttachRetry *s, uint64_t now) {
    unsigned remaining = s->left ? s->left - 1 : 0;
    s->left = 0;  // fail closed on all subsequent exits unless discovery explicitly rearms
    s->tried = true;
    s->last = now;
    return remaining;
}
static inline void attach_query_result(AttachRetry *s, uint32_t result, unsigned remaining) {
    if (result == 0x725) s->left = remaining;
}
