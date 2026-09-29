/* server/reflux_runtime.c -- see reflux_runtime.h for the real design rationale. Near-identical
 * port of SHANKPIT/packages/reflux/reflux_runtime.c's own proven ring-buffer shape -- same real
 * "total_dispatched vs. capacity" wraparound logic, ported into EDGE.GAME's own tree with its own
 * global log, matching that file's own established "own copy, own log" convention. */
#include "reflux_runtime.h"

#include <string.h>

void reflux_log_reset(RefluxLog *log) {
    memset(log, 0, sizeof(*log));
}

void reflux_log_dispatch(RefluxLog *log, int action_type, int a, int b, int c) {
    int slot = log->total_dispatched % REFLUX_LOG_CAPACITY;
    log->actions[slot].action_type = action_type;
    log->actions[slot].a = a;
    log->actions[slot].b = b;
    log->actions[slot].c = c;
    log->total_dispatched++;
}

int reflux_log_length(const RefluxLog *log) {
    return log->total_dispatched < REFLUX_LOG_CAPACITY
        ? log->total_dispatched
        : REFLUX_LOG_CAPACITY;
}

const RefluxAction *reflux_log_at(const RefluxLog *log, int index) {
    int size = reflux_log_length(log);
    if (index < 0 || index >= size) return NULL;

    if (log->total_dispatched <= REFLUX_LOG_CAPACITY) {
        return &log->actions[index];
    }

    int oldest_slot = log->total_dispatched % REFLUX_LOG_CAPACITY;
    int slot = (oldest_slot + index) % REFLUX_LOG_CAPACITY;
    return &log->actions[slot];
}

/* ---- the one, real, per-process log + host wrappers ---------------------------------------- */

static RefluxLog g_reflux_log;

void reflux_host_reset(void) {
    reflux_log_reset(&g_reflux_log);
}

void reflux_host_dispatch(int action_type, int a, int b, int c) {
    reflux_log_dispatch(&g_reflux_log, action_type, a, b, c);
}

int reflux_host_log_size(void) {
    return reflux_log_length(&g_reflux_log);
}

int reflux_host_action_type_at(int index) {
    const RefluxAction *e = reflux_log_at(&g_reflux_log, index);
    return e ? e->action_type : -1;
}

int reflux_host_action_a_at(int index) {
    const RefluxAction *e = reflux_log_at(&g_reflux_log, index);
    return e ? e->a : 0;
}

int reflux_host_action_b_at(int index) {
    const RefluxAction *e = reflux_log_at(&g_reflux_log, index);
    return e ? e->b : 0;
}

int reflux_host_action_c_at(int index) {
    const RefluxAction *e = reflux_log_at(&g_reflux_log, index);
    return e ? e->c : 0;
}
