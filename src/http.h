#ifndef EUROS_HTTP_H
#define EUROS_HTTP_H

#include <curl/curl.h>

#include <stdbool.h>
#include <stddef.h>

typedef struct
{
    long status; // HTTP status, 0 if the request didn't happen
    char *body;  // response body (NUL-terminated), caller frees with http_free
    size_t len;
    char error[CURL_ERROR_SIZE]; // transport error text, if any
} http_response;

bool http_global_init(void);
void http_global_cleanup(void);

// Applies TLS settings shared by every handle (system CA store on Windows, the distro bundle on Linux).
void http_setup_tls(CURL *curl);

// "euros-cli/<version>"
const char *http_user_agent(void);

// GET an absolute URL outside the backend (no Supabase headers). accept may be NULL.
bool http_get_url(const char *url, const char *accept, long timeout_s, http_response *out);

// method: "GET" | "POST" | "PATCH" | "DELETE". bearer: user access token or NULL (→ anon key).
// json_body may be NULL. extra_header: one more "Name: value" header or NULL.
bool http_request(const char *method, const char *path, const char *bearer, const char *json_body,
                  const char *extra_header, http_response *out);

void http_free(http_response *r);

#endif
