#include "config.h"
#include "platform.h"
#include "realtime.h"
#include "ws.h"

#include <cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEARTBEAT_MS 25000
#define JOIN_TIMEOUT_MS 10000
#define JOIN_REF "1"

struct rt_channel
{
    ws_conn *ws;
    char topic[160]; // "realtime:<topic>"
    long next_ref;
    long long last_heartbeat;
    rt_handler handler;
    void *ctx;
};

// Phoenix protocol 1.0.0: every message is a JSON object { topic, event, payload, ref, join_ref }.
static long push(rt_channel *ch, const char *topic, const char *event, cJSON *payload, bool with_join_ref)
{
    long ref = ++ch->next_ref;
    char ref_text[24];
    snprintf(ref_text, sizeof ref_text, "%ld", ref);

    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "topic", topic);
    cJSON_AddStringToObject(msg, "event", event);
    cJSON_AddItemToObject(msg, "payload", payload);
    cJSON_AddStringToObject(msg, "ref", ref_text);
    if (with_join_ref)
    {
        cJSON_AddStringToObject(msg, "join_ref", JOIN_REF);
    }
    char *text = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    bool ok = text != NULL && ws_send_text(ch->ws, text);
    cJSON_free(text);
    return ok
        ? ref
        : 0;
}

static long ref_of(const cJSON *msg)
{
    const cJSON *ref = cJSON_GetObjectItemCaseSensitive(msg, "ref");
    return cJSON_IsString(ref)
        ? strtol(ref->valuestring, NULL, 10)
        : 0;
}

static void dispatch(rt_channel *ch, const cJSON *msg)
{
    const cJSON *topic = cJSON_GetObjectItemCaseSensitive(msg, "topic");
    const cJSON *event = cJSON_GetObjectItemCaseSensitive(msg, "event");
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(msg, "payload");
    if (cJSON_IsString(topic) == false || cJSON_IsString(event) == false || strcmp(topic->valuestring, ch->topic) != 0)
    {
        return; // heartbeat replies and other topics
    }

    if (strcmp(event->valuestring, "phx_reply") == 0)
    {
        const cJSON *status = cJSON_GetObjectItemCaseSensitive(payload, "status");
        const char *status_text = cJSON_IsString(status)
            ? status->valuestring
            : "error";
        cJSON *reply = cJSON_CreateObject();
        cJSON_AddNumberToObject(reply, "ref", (double)ref_of(msg));
        cJSON_AddStringToObject(reply, "status", status_text);
        ch->handler(ch->ctx, "reply", reply);
        cJSON_Delete(reply);
    }
    else if (strcmp(event->valuestring, "phx_error") == 0 || strcmp(event->valuestring, "phx_close") == 0)
    {
        ch->handler(ch->ctx, "closed", payload);
    }
    else
    {
        ch->handler(ch->ctx, event->valuestring, payload);
    }
}

// Reads everything that is already buffered. False if the connection is gone.
static bool drain(rt_channel *ch)
{
    for (;;)
    {
        char *text = NULL;
        int rc = ws_next_message(ch->ws, &text);
        if (rc == 0)
        {
            return true;
        }
        if (rc < 0)
        {
            return false;
        }
        cJSON *msg = cJSON_Parse(text);
        free(text);
        if (msg != NULL)
        {
            dispatch(ch, msg);
            cJSON_Delete(msg);
        }
    }
}

// The join reply arrives before anything else on the channel; wait for it directly.
static bool await_join(rt_channel *ch, long join_ref, char *err, size_t err_size)
{
    long long deadline = platform_now_ms() + JOIN_TIMEOUT_MS;
    while (platform_now_ms() < deadline)
    {
        char *text = NULL;
        int rc = ws_next_message(ch->ws, &text);
        if (rc < 0)
        {
            snprintf(err, err_size, "connection closed");
            return false;
        }
        if (rc == 0)
        {
            ws_wait(ch->ws, 200);
            continue;
        }
        cJSON *msg = cJSON_Parse(text);
        free(text);
        const cJSON *event = cJSON_GetObjectItemCaseSensitive(msg, "event");
        bool is_reply = cJSON_IsString(event) && strcmp(event->valuestring, "phx_reply") == 0 && ref_of(msg) == join_ref;
        if (is_reply == false)
        {
            if (msg != NULL)
            {
                dispatch(ch, msg);
            }
            cJSON_Delete(msg);
            continue;
        }
        const cJSON *payload = cJSON_GetObjectItemCaseSensitive(msg, "payload");
        const cJSON *status = cJSON_GetObjectItemCaseSensitive(payload, "status");
        bool ok = cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0;
        if (ok == false)
        {
            const cJSON *response = cJSON_GetObjectItemCaseSensitive(payload, "response");
            const cJSON *reason = cJSON_GetObjectItemCaseSensitive(response, "reason");
            const char *text_reason = cJSON_IsString(reason)
                ? reason->valuestring
                : "join refused";
            snprintf(err, err_size, "%s", text_reason);
        }
        cJSON_Delete(msg);
        return ok;
    }
    snprintf(err, err_size, "no answer from the server");
    return false;
}

