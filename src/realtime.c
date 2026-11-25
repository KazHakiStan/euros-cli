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

struct rt_channel
{
    rt_socket *socket;
    rt_channel *next;
    char topic[160];   // "realtime:<topic>"
    char join_ref[24]; // the ref of our phx_join: Phoenix tells channel instances apart by it
    rt_handler handler;
    void *ctx;
};

struct rt_socket
{
    ws_conn *ws;
    long next_ref;
    long long last_heartbeat;
    rt_channel *channels;
};

// Phoenix protocol 1.0.0: every message is a JSON object { topic, event, payload, ref, join_ref }.
static long push(rt_socket *s, const char *topic, const char *event, cJSON *payload, const char *join_ref)
{
    long ref = ++s->next_ref;
    char ref_text[24];
    snprintf(ref_text, sizeof ref_text, "%ld", ref);

    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "topic", topic);
    cJSON_AddStringToObject(msg, "event", event);
    cJSON_AddItemToObject(msg, "payload", payload);
    cJSON_AddStringToObject(msg, "ref", ref_text);
    if (join_ref != NULL)
    {
        // the join itself carries its own ref as join_ref
        cJSON_AddStringToObject(msg, "join_ref", join_ref[0] != '\0' ? join_ref : ref_text);
    }
    char *text = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    bool ok = text != NULL && ws_send_text(s->ws, text);
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

static rt_channel *channel_of(rt_socket *s, const char *topic)
{
    for (rt_channel *ch = s->channels; ch != NULL; ch = ch->next)
    {
        if (strcmp(ch->topic, topic) == 0)
        {
            return ch;
        }
    }
    return NULL;
}

