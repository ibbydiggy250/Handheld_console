#ifndef WIFI_H
#define WIFI_H

#include <stdbool.h>

/* Station-mode WiFi. Non-blocking: wifi_start() returns as soon as the
   driver is running, and the connection completes on the event loop.
   The game loop never waits on the radio. */
void wifi_start(void);

/* True once an IP has been assigned. Cleared on disconnect. */
bool wifi_is_connected(void);

#endif