#ifndef EUROS_REALTIME_H
#define EUROS_REALTIME_H

#include <cJSON.h>

#include <stdbool.h>
#include <stddef.h>

// One WebSocket to the relay carries any number of channels (Phoenix multiplexes them by topic).
typedef struct rt_socket rt_socket;
typedef struct rt_channel rt_channel;

// Incoming channel traffic:
//   "broadcast"      payload = { "event": "...", "payload": {...} }
//   "presence_state" payload = { "<key>": { "metas": [...] }, ... }
//   "presence_diff"  payload = { "joins": {...}, "leaves": {...} }
//   "postgres_changes" payload = { "ids": [...], "data": { "type", "table", "record": {...}, ... } }
//   "reply"          payload = { "ref": <number>, "status": "ok" | "error" } for our pushes
//   "closed"         the server dropped this channel (the socket may still be up)
typedef void (*rt_handler)(void *ctx, const char *event, const cJSON *payload);

// Opens the WebSocket. NULL on failure (reason in err).
rt_socket *rt_connect(char *err, size_t err_size);

// Joins a private channel (e.g. topic "room:main") on the socket. Blocks until the join is answered;
// traffic of the other channels meanwhile goes to their handlers. presence_key: the user id, or NULL
// for no presence. changes: postgres_changes bindings ([{ event, schema, table, filter }]) or NULL.
// NULL on failure (reason in err).
rt_channel *rt_join(rt_socket *s, const char *topic, const char *presence_key, const cJSON *changes,
                    const char *access_token, rt_handler handler, void *ctx, char *err, size_t err_size);

// Pushes { type: "broadcast", event, payload }. Returns the push ref (> 0), or 0 if sending failed.
long rt_broadcast(rt_channel *ch, const char *event, const cJSON *payload);

// Tracks this client's presence meta.
bool rt_track(rt_channel *ch, const cJSON *meta);

// Hands a refreshed access token to the server, for every channel on the socket.
bool rt_set_token(rt_socket *s, const char *access_token);

// Processes incoming messages and keeps the heartbeat, waiting up to timeout_ms.
// False once the connection is gone.
bool rt_pump(rt_socket *s, int timeout_ms);

// Leaves one channel (the socket stays). NULL is fine.
void rt_leave(rt_channel *ch);

// Leaves every channel and closes the socket. NULL is fine.
void rt_close(rt_socket *s);

#endif
