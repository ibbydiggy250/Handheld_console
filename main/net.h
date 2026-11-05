#ifndef NET_H
#define NET_H

#include <stdbool.h>

/* HTTP client on its own task. The game loop never blocks on the network:
   net_request() only raises a flag, the task does the blocking transfer,
   and the result is read back through net_last_status(). */

void net_start(void);

/* Ask the net task to run one request. Returns false if one is already in
   flight, so a held chord can't queue a hundred of them. */
bool net_request(void);

/* -1 = never run, -2 = in flight, -3 = transport error (no HTTP response),
   otherwise the HTTP status code. */
int net_last_status(void);

#endif