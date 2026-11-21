#ifndef EUROS_CLI_H
#define EUROS_CLI_H

#include "api.h"
#include "session.h"

#include <cJSON.h>

#include <stdbool.h>
#include <stddef.h>

#define CLI_DAY_S 86400LL

// session_load + a fresh token; tells the user what's wrong and returns false otherwise.
bool cli_open_session(euros_session *s);

// [{ id, nickname, public_key }] of every member. NULL on error.
cJSON *cli_load_profiles(euros_session *s, api_error *err);

const cJSON *cli_profile_by(const cJSON *profiles, const char *key, const char *value, bool ignore_case);

// Nickname for a user id, "???" if unknown.
const char *cli_nick(const cJSON *profiles, const char *user_id);

// String member of an object, or NULL.
const char *cli_str(const cJSON *obj, const char *key);

bool cli_same_text_ci(const char *a, const char *b);

// The ANSI code when stdout is a terminal, else "".
const char *cli_sty(const char *code);

// Bold nickname in its color, control characters removed.
void cli_put_nick(const char *nick);

// ISO 8601 timestamp ("2026-11-21T01:02:03.456+00:00", "...Z") → Unix seconds; -1 if malformed.
long long cli_parse_time(const char *iso);

// "21.11 02:14" in local time, "?" if malformed.
void cli_local_time(const char *iso, char *out, size_t size);

// A whole positive number up to 100000.
bool cli_parse_number(const char *text, int *out);

// Unicode code points in UTF-8 text (what Postgres length() counts).
size_t cli_codepoints(const char *utf8);

// True if there is something besides whitespace.
bool cli_has_text(const char *s);

// Multi-line text from the keyboard (until a line with a single dot; intro is shown first on stderr)
// or from a pipe. NULL if it is over 64 KiB (*too_long set) or memory runs out. Free with cli_free_text.
char *cli_read_text(const char *intro, bool *too_long);

// Wipes and frees.
void cli_free_text(char *text);

#endif
