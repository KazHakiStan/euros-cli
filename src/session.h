#ifndef EUROS_SESSION_H
#define EUROS_SESSION_H

#include <stdbool.h>

typedef struct
{
    char user_id[40];
    char nickname[40];
    char rank[16];
    char access_token[4096];
    char refresh_token[256];
    long long expires_at; // unix seconds
    bool has_seed;
    unsigned char box_seed[32];
} euros_session;

bool session_load(euros_session *s); // false if not signed in
bool session_save(const euros_session *s);
bool session_clear(void);            // log out locally
void session_wipe(euros_session *s); // zero memory

#endif
