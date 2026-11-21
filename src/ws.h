#ifndef EUROS_WS_H
#define EUROS_WS_H

#include <stdbool.h>
#include <stddef.h>

typedef struct ws_conn ws_conn;

// Opens a wss:// connection. NULL on failure (reason in err).
ws_conn *ws_connect(const char *url, char *err, size_t err_size);

// Sends one text frame.
bool ws_send_text(ws_conn *ws, const char *text);

// Non-blocking. 1: a complete text message is in *out (caller frees). 0: nothing yet. -1: closed or failed.
int ws_next_message(ws_conn *ws, char **out);

// Blocks until the socket is readable or timeout_ms passes.
void ws_wait(ws_conn *ws, int timeout_ms);

void ws_close(ws_conn *ws);

#endif
