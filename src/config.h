#ifndef EUROS_CONFIG_H
#define EUROS_CONFIG_H

typedef struct
{
    const char *url;      // https://<ref>.supabase.co
    const char *anon_key; // public API key
} euros_config;

// Built-in defaults, overridable with EUROS_URL / EUROS_ANON_KEY environment variables.
euros_config config_get(void);

#endif
