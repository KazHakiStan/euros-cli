#ifndef EUROS_REALTIME_H
#define EUROS_REALTIME_H

#include <cJSON.h>

#include <stdbool.h>
#include <stddef.h>

typedef struct rt_channel rt_channel;

// Incoming channel traffic:
//   "broadcast"      payload = { "event": "...", "payload": {...} }
//   "presence_state" payload = { "<key>": { "metas": [...] }, ... }
//   "presence_diff"  payload = { "joins": {...}, "leaves": {...} }
//   "postgres_changes" payload = { "ids": [...], "data": { "type", "table", "record": {...}, ... } }
//   "reply"          payload = { "ref": <number>, "status": "ok" | "error" } for our pushes
typedef void (*rt_handler)(void *ctx, const char *event, const cJSON *payload);

// Connects and joins a private channel (e.g. topic "room:main"). Blocks until the join is answered.
// presence_key: the user id, or NULL for no presence. changes: postgres_changes bindings
// ([{ event, schema, table, filter }]) or NULL. NULL on failure (reason in err); *connected tells
// whether the WebSocket itself was up (may be NULL).
rt_channel *rt_join(const char *topic, const char *presence_key, const cJSON *changes, const char *access_token,
                    rt_handler handler, void *ctx, bool *connected, char *err, size_t err_size);

// Pushes { type: "broadcast", event, payload }. Returns the push ref (> 0), or 0 if sending failed.
long rt_broadcast(rt_channel *ch, const char *event, const cJSON *payload);

// Tracks this client's presence meta.
bool rt_track(rt_channel *ch, const cJSON *meta);

// Hands a refreshed access token to the server.
bool rt_set_token(rt_channel *ch, const char *access_token);

// Processes incoming messages and keeps the heartbeat, waiting up to timeout_ms.
// False once the connection is gone.
bool rt_pump(rt_channel *ch, int timeout_ms);

void rt_leave(rt_channel *ch);

#endif
