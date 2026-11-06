#ifndef NET_H
#define NET_H

#include <stdbool.h>
#include <stdint.h>

/* HTTP client on its own task. The game loop never blocks on the network:
   it only raises flags, the task does the blocking transfers, and results
   are read back through snapshot getters. */

void net_start(void);

/* ---------------------------------------------------------------- test GET
   Ask the net task to run one request. Returns false if one is already in
   flight (or pairing is running), so a held chord can't queue a hundred. */
bool net_request(void);

/* -1 = never run, -2 = in flight, -3 = transport error (no HTTP response),
   otherwise the HTTP status code. */
int net_last_status(void);

/* ---------------------------------------------------------------- pairing
   3.4 device-code flow: ask Supabase for a code, show it, poll until the
   user approves it on the web app, then hold the session in RAM. */

typedef enum {
    PAIR_IDLE,         // not pairing
    PAIR_NO_WIFI,      // waiting for an IP before asking for a code
    PAIR_REQUESTING,   // device-issue in flight
    PAIR_SHOW_CODE,    // code on screen, polling device-poll
    PAIR_LINKED,       // session collected and verified against /auth/v1/user
    PAIR_ERROR,        // last request failed; retrying shortly
} pair_state_t;

typedef struct {
    pair_state_t state;
    char         code[7];      // 6-char user code + NUL, valid in PAIR_SHOW_CODE
    int64_t      expires_us;   // esp_timer_get_time() at which the code dies
} pair_view_t;

/* Start pairing (no-op if already running). Cancels nothing else. */
void net_pair_start(void);

/* Stop pairing and return to PAIR_IDLE. Safe to call any time; a linked
   session is kept. */
void net_pair_cancel(void);

/* Copy the current pairing state. Cheap (a ~24 B copy under a spinlock) --
   fine to call every frame. */
void net_pair_view(pair_view_t *out);

#endif
