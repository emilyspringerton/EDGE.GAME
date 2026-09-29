/* server/reflux_runtime.h -- EDGE.GAME's own copy of REFLUX's host runtime (same real API/ABI as
 * SHANKPIT/packages/reflux/reflux_runtime.h and ECOWAR's own copy before it -- "same API, same
 * ABI, own copy, own log, own action-type constants", per SHANKPIT's own header comment on this
 * exact convention). Founder real-time, 2026-09-29: "make sure we are using REFLUX for the pub
 * sub." stdlib/reflux/reflux.prn (PARENA, unmodified, shared with every other repo) is the real
 * decision/API layer; this file is EDGE.GAME's own real, separate backing log + constants, exactly
 * like SHANKPIT's `packages/reflux/reflux_runtime.c`/`.h` are SHANKPIT's own.
 */
#ifndef EDGE_GAME_REFLUX_RUNTIME_H
#define EDGE_GAME_REFLUX_RUNTIME_H

/* EDGE.GAME's own action-type constants -- own numbering (SHANKPIT's/IDUNA.GAME's own numbers
 * don't need to match; only the ring-buffer API/ABI shape is shared).
 *
 * Dispatched once by a Raspberry Pi's boot-announce script (pi/boot_announce.sh) the moment it can
 * reach this relay after boot. Payload: a = pi_id (a small, founder-assigned integer per physical
 * Pi), b = boot unix-epoch-seconds (server-side clock, not the reporting device's own -- truncated
 * to a signed I32, valid until 2038), c unused (0). */
#define REFLUX_ACTION_PI_BOOTED 1

/* Real, named, NOT YET DISPATCHED anywhere (reserved now so numbering stays stable once these
 * land -- same convention SHANKPIT's own reflux_runtime.h already established for its own
 * PROXIMITY_ENTER/EXIT reservation). Payload once built: COMPILE_RESULT a=1/0 success, b/c
 * reserved; UPLOAD_RESULT a=1/0 success, b=board profile id, c reserved; PIN_EVENT a=pin number,
 * b=value, c=0 (read) or 1 (write). */
#define REFLUX_ACTION_COMPILE_RESULT 2
#define REFLUX_ACTION_UPLOAD_RESULT 3
#define REFLUX_ACTION_PIN_EVENT 4

#define REFLUX_LOG_CAPACITY 256

typedef struct {
    int action_type;
    int a, b, c;
} RefluxAction;

typedef struct {
    RefluxAction actions[REFLUX_LOG_CAPACITY];
    int total_dispatched;
} RefluxLog;

void reflux_log_reset(RefluxLog *log);
void reflux_log_dispatch(RefluxLog *log, int action_type, int a, int b, int c);
int reflux_log_length(const RefluxLog *log);
const RefluxAction *reflux_log_at(const RefluxLog *log, int index);

/* ---- the one, real, per-process REFLUX log + its PARENA-callable host wrappers ----------------
 * reflux_host_* are the real functions PARENA/stdlib/reflux/reflux.prn's own #target bodies call
 * into by name (both the :c and, as of 2026-09-29, the :llvm key) -- same call-by-name convention
 * every other REFLUX port already uses. */
void reflux_host_reset(void);
void reflux_host_dispatch(int action_type, int a, int b, int c);
int reflux_host_log_size(void);
int reflux_host_action_type_at(int index);
int reflux_host_action_a_at(int index);
int reflux_host_action_b_at(int index);
int reflux_host_action_c_at(int index);

#endif /* EDGE_GAME_REFLUX_RUNTIME_H */
