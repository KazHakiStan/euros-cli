#include "api.h"
#include "http.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static void set_err(api_error *err, const char *code, const char *detail)
{
    if (err == NULL)
    {
        return;
    }
    const char *text = detail != NULL
        ? detail
        : "";
    snprintf(err->code, sizeof err->code, "%s", code);
    snprintf(err->detail, sizeof err->detail, "%s", text);
}

// Backend message → error code.
static void error_from_response(const http_response *r, api_error *err)
{
    if (r->status == 0)
    {
        set_err(err, "network", r->error);
        return;
    }
    static const char *const known[] =
    {
        "invite_limit", 
        "not_member", 
        "rate_limited", 
        "group_too_big", 
        "not_allowed", 
        "no_such_user",
    };
    for (size_t i = 0; i < sizeof known / sizeof *known; ++i)
    {
        if (strstr(r->body, known[i]) != NULL)
        {
            set_err(err, known[i], NULL);
            return;
        }
    }
    if (strstr(r->body, "Invalid login credentials") != NULL)
    {
        set_err(err, "wrong_password", NULL);
        return;
    }
    set_err(err, "unknown", r->body);
}

static bool store_tokens(const cJSON *tok, euros_session *s)
{
    const cJSON *access = cJSON_GetObjectItemCaseSensitive(tok, "access_token");
    const cJSON *refresh = cJSON_GetObjectItemCaseSensitive(tok, "refresh_token");
    const cJSON *expires = cJSON_GetObjectItemCaseSensitive(tok, "expires_at");
    const cJSON *user = cJSON_GetObjectItemCaseSensitive(tok, "user");
    const cJSON *uid = cJSON_GetObjectItemCaseSensitive(user, "id");
    if (cJSON_IsString(access) == false || cJSON_IsString(refresh) == false || cJSON_IsString(uid) == false)
    {
        return false;
    }
    snprintf(s->access_token, sizeof s->access_token, "%s", access->valuestring);
    snprintf(s->refresh_token, sizeof s->refresh_token, "%s", refresh->valuestring);
    snprintf(s->user_id, sizeof s->user_id, "%s", uid->valuestring);
    s->expires_at = cJSON_IsNumber(expires)
        ? (long long)expires->valuedouble
        : (long long)time(NULL) + 3600;
    return true;
}

static bool token_request(const char *grant, const char *body, euros_session *s, api_error *err)
{
    char path[64];
    snprintf(path, sizeof path, "/auth/v1/token?grant_type=%s", grant);
    http_response r;
    bool ok = http_request("POST", path, NULL, body, NULL, &r) && r.status == 200;
    cJSON *tok = ok
        ? cJSON_Parse(r.body)
        : NULL;
    ok = tok != NULL && store_tokens(tok, s);
    if (ok == false)
    {
        error_from_response(&r, err);
    }
    cJSON_Delete(tok);
    http_free(&r);
    return ok;
}

bool api_sign_in(const euros_keys *keys, bool remember, euros_session *s, api_error *err)
{
    memset(s, 0, sizeof *s);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "email", keys->email);
    cJSON_AddStringToObject(body, "password", keys->password);
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    bool ok = json != NULL && token_request("password", json, s, err);
    if (json != NULL)
    {
        sodium_memzero(json, strlen(json));
        cJSON_free(json);
    }
    if (ok == false)
    {
        return false;
    }

    // No profile (or banned) → not a member.
    char q[128];
    snprintf(q, sizeof q, "profiles?select=nickname,rank&id=eq.%s", s->user_id);
    cJSON *rows = api_select(s, q, err);
    const cJSON *p = cJSON_GetArrayItem(rows, 0);
    const cJSON *nick = cJSON_GetObjectItemCaseSensitive(p, "nickname");
    const cJSON *rank = cJSON_GetObjectItemCaseSensitive(p, "rank");
    if (cJSON_IsString(nick) == false)
    {
        cJSON_Delete(rows);
        api_sign_out(s);
        set_err(err, "no_profile", NULL);
        return false;
    }
    const char *rank_name = cJSON_IsString(rank)
        ? rank->valuestring
        : "member";
    snprintf(s->nickname, sizeof s->nickname, "%s", nick->valuestring);
    snprintf(s->rank, sizeof s->rank, "%s", rank_name);
    cJSON_Delete(rows);

    s->has_seed = remember;
    if (remember)
    {
        memcpy(s->box_seed, keys->box_seed, sizeof s->box_seed);
    }
    if (session_save(s) == false)
    {
        set_err(err, "unknown", "can't write the session file");
        return false;
    }
    return true;
}

bool api_ensure_fresh(euros_session *s, api_error *err)
{
    if (s->expires_at - 60 > (long long)time(NULL))
    {
        return true;
    }
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "refresh_token", s->refresh_token);
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    bool ok = json != NULL && token_request("refresh_token", json, s, err);
    cJSON_free(json);
    if (ok == false)
    {
        // A network error stays a network error; anything else means the session is gone.
        const char *code = err != NULL
            ? err->code
            : "";
        if (strcmp(code, "network") != 0)
        {
            set_err(err, "session_expired", NULL);
        }
        return false;
    }
    return session_save(s);
}

