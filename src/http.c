#include "config.h"
#include "http.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#  include <unistd.h>
#endif

bool http_global_init(void)
{
    return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
}

void http_global_cleanup(void)
{
    curl_global_cleanup();
}

#if defined(__linux__)
// A static release binary can't rely on the bundle path compiled into libcurl: distros keep it in different places.
static const char *linux_ca_bundle(void)
{
    static const char *const paths[] =
    {
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/ssl/ca-bundle.pem",
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
        "/etc/ssl/cert.pem",
    };
    const char *env = getenv("SSL_CERT_FILE");
    if (env != NULL && access(env, R_OK) == 0)
    {
        return env;
    }
    for (size_t i = 0; i < sizeof paths / sizeof *paths; ++i)
    {
        if (access(paths[i], R_OK) == 0)
        {
            return paths[i];
        }
    }
    return NULL;
}
#endif

void http_setup_tls(CURL *curl)
{
#ifdef _WIN32
    // Use the Windows certificate store instead of a CA bundle file.
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#elif defined(__linux__)
    const char *bundle = linux_ca_bundle();
    if (bundle != NULL)
    {
        curl_easy_setopt(curl, CURLOPT_CAINFO, bundle);
    }
#endif
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
}

const char *http_user_agent(void)
{
    static char agent[64];
    if (agent[0] == '\0')
    {
        snprintf(agent, sizeof agent, "euros-cli/%s", version_current());
    }
    return agent;
}

static size_t on_data(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    http_response *r = userdata;
    size_t n = size * nmemb;
    char *grown = realloc(r->body, r->len + n + 1);
    if (grown == NULL)
    {
        return 0; // aborts the transfer
    }
    r->body = grown;
    memcpy(r->body + r->len, ptr, n);
    r->len += n;
    r->body[r->len] = '\0';
    return n;
}

bool http_request(const char *method, const char *path, const char *bearer, const char *json_body,
                  const char *extra_header, http_response *out)
{
    memset(out, 0, sizeof *out);
    euros_config cfg = config_get();
    CURL *curl = curl_easy_init();
    if (curl == NULL)
    {
        snprintf(out->error, sizeof out->error, "curl init failed");
        return false;
    }

    const char *token = bearer != NULL
        ? bearer
        : cfg.anon_key;
    char url[2048];
    snprintf(url, sizeof url, "%s%s", cfg.url, path);
    char apikey[1024], auth[4096];
    snprintf(apikey, sizeof apikey, "apikey: %s", cfg.anon_key);
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", token);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, apikey);
    headers = curl_slist_append(headers, auth);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    if (extra_header != NULL)
    {
        headers = curl_slist_append(headers, extra_header);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, out->error);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, http_user_agent());
    if (json_body != NULL)
    {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(json_body));
    }
    http_setup_tls(curl);

    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK)
    {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out->status);
    }
    else if (out->error[0] == '\0')
    {
        snprintf(out->error, sizeof out->error, "%s", curl_easy_strerror(rc));
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (out->body == NULL)
    {
        out->body = calloc(1, 1);
    }
    return rc == CURLE_OK;
}

bool http_get_url(const char *url, const char *accept, long timeout_s, http_response *out)
{
    memset(out, 0, sizeof *out);
    CURL *curl = curl_easy_init();
    if (curl == NULL)
    {
        snprintf(out->error, sizeof out->error, "curl init failed");
        return false;
    }
    struct curl_slist *headers = NULL;
    char accept_header[128];
    if (accept != NULL)
    {
        snprintf(accept_header, sizeof accept_header, "Accept: %s", accept);
        headers = curl_slist_append(headers, accept_header);
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, out->error);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, http_user_agent());
    http_setup_tls(curl);

    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK)
    {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out->status);
    }
    else if (out->error[0] == '\0')
    {
        snprintf(out->error, sizeof out->error, "%s", curl_easy_strerror(rc));
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (out->body == NULL)
    {
        out->body = calloc(1, 1);
    }
    return rc == CURLE_OK;
}

void http_free(http_response *r)
{
    free(r->body);
    r->body = NULL;
    r->len = 0;
}
