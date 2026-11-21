#ifndef EUROS_LETTERS_H
#define EUROS_LETTERS_H

#include "api.h"
#include "session.h"

#include <cJSON.h>

#include <stdbool.h>

#define LETTER_MAX_BODY 4000

// `euros letters`: unread letters, oldest first. Returns the process exit code.
int letters_list(void);

// `euros read [n]`: shows letter n (1 = oldest), burns it when closed, tells the sender.
int letters_read(int argc, char **argv);

// `euros write <nick> [--ttl 1|7|30]`: the text comes from stdin.
int letters_write(int argc, char **argv);

// Unread letters to the signed-in user, oldest first:
// [{ id, from_id, created_at, expires_at[, ciphertext] }]. NULL on error.
cJSON *letters_fetch(euros_session *s, bool with_ciphertext, api_error *err);

// postgres_changes bindings for "a letter to user_id arrived" (caller deletes).
cJSON *letters_changes_binding(const char *user_id);

// Whole days until expires_iso, rounded up, at least 1.
int letters_days_left(const char *expires_iso, long long now_s);

// Not blank and at most LETTER_MAX_BODY UTF-16 units.
bool letters_valid_body(const char *body);

#endif