rt_channel *rt_join(const char *topic, const char *presence_key, const cJSON *changes, const char *access_token,
                    rt_handler handler, void *ctx, bool *connected, char *err, size_t err_size)
{
    if (connected != NULL)
    {
        *connected = false;
    }
    euros_config cfg = config_get();
    const char *host = strstr(cfg.url, "://");
    host = host != NULL
        ? host + 3
        : cfg.url;
    char url[2048];
    snprintf(url, sizeof url, "wss://%s/realtime/v1/websocket?apikey=%s&vsn=1.0.0", host, cfg.anon_key);

    rt_channel *ch = calloc(1, sizeof *ch);
    if (ch == NULL)
    {
        snprintf(err, err_size, "out of memory");
        return NULL;
    }
    snprintf(ch->topic, sizeof ch->topic, "realtime:%s", topic);
    ch->handler = handler;
    ch->ctx = ctx;
    ch->ws = ws_connect(url, err, err_size);
    if (ch->ws == NULL)
    {
        free(ch);
        return NULL;
    }
    if (connected != NULL)
    {
        *connected = true;
    }

    cJSON *payload = cJSON_CreateObject();
    cJSON *config = cJSON_AddObjectToObject(payload, "config");
    cJSON *broadcast = cJSON_AddObjectToObject(config, "broadcast");
    cJSON_AddBoolToObject(broadcast, "ack", true);
    cJSON_AddBoolToObject(broadcast, "self", false);
    cJSON *presence = cJSON_AddObjectToObject(config, "presence");
    const char *key = presence_key != NULL
        ? presence_key
        : "";
    cJSON_AddStringToObject(presence, "key", key);
    cJSON_AddBoolToObject(presence, "enabled", presence_key != NULL);
    if (changes != NULL)
    {
        cJSON_AddItemToObject(config, "postgres_changes", cJSON_Duplicate(changes, true));
    }
    else
    {
        cJSON_AddArrayToObject(config, "postgres_changes");
    }
    cJSON_AddBoolToObject(config, "private", true);
    cJSON_AddStringToObject(payload, "access_token", access_token);

    long join_ref = push(ch, ch->topic, "phx_join", payload, true);
    if (join_ref == 0 || await_join(ch, join_ref, err, err_size) == false)
    {
        if (join_ref == 0)
        {
            snprintf(err, err_size, "send failed");
        }
        ws_close(ch->ws);
        free(ch);
        return NULL;
    }
    ch->last_heartbeat = platform_now_ms();
    return ch;
}

long rt_broadcast(rt_channel *ch, const char *event, const cJSON *payload)
{
    cJSON *wrapper = cJSON_CreateObject();
    cJSON_AddStringToObject(wrapper, "type", "broadcast");
    cJSON_AddStringToObject(wrapper, "event", event);
    cJSON_AddItemToObject(wrapper, "payload", cJSON_Duplicate(payload, true));
    return push(ch, ch->topic, "broadcast", wrapper, true);
}

bool rt_track(rt_channel *ch, const cJSON *meta)
{
    cJSON *wrapper = cJSON_CreateObject();
    cJSON_AddStringToObject(wrapper, "type", "presence");
    cJSON_AddStringToObject(wrapper, "event", "track");
    cJSON_AddItemToObject(wrapper, "payload", cJSON_Duplicate(meta, true));
    return push(ch, ch->topic, "presence", wrapper, true) != 0;
}

bool rt_set_token(rt_channel *ch, const char *access_token)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "access_token", access_token);
    return push(ch, ch->topic, "access_token", payload, true) != 0;
}

bool rt_pump(rt_channel *ch, int timeout_ms)
{
    if (drain(ch) == false)
    {
        return false;
    }
    long long now = platform_now_ms();
    if (now - ch->last_heartbeat >= HEARTBEAT_MS)
    {
        ch->last_heartbeat = now;
        if (push(ch, "phoenix", "heartbeat", cJSON_CreateObject(), false) == 0)
        {
            return false;
        }
    }
    ws_wait(ch->ws, timeout_ms);
    return drain(ch);
}

void rt_leave(rt_channel *ch)
{
    if (ch == NULL)
    {
        return;
    }
    push(ch, ch->topic, "phx_leave", cJSON_CreateObject(), true);
    ws_close(ch->ws);
    free(ch);
}
