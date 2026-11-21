#include "platform.h"
#include "session.h"

#include <cJSON.h>

#include <sodium.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool session_path(char *out, size_t size)
{
    char dir[1024];
    if (platform_config_dir(dir, sizeof dir) == false)
    {
        return false;
    }
#ifdef _WIN32
    return (size_t)snprintf(out, size, "%s\\session.json", dir) < size;
#else
    return (size_t)snprintf(out, size, "%s/session.json", dir) < size;
#endif
}

static void copy_str(char *dst, size_t size, const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    const char *value = cJSON_IsString(v)
        ? v->valuestring
        : "";
    snprintf(dst, size, "%s", value);
}

static bool read_seed(const cJSON *obj, unsigned char seed[32])
{
    const cJSON *hex = cJSON_GetObjectItemCaseSensitive(obj, "box_seed");
    if (cJSON_IsString(hex) == false)
    {
        return false;
    }
    size_t len = 0;
    return sodium_hex2bin(seed, 32, hex->valuestring, strlen(hex->valuestring), NULL, &len, NULL) == 0
        && len == 32;
}

bool session_load(euros_session *s)
{
    memset(s, 0, sizeof *s);
    char path[1100];
    if (session_path(path, sizeof path) == false)
    {
        return false;
    }
    char *text = platform_read_file(path, NULL);
    if (text == NULL)
    {
        return false;
    }
    cJSON *obj = cJSON_Parse(text);
    sodium_memzero(text, strlen(text));
    free(text);
    if (obj == NULL)
    {
        return false;
    }

    copy_str(s->user_id, sizeof s->user_id, obj, "user_id");
    copy_str(s->nickname, sizeof s->nickname, obj, "nickname");
    copy_str(s->rank, sizeof s->rank, obj, "rank");
    copy_str(s->access_token, sizeof s->access_token, obj, "access_token");
    copy_str(s->refresh_token, sizeof s->refresh_token, obj, "refresh_token");
    const cJSON *exp = cJSON_GetObjectItemCaseSensitive(obj, "expires_at");
    s->expires_at = cJSON_IsNumber(exp)
        ? (long long)exp->valuedouble
        : 0;
    s->has_seed = read_seed(obj, s->box_seed);
    cJSON_Delete(obj);
    return s->user_id[0] != '\0' && s->refresh_token[0] != '\0';
}

bool session_save(const euros_session *s)
{
    char path[1100];
    if (session_path(path, sizeof path) == false)
    {
        return false;
    }
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "user_id", s->user_id);
    cJSON_AddStringToObject(obj, "nickname", s->nickname);
    cJSON_AddStringToObject(obj, "rank", s->rank);
    cJSON_AddStringToObject(obj, "access_token", s->access_token);
    cJSON_AddStringToObject(obj, "refresh_token", s->refresh_token);
    cJSON_AddNumberToObject(obj, "expires_at", (double)s->expires_at);
    if (s->has_seed)
    {
        char hex[65];
        sodium_bin2hex(hex, sizeof hex, s->box_seed, sizeof s->box_seed);
        cJSON_AddStringToObject(obj, "box_seed", hex);
        sodium_memzero(hex, sizeof hex);
    }
    char *text = cJSON_Print(obj);
    cJSON_Delete(obj);
    if (text == NULL)
    {
        return false;
    }
    bool ok = platform_write_private(path, text, strlen(text));
    sodium_memzero(text, strlen(text));
    free(text);
    return ok;
}

bool session_clear(void)
{
    char path[1100];
    return session_path(path, sizeof path) && platform_remove(path);
}

void session_wipe(euros_session *s)
{
    sodium_memzero(s, sizeof *s);
}
