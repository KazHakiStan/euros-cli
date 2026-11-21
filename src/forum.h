#ifndef EUROS_FORUM_H
#define EUROS_FORUM_H

#include "session.h"

#include <stddef.h>

#define FORUM_MAX_TITLE 120
#define FORUM_MAX_BODY 8000
#define FORUM_MAX_BOARD_NAME 48
#define FORUM_MAX_BOARD_DESC 200
#define FORUM_SHORT_ID 6

// `euros forum ...`. Returns the process exit code.
int forum_run(int argc, char **argv);

// Threads by others with activity not seen on this device yet; -1 if it can't be checked.
int forum_unread(euros_session *s);

// "burns in 5h" / "burns in 3d", like the web client.
void forum_burns_in(long long expires_s, long long now_s, char *out, size_t size);

#endif