static void dispatch(rt_socket *s, const cJSON *msg)
{
    const cJSON *topic = cJSON_GetObjectItemCaseSensitive(msg, "topic");
    const cJSON *event = cJSON_GetObjectItemCaseSensitive(msg, "event");
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(msg, "payload");
    if (cJSON_IsString(topic) == false || cJSON_IsString(event) == false)
    {
        return;
    }
    rt_channel *ch = channel_of(s, topic->valuestring);
    if (ch == NULL)
    {
        return; // heartbeat replies, channels being joined or already left
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
static bool drain(rt_socket *s)
{
    for (;;)
    {
        char *text = NULL;
        int rc = ws_next_message(s->ws, &text);
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
            dispatch(s, msg);
            cJSON_Delete(msg);
        }
    }
}

// Waits for the reply to our join; everything else on the socket is dispatched meanwhile.
static bool await_join(rt_socket *s, const char *topic, long join_ref, char *err, size_t err_size)
{
    long long deadline = platform_now_ms() + JOIN_TIMEOUT_MS;
    while (platform_now_ms() < deadline)
    {
        char *text = NULL;
        int rc = ws_next_message(s->ws, &text);
        if (rc < 0)
        {
            snprintf(err, err_size, "connection closed");
            return false;
        }
        if (rc == 0)
        {
            ws_wait(s->ws, 200);
            continue;
        }
        cJSON *msg = cJSON_Parse(text);
        free(text);
        const cJSON *event = cJSON_GetObjectItemCaseSensitive(msg, "event");
        const cJSON *msg_topic = cJSON_GetObjectItemCaseSensitive(msg, "topic");
        bool is_reply = cJSON_IsString(event) && strcmp(event->valuestring, "phx_reply") == 0
            && cJSON_IsString(msg_topic) && strcmp(msg_topic->valuestring, topic) == 0 && ref_of(msg) == join_ref;
        if (is_reply == false)
        {
            if (msg != NULL)
            {
                dispatch(s, msg);
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

rt_socket *rt_connect(char *err, size_t err_size)
{
    euros_config cfg = config_get();
    const char *host = strstr(cfg.url, "://");
    host = host != NULL
        ? host + 3
        : cfg.url;
    char url[2048];
    snprintf(url, sizeof url, "wss://%s/realtime/v1/websocket?apikey=%s&vsn=1.0.0", host, cfg.anon_key);

    rt_socket *s = calloc(1, sizeof *s);
    if (s == NULL)
    {
        snprintf(err, err_size, "out of memory");
        return NULL;
    }
    s->ws = ws_connect(url, err, err_size);
    if (s->ws == NULL)
    {
        free(s);
        return NULL;
    }
    s->last_heartbeat = platform_now_ms();
    return s;
}

rt_channel *rt_join(rt_socket *s, const char *topic, const char *presence_key, const cJSON *changes,
                    const char *access_token, rt_handler handler, void *ctx, char *err, size_t err_size)
{
    char full_topic[160];
    snprintf(full_topic, sizeof full_topic, "realtime:%s", topic);
    if (channel_of(s, full_topic) != NULL)
    {
        snprintf(err, err_size, "already joined");
        return NULL;
    }
    rt_channel *ch = calloc(1, sizeof *ch);
    if (ch == NULL)
    {
        snprintf(err, err_size, "out of memory");
        return NULL;
    }
    snprintf(ch->topic, sizeof ch->topic, "%s", full_topic);
    ch->socket = s;
    ch->handler = handler;
    ch->ctx = ctx;

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

    long join_ref = push(s, ch->topic, "phx_join", payload, "");
    if (join_ref == 0)
    {
        snprintf(err, err_size, "send failed");
        free(ch);
        return NULL;
    }
    snprintf(ch->join_ref, sizeof ch->join_ref, "%ld", join_ref);
    if (await_join(s, ch->topic, join_ref, err, err_size) == false)
    {
        push(s, ch->topic, "phx_leave", cJSON_CreateObject(), ch->join_ref); // in case it joins late
        free(ch);
        return NULL;
    }
    // Only now on the list: what came before the join reply belongs to no one yet.
    ch->next = s->channels;
    s->channels = ch;
    return ch;
}

long rt_broadcast(rt_channel *ch, const char *event, const cJSON *payload)
{
    cJSON *wrapper = cJSON_CreateObject();
    cJSON_AddStringToObject(wrapper, "type", "broadcast");
    cJSON_AddStringToObject(wrapper, "event", event);
    cJSON_AddItemToObject(wrapper, "payload", cJSON_Duplicate(payload, true));
    return push(ch->socket, ch->topic, "broadcast", wrapper, ch->join_ref);
}

bool rt_track(rt_channel *ch, const cJSON *meta)
{
    cJSON *wrapper = cJSON_CreateObject();
    cJSON_AddStringToObject(wrapper, "type", "presence");
    cJSON_AddStringToObject(wrapper, "event", "track");
    cJSON_AddItemToObject(wrapper, "payload", cJSON_Duplicate(meta, true));
    return push(ch->socket, ch->topic, "presence", wrapper, ch->join_ref) != 0;
}

bool rt_set_token(rt_socket *s, const char *access_token)
{
    bool ok = true;
    for (rt_channel *ch = s->channels; ch != NULL; ch = ch->next)
    {
        cJSON *payload = cJSON_CreateObject();
        cJSON_AddStringToObject(payload, "access_token", access_token);
        ok = push(s, ch->topic, "access_token", payload, ch->join_ref) != 0 && ok;
    }
    return ok;
}

bool rt_pump(rt_socket *s, int timeout_ms)
{
    if (drain(s) == false)
    {
        return false;
    }
    long long now = platform_now_ms();
    if (now - s->last_heartbeat >= HEARTBEAT_MS)
    {
        s->last_heartbeat = now;
        if (push(s, "phoenix", "heartbeat", cJSON_CreateObject(), NULL) == 0)
        {
            return false;
        }
    }
    ws_wait(s->ws, timeout_ms);
    return drain(s);
}

void rt_leave(rt_channel *ch)
{
    if (ch == NULL)
    {
        return;
    }
    rt_socket *s = ch->socket;
    for (rt_channel **p = &s->channels; *p != NULL; p = &(*p)->next)
    {
        if (*p == ch)
        {
            *p = ch->next;
            break;
        }
    }
    push(s, ch->topic, "phx_leave", cJSON_CreateObject(), ch->join_ref);
    free(ch);
}

void rt_close(rt_socket *s)
{
    if (s == NULL)
    {
        return;
    }
    while (s->channels != NULL)
    {
        rt_leave(s->channels);
    }
    ws_close(s->ws);
    free(s);
}