void api_sign_out(euros_session *s)
{
    if (api_ensure_fresh(s, NULL))
    {
        http_response r;
        http_request("POST", "/auth/v1/logout?scope=local", s->access_token, NULL, NULL, &r);
        http_free(&r);
    }
}

static cJSON *rest(euros_session *s, const char *method, const char *path, const char *body, const char *header,
                   api_error *err)
{
    if (api_ensure_fresh(s, err) == false)
    {
        return NULL;
    }
    http_response r;
    bool ok = http_request(method, path, s->access_token, body, header, &r) && r.status >= 200 && r.status < 300;
    cJSON *json = NULL;
    if (ok)
    {
        json = r.len > 0
            ? cJSON_Parse(r.body)
            : cJSON_CreateNull();
    }
    if (ok == false)
    {
        error_from_response(&r, err);
    }
    else if (json == NULL)
    {
        set_err(err, "unknown", "bad JSON from the server");
    }
    http_free(&r);
    return json;
}

cJSON *api_rpc(euros_session *s, const char *fn, const char *json_body, api_error *err)
{
    const char *body = json_body != NULL
        ? json_body
        : "{}";
    char path[256];
    snprintf(path, sizeof path, "/rest/v1/rpc/%s", fn);
    return rest(s, "POST", path, body, NULL, err);
}

cJSON *api_select(euros_session *s, const char *query, api_error *err)
{
    char path[1024];
    snprintf(path, sizeof path, "/rest/v1/%s", query);
    return rest(s, "GET", path, NULL, NULL, err);
}

cJSON *api_function(euros_session *s, const char *name, const char *json_body, api_error *err)
{
    char path[256];
    snprintf(path, sizeof path, "/functions/v1/%s", name);
    return rest(s, "POST", path, json_body, NULL, err);
}

bool api_insert(euros_session *s, const char *table, const char *json_body, api_error *err)
{
    char path[256];
    snprintf(path, sizeof path, "/rest/v1/%s", table);
    cJSON *res = rest(s, "POST", path, json_body, NULL, err);
    bool ok = res != NULL;
    cJSON_Delete(res);
    return ok;
}

cJSON *api_insert_returning(euros_session *s, const char *table, const char *json_body, const char *columns,
                           api_error *err)
{
    char path[256];
    snprintf(path, sizeof path, "/rest/v1/%s?select=%s", table, columns);
    cJSON *rows = rest(s, "POST", path, json_body, "Prefer: return=representation", err);
    cJSON *row = cJSON_DetachItemFromArray(rows, 0);
    cJSON_Delete(rows);
    if (row == NULL && err != NULL && err->code[0] == '\0')
    {
        set_err(err, "unknown", "the server returned no row");
    }
    return row;
}

bool api_delete(euros_session *s, const char *query, api_error *err)
{
    char path[1024];
    snprintf(path, sizeof path, "/rest/v1/%s", query);
    cJSON *res = rest(s, "DELETE", path, NULL, NULL, err);
    bool ok = res != NULL;
    cJSON_Delete(res);
    return ok;
}

bool api_broadcast(euros_session *s, const char *topic, const char *event, const cJSON *payload, api_error *err)
{
    cJSON *body = cJSON_CreateObject();
    cJSON *messages = cJSON_AddArrayToObject(body, "messages");
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "topic", topic);
    cJSON_AddStringToObject(msg, "event", event);
    cJSON_AddItemToObject(msg, "payload", cJSON_Duplicate(payload, true));
    cJSON_AddBoolToObject(msg, "private", true);
    cJSON_AddItemToArray(messages, msg);
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (json == NULL)
    {
        set_err(err, "unknown", "out of memory");
        return false;
    }
    cJSON *res = rest(s, "POST", "/realtime/v1/api/broadcast", json, NULL, err);
    cJSON_free(json);
    bool ok = res != NULL;
    cJSON_Delete(res);
    return ok;
}

const char *api_explain(const api_error *e)
{
    static const struct
    {
        const char *code, *text;
    } texts[] =
    {
        { "wrong_password", "Wrong password" },
        { "invite_limit", "Invite limit reached (3)" },
        { "not_member", "You are not a member" },
        { "no_profile", "Profile not found" },
        { "rate_limited", "Too much at once. Try again later." },
        { "session_expired", "Session expired: run `euros login` again" },
        { "network", "Network error" },
        { "join_failed", "Could not join" },
        { "nick_exhausted", "Could not pick a nickname, try again" },
        { "not_allowed", "Not allowed" },
    };

    for (size_t i = 0; i < sizeof texts / sizeof *texts; ++i)
    {
        if (strcmp(e->code, texts[i].code) == 0)
        {
            return texts[i].text;
        }
    }
    return "Something went wrong";
}

int api_fail(const api_error *e)
{
    fprintf(stderr, "%s", api_explain(e));
    bool technical = strcmp(e->code, "unknown") == 0 || strcmp(e->code, "network") == 0;
    if (e->detail[0] != '\0' && technical)
    {
        fprintf(stderr, " (%s)", e->detail);
    }
    fputc('\n', stderr);
    return 1;
}

bool api_load_session(euros_session *s)
{
    if (session_load(s))
    {
        return true;
    }
    fprintf(stderr, "Not signed in. Run `euros login`.\n");
    return false;
}
