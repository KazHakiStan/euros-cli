#ifndef EUROS_ROOM_H
#define EUROS_ROOM_H

#include <cJSON.h>

#include <stdbool.h>

#define ROOM_MAX_BODY 2000
#define ROOM_MAX_IMAGE_B64 190000

typedef struct room room;

typedef struct
{
    // Sends a broadcast event; returns its ref (> 0) or 0 if it couldn't be sent.
    long (*send)(void *ctx, const char *event, const cJSON *payload);
    void *send_ctx;

    void (*on_message)(void *ui, const cJSON *msg);   // a live message, including our own
    void (*on_history)(void *ui, const cJSON *msgs);  // messages handed over by the room, oldest first
    void (*on_presence)(void *ui, int online);        // distinct people in the room
    void (*on_failed)(void *ui, const cJSON *msg);    // our message was rejected by the server
    void (*on_join)(void *ui, const char *user_id);   // optional: a person entered (not on the first snapshot)
    void (*on_leave)(void *ui, const char *user_id);  // optional: a person's last tab left
    void *ui;
} room_hooks;

room *room_create(const char *user_id, const char *nickname, room_hooks hooks);
void room_destroy(room *r);

// Presence meta to track for this client (caller deletes).
cJSON *room_presence_meta(const room *r);

// Call right after joining: opens the window in which history is requested.
void room_joined(room *r, long long now_ms);

// Feeds channel traffic (see rt_handler).
void room_on_event(room *r, const char *event, const cJSON *payload, long long now_ms);

// Sends a text message. False if it is invalid or couldn't be sent.
bool room_send_text(room *r, const char *body, long long now_ms);

// Sends a message with an optional picture { mime, w, h, data } (body may then be empty).
bool room_send(room *r, const char *body, const cJSON *img, long long now_ms);

int room_online(const room *r);

// True while room memory may still arrive: presence not synced yet, or someone is here to ask.
bool room_waiting_history(const room *r);

// { "<user id>": { "metas": [ { client, nick, since, phx_ref }, ... ] }, ... }
const cJSON *room_presence(const room *r);

bool room_valid_message(const cJSON *msg);

// Length as JavaScript counts it (UTF-16 code units).
size_t room_utf16_length(const char *utf8);

#endif
