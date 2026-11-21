#ifndef EUROS_API_H
#define EUROS_API_H

#include "keys.h"
#include "session.h"

#include <cJSON.h>

#include <stdbool.h>

// Error code shown to the user:
// "wrong_password", "invite_limit", "not_member", "rate_limited", "network", "unknown", ...
typedef struct
{
    char code[48];
    char detail[256];
} api_error;

// Sign in with derived credentials, fill the session (tokens + profile). remember → keep the seed.
bool api_sign_in(const euros_keys *keys, bool remember, euros_session *s, api_error *err);

// Refreshes the access token if it expires within a minute; saves the session.
bool api_ensure_fresh(euros_session *s, api_error *err);

// Revokes the refresh token on the server (best effort).
void api_sign_out(euros_session *s);

// POST /rest/v1/rpc/<fn> with a JSON body; returns parsed JSON (caller deletes) or NULL.
cJSON *api_rpc(euros_session *s, const char *fn, const char *json_body, api_error *err);

// GET /rest/v1/<query>; returns parsed JSON array (caller deletes) or NULL.
cJSON *api_select(euros_session *s, const char *query, api_error *err);

// POST /functions/v1/<name> as the signed-in user; returns parsed JSON (caller deletes) or NULL.
cJSON *api_function(euros_session *s, const char *name, const char *json_body, api_error *err);

// POST /rest/v1/<table> without reading the row back (RLS may hide it from us).
bool api_insert(euros_session *s, const char *table, const char *json_body, api_error *err);

// POST /rest/v1/<table> and read the new row back (columns: "id,created_at"); caller deletes. NULL on error.
cJSON *api_insert_returning(euros_session *s, const char *table, const char *json_body, const char *columns,
                           api_error *err);

// DELETE /rest/v1/<query>, e.g. "letters?id=eq.<uuid>".
bool api_delete(euros_session *s, const char *query, api_error *err);

// One broadcast event to a private channel over REST, without joining it.
bool api_broadcast(euros_session *s, const char *topic, const char *event, const cJSON *payload, api_error *err);

// Human text for an error code.
const char *api_explain(const api_error *e);

// Prints the error to stderr; returns the exit code 1.
int api_fail(const api_error *e);

// session_load, or tells the user to sign in.
bool api_load_session(euros_session *s);

#endif
