#include "config.h"

#include <stdlib.h>

#define DEFAULT_URL "https://mbxyuwdeixzuafiuhmee.supabase.co"
#define DEFAULT_ANON_KEY                                                                                               \
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."                                                                            \
    "eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Im1ieHl1d2RlaXh6dWFmaXVobWVlIiwicm9sZSI6ImFub24iLCJpYXQiOjE3OTUyMDk2MTMsImV4cCI6" \
    "MjExMDc4NTYxM30.Pq3KvTDLU8OLEqyYAFbAgtv7exFb4ORa_4-3ST4LORQ"

static const char *env_or(const char *name, const char *fallback)
{
    const char *value = getenv(name);
    return value != NULL && *value != '\0'
        ? value
        : fallback;
}

euros_config config_get(void)
{
    return (euros_config){
        .url = env_or("EUROS_URL", DEFAULT_URL),
        .anon_key = env_or("EUROS_ANON_KEY", DEFAULT_ANON_KEY),
    };
}
