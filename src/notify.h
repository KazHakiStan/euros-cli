#ifndef EUROS_NOTIFY_H
#define EUROS_NOTIFY_H

#include <cJSON.h>

#include <stdbool.h>
#include <stddef.h>

// Notifications collected by `euros chat` for the user (and the terminal: WezTerm reads the file
// for its badge). They live in <config dir>/notifications.json, readable only by the user:
//
//   { "v": 1, "unread": 3, "mentions": 1, "next": 17,
//     "items": [ { "n": 16, "ts": <ms>, "kind": "mention" | "message" | "letter" | "receipt",
//                  "room": "#everyone", "topic": "room:main", "from": "Nick", "text": "...",
//                  "id": "<message or letter id>", "pane": "<WEZTERM_PANE>", "read": false }, ... ],
//     "seen": [ "<message id>", ... ] }
//
// Only mentions keep their text: other room messages are counted, their words stay in the room.

typedef struct
{
    const char *kind;  // "mention", "message", "letter", "receipt"
    const char *id;    // message / letter id, or NULL: the same id is never added twice
    const char *topic; // "room:main", or NULL
    const char *room;  // "#everyone", or NULL
    const char *from;  // a nickname, or NULL
    const char *text;  // mentions only, or NULL
    long long ts;      // ms
} note;

// Pure operations on a loaded document (the tests use them directly).
cJSON *notify_doc_new(void);
bool notify_doc_add(cJSON *doc, const note *n, const char *pane); // false: a known id
void notify_doc_seen(cJSON *doc, const char *id);                  // seen elsewhere: read, never added
int notify_doc_mark_read(cJSON *doc, const char *topic);           // topic NULL: all; returns how many
int notify_doc_mark_item(cJSON *doc, double n);                    // the item numbered n
void notify_doc_recount(cJSON *doc);

// Readable lines, newest last: one per mention / letter / receipt, one per room for plain
// messages. only_unread false: the recent ones too. Returns how many lines were emitted.
int notify_doc_lines(const cJSON *doc, bool only_unread, void (*emit)(void *ctx, const char *line), void *ctx);

// The same on the file, under a lock between processes. All return false if it couldn't be saved.
bool notify_add(const note *n);
bool notify_seen(const char *id);
bool notify_mark_read(const char *topic);
bool notify_mark_item(double n);
bool notify_clear(void);
cJSON *notify_load(void); // never NULL

// `euros notifs [--all] [--keep] | read [<topic> | <n>] | clear`
int notify_cli(int argc, char **argv);

#endif
