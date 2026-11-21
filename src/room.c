#include "room.h"

#include <cJSON.h>

#include <sodium.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROTOCOL 1
#define MEMORY 200
#define SHARE 50
#define KEEP_IMAGES 30
#define HIST_WINDOW_MS 5000
#define MAX_PENDING 32

typedef struct
{
    long ref;
    cJSON *msg;
} pending_send;

struct room
{
    char user_id[40];
    char nickname[40];
    char client[37];
    long long since;
    room_hooks hooks;

    cJSON *msgs;     // array, oldest first
    cJSON *presence; // object, see room_presence()
    bool presence_known;
    bool asked;
    bool got_history;
    long long ask_until;
    pending_send pending[MAX_PENDING];
};

static void make_uuid(char out[37])
{
    unsigned char b[16];
    randombytes_buf(b, sizeof b);
    b[6] = (unsigned char)((b[6] & 0x0f) | 0x40); // version 4
    b[8] = (unsigned char)((b[8] & 0x3f) | 0x80); // RFC 4122 variant
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

size_t room_utf16_length(const char *s)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; ++p)
    {
        if ((*p & 0xc0) == 0x80)
        {
            continue; // continuation byte
        }
        n += (*p >= 0xf0)
            ? 2  // outside the BMP: a surrogate pair
            : 1;
    }
    return n;
}

static bool is_short_string(const cJSON *item, size_t max)
{
    return cJSON_IsString(item) && room_utf16_length(item->valuestring) <= max;
}

static bool is_base64(const char *s)
{
    size_t len = strlen(s);
    if (len == 0)
    {
        return false;
    }
    size_t pad = 0;
    for (size_t i = 0; i < len; ++i)
    {
        unsigned char c = (unsigned char)s[i];
        bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (c == '=')
        {
            ++pad;
        }
        else if (pad > 0 || (alnum == false && c != '+' && c != '/'))
        {
            return false;
        }
    }
    return pad <= 2;
}

static bool is_dimension(const cJSON *item)
{
    return cJSON_IsNumber(item) && item->valuedouble == floor(item->valuedouble)
        && item->valuedouble >= 1 && item->valuedouble <= 4096;
}

static bool valid_image(const cJSON *img)
{
    if (img == NULL || cJSON_IsNull(img))
    {
        return true;
    }
    if (cJSON_IsObject(img) == false)
    {
        return false;
    }
    if (is_dimension(cJSON_GetObjectItemCaseSensitive(img, "w")) == false
        || is_dimension(cJSON_GetObjectItemCaseSensitive(img, "h")) == false)
    {
        return false;
    }
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(img, "data");
    if (data == NULL || cJSON_IsNull(data))
    {
        return true; // stub: the picture was only seen live
    }
    const cJSON *mime = cJSON_GetObjectItemCaseSensitive(img, "mime");
    return cJSON_IsString(mime) && strcmp(mime->valuestring, "image/jpeg") == 0
        && cJSON_IsString(data) && strlen(data->valuestring) <= ROOM_MAX_IMAGE_B64
        && is_base64(data->valuestring);
}

static bool has_text(const char *s)
{
    for (; *s != '\0'; ++s)
    {
        if (*s != ' ' && *s != '\t' && *s != '\n' && *s != '\r')
        {
            return true;
        }
    }
    return false;
}

bool room_valid_message(const cJSON *m)
{
    if (cJSON_IsObject(m) == false)
    {
        return false;
    }
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(m, "v");
    const cJSON *body = cJSON_GetObjectItemCaseSensitive(m, "body");
    const cJSON *ts = cJSON_GetObjectItemCaseSensitive(m, "ts");
    const cJSON *img = cJSON_GetObjectItemCaseSensitive(m, "img");
    bool has_img = img != NULL && cJSON_IsNull(img) == false;
    return cJSON_IsNumber(v) && v->valuedouble == PROTOCOL
        && is_short_string(cJSON_GetObjectItemCaseSensitive(m, "id"), 64)
        && is_short_string(cJSON_GetObjectItemCaseSensitive(m, "author"), 64)
        && is_short_string(cJSON_GetObjectItemCaseSensitive(m, "nick"), 32)
        && is_short_string(body, ROOM_MAX_BODY)
        && (has_text(body->valuestring) || has_img)
        && cJSON_IsNumber(ts) && isfinite(ts->valuedouble)
        && valid_image(img);
}

static const char *str_of(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v)
        ? v->valuestring
        : "";
}

static double num_of(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v)
        ? v->valuedouble
        : 0;
}

static bool has_image_data(const cJSON *m)
{
    const cJSON *img = cJSON_GetObjectItemCaseSensitive(m, "img");
    return cJSON_IsString(cJSON_GetObjectItemCaseSensitive(img, "data"));
}

// Copy with the picture replaced by a stub (only its size).
static cJSON *stripped(const cJSON *m)
{
    cJSON *copy = cJSON_Duplicate(m, true);
    cJSON *img = cJSON_GetObjectItemCaseSensitive(copy, "img");
    if (cJSON_IsObject(img))
    {
        cJSON_DeleteItemFromObjectCaseSensitive(img, "data");
        cJSON_DeleteItemFromObjectCaseSensitive(img, "mime");
    }
    return copy;
}

static bool known_id(const room *r, const char *id)
{
    const cJSON *m;
    cJSON_ArrayForEach(m, r->msgs)
    {
        if (strcmp(str_of(m, "id"), id) == 0)
        {
            return true;
        }
    }
    return false;
}

// Keeps the picture data only for the newest KEEP_IMAGES pictures.
static void trim_images(room *r)
{
    int seen = 0;
    for (int i = cJSON_GetArraySize(r->msgs) - 1; i >= 0; --i)
    {
        cJSON *m = cJSON_GetArrayItem(r->msgs, i);
        if (has_image_data(m) && ++seen > KEEP_IMAGES)
        {
            cJSON_ReplaceItemInArray(r->msgs, i, stripped(m));
        }
    }
}

// Adds a message to memory. Returns the stored copy, or NULL if invalid or already known.
static const cJSON *remember(room *r, const cJSON *m)
{
    if (room_valid_message(m) == false || known_id(r, str_of(m, "id")))
    {
        return NULL;
    }
    cJSON_AddItemToArray(r->msgs, cJSON_Duplicate(m, true));
    while (cJSON_GetArraySize(r->msgs) > MEMORY)
    {
        cJSON_DeleteItemFromArray(r->msgs, 0);
    }
    trim_images(r);
    return cJSON_GetArrayItem(r->msgs, cJSON_GetArraySize(r->msgs) - 1);
}

static int by_ts(const void *a, const void *b)
{
    double ta = num_of(*(cJSON *const *)a, "ts");
    double tb = num_of(*(cJSON *const *)b, "ts");
    return (ta > tb) - (ta < tb);
}

static void sort_by_ts(cJSON *array)
{
    int n = cJSON_GetArraySize(array);
    if (n < 2)
    {
        return;
    }
    cJSON **items = malloc((size_t)n * sizeof *items);
    if (items == NULL)
    {
        return;
    }
    for (int i = n - 1; i >= 0; --i)
    {
        items[i] = cJSON_DetachItemFromArray(array, i);
    }
    qsort(items, (size_t)n, sizeof *items, by_ts);
    for (int i = 0; i < n; ++i)
    {
        cJSON_AddItemToArray(array, items[i]);
    }
    free(items);
}

// A person (not a tab) appearing or leaving; our own key is not announced.
static void announce(const room *r, const char *user_id, bool joined)
{
    if (strcmp(user_id, r->user_id) == 0)
    {
        return;
    }
    void (*hook)(void *, const char *) = joined
        ? r->hooks.on_join
        : r->hooks.on_leave;
    if (hook != NULL)
    {
        hook(r->hooks.ui, user_id);
    }
}

static void presence_join(room *r, const cJSON *joins)
{
    const cJSON *entry;
    cJSON_ArrayForEach(entry, joins)
    {
        cJSON *slot = cJSON_GetObjectItemCaseSensitive(r->presence, entry->string);
        if (slot == NULL)
        {
            slot = cJSON_AddObjectToObject(r->presence, entry->string);
            cJSON_AddArrayToObject(slot, "metas");
            announce(r, entry->string, true);
        }
        cJSON *metas = cJSON_GetObjectItemCaseSensitive(slot, "metas");
        const cJSON *meta;
        cJSON_ArrayForEach(meta, cJSON_GetObjectItemCaseSensitive(entry, "metas"))
        {
            cJSON_AddItemToArray(metas, cJSON_Duplicate(meta, true));
        }
    }
}

static void presence_leave(room *r, const cJSON *leaves)
{
    const cJSON *entry;
    cJSON_ArrayForEach(entry, leaves)
    {
        cJSON *slot = cJSON_GetObjectItemCaseSensitive(r->presence, entry->string);
        if (slot == NULL)
        {
            continue;
        }
        cJSON *metas = cJSON_GetObjectItemCaseSensitive(slot, "metas");
        const cJSON *gone;
        cJSON_ArrayForEach(gone, cJSON_GetObjectItemCaseSensitive(entry, "metas"))
        {
            for (int i = cJSON_GetArraySize(metas) - 1; i >= 0; --i)
            {
                if (strcmp(str_of(cJSON_GetArrayItem(metas, i), "phx_ref"), str_of(gone, "phx_ref")) == 0)
                {
                    cJSON_DeleteItemFromArray(metas, i);
                }
            }
        }
        if (cJSON_GetArraySize(metas) == 0)
        {
            announce(r, entry->string, false);
            cJSON_DeleteItemFromObjectCaseSensitive(r->presence, entry->string);
        }
    }
}

static int by_seniority(const void *a, const void *b)
{
    const cJSON *ma = *(const cJSON *const *)a;
    const cJSON *mb = *(const cJSON *const *)b;
    double sa = num_of(ma, "since");
    double sb = num_of(mb, "since");
    if (sa != sb)
    {
        return (sa > sb) - (sa < sb);
    }
    return strcmp(str_of(ma, "client"), str_of(mb, "client"));
}

// The longest-present client other than `except`; everyone computes the same answer.
static const char *eldest_client(const room *r, const char *except)
{
    const cJSON *metas[256];
    size_t n = 0;
    const cJSON *slot;
    cJSON_ArrayForEach(slot, r->presence)
    {
        const cJSON *meta;
        cJSON_ArrayForEach(meta, cJSON_GetObjectItemCaseSensitive(slot, "metas"))
        {
            const char *client = str_of(meta, "client");
            if (client[0] != '\0' && strcmp(client, except) != 0 && n < sizeof metas / sizeof *metas)
            {
                metas[n++] = meta;
            }
        }
    }
    if (n == 0)
    {
        return NULL;
    }
    qsort(metas, n, sizeof *metas, by_seniority);
    return str_of(metas[0], "client");
}

static void maybe_request_history(room *r, long long now_ms)
{
    if (r->got_history || r->ask_until == 0 || now_ms > r->ask_until)
    {
        return;
    }
    if (eldest_client(r, r->client) == NULL)
    {
        return;
    }
    r->ask_until = 0;
    r->asked = true;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddNumberToObject(req, "v", PROTOCOL);
    cJSON_AddStringToObject(req, "from", r->client);
    r->hooks.send(r->hooks.send_ctx, "hist_req", req);
    cJSON_Delete(req);
}

static void on_hist_req(room *r, const cJSON *p)
{
    const cJSON *from = cJSON_GetObjectItemCaseSensitive(p, "from");
    if (num_of(p, "v") != PROTOCOL || is_short_string(from, 64) == false)
    {
        return;
    }
    const char *eldest = eldest_client(r, from->valuestring);
    if (eldest == NULL || strcmp(eldest, r->client) != 0)
    {
        return; // not my turn to answer
    }
    cJSON *msgs = cJSON_CreateArray();
    int n = cJSON_GetArraySize(r->msgs);
    int start = n > SHARE
        ? n - SHARE
        : 0;
    for (int i = start; i < n; ++i)
    {
        cJSON_AddItemToArray(msgs, stripped(cJSON_GetArrayItem(r->msgs, i)));
    }
    cJSON *answer = cJSON_CreateObject();
    cJSON_AddNumberToObject(answer, "v", PROTOCOL);
    cJSON_AddStringToObject(answer, "to", from->valuestring);
    cJSON_AddItemToObject(answer, "msgs", msgs);
    r->hooks.send(r->hooks.send_ctx, "hist", answer);
    cJSON_Delete(answer);
}

static void on_hist(room *r, const cJSON *p)
{
    const cJSON *msgs = cJSON_GetObjectItemCaseSensitive(p, "msgs");
    bool for_me = num_of(p, "v") == PROTOCOL && strcmp(str_of(p, "to"), r->client) == 0;
    if (for_me == false || r->got_history || cJSON_IsArray(msgs) == false)
    {
        return;
    }
    r->got_history = true;
    cJSON *added = cJSON_CreateArray();
    int n = cJSON_GetArraySize(msgs);
    int start = n > SHARE
        ? n - SHARE
        : 0;
    for (int i = start; i < n; ++i)
    {
        const cJSON *stored = remember(r, cJSON_GetArrayItem(msgs, i));
        if (stored != NULL)
        {
            cJSON_AddItemToArray(added, cJSON_Duplicate(stored, true));
        }
    }
    sort_by_ts(r->msgs);
    sort_by_ts(added);
    if (cJSON_GetArraySize(added) > 0)
    {
        r->hooks.on_history(r->hooks.ui, added);
    }
    cJSON_Delete(added);
}

static void on_reply(room *r, const cJSON *p)
{
    long ref = (long)num_of(p, "ref");
    bool ok = strcmp(str_of(p, "status"), "ok") == 0;
    for (int i = 0; i < MAX_PENDING; ++i)
    {
        if (r->pending[i].msg == NULL || r->pending[i].ref != ref)
        {
            continue;
        }
        if (ok == false)
        {
            r->hooks.on_failed(r->hooks.ui, r->pending[i].msg);
        }
        cJSON_Delete(r->pending[i].msg);
        r->pending[i].msg = NULL;
    }
}

static void add_pending(room *r, long ref, const cJSON *msg)
{
    int slot = 0;
    for (int i = 0; i < MAX_PENDING; ++i)
    {
        if (r->pending[i].msg == NULL)
        {
            slot = i;
            break;
        }
        if (r->pending[i].ref < r->pending[slot].ref)
        {
            slot = i; // all full: reuse the oldest
        }
    }
    cJSON_Delete(r->pending[slot].msg);
    r->pending[slot].ref = ref;
    r->pending[slot].msg = cJSON_Duplicate(msg, true);
}

room *room_create(const char *user_id, const char *nickname, room_hooks hooks)
{
    room *r = calloc(1, sizeof *r);
    if (r == NULL)
    {
        return NULL;
    }
    snprintf(r->user_id, sizeof r->user_id, "%s", user_id);
    snprintf(r->nickname, sizeof r->nickname, "%s", nickname);
    make_uuid(r->client);
    r->hooks = hooks;
    r->msgs = cJSON_CreateArray();
    r->presence = cJSON_CreateObject();
    return r;
}

void room_destroy(room *r)
{
    if (r == NULL)
    {
        return;
    }
    for (int i = 0; i < MAX_PENDING; ++i)
    {
        cJSON_Delete(r->pending[i].msg);
    }
    cJSON_Delete(r->msgs);
    cJSON_Delete(r->presence);
    free(r);
}

cJSON *room_presence_meta(const room *r)
{
    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "client", r->client);
    cJSON_AddStringToObject(meta, "nick", r->nickname);
    cJSON_AddNumberToObject(meta, "since", (double)r->since);
    return meta;
}

void room_joined(room *r, long long now_ms)
{
    if (r->since == 0)
    {
        r->since = now_ms;
    }
    r->ask_until = now_ms + HIST_WINDOW_MS;
    maybe_request_history(r, now_ms);
}

void room_on_event(room *r, const char *event, const cJSON *payload, long long now_ms)
{
    if (strcmp(event, "presence_state") == 0)
    {
        cJSON_Delete(r->presence);
        r->presence = cJSON_Duplicate(payload, true);
        r->presence_known = true;
    }
    else if (strcmp(event, "presence_diff") == 0)
    {
        presence_join(r, cJSON_GetObjectItemCaseSensitive(payload, "joins"));
        presence_leave(r, cJSON_GetObjectItemCaseSensitive(payload, "leaves"));
    }
    else if (strcmp(event, "reply") == 0)
    {
        on_reply(r, payload);
        return;
    }
    else if (strcmp(event, "broadcast") == 0)
    {
        const char *name = str_of(payload, "event");
        const cJSON *inner = cJSON_GetObjectItemCaseSensitive(payload, "payload");
        if (strcmp(name, "msg") == 0)
        {
            const cJSON *stored = remember(r, inner);
            if (stored != NULL)
            {
                r->hooks.on_message(r->hooks.ui, stored);
            }
        }
        else if (strcmp(name, "hist_req") == 0)
        {
            on_hist_req(r, inner);
        }
        else if (strcmp(name, "hist") == 0)
        {
            on_hist(r, inner);
        }
        return;
    }
    else
    {
        return;
    }
    maybe_request_history(r, now_ms);
    r->hooks.on_presence(r->hooks.ui, room_online(r));
}

bool room_send_text(room *r, const char *body, long long now_ms)
{
    return room_send(r, body, NULL, now_ms);
}

bool room_send(room *r, const char *body, const cJSON *img, long long now_ms)
{
    char id[37];
    make_uuid(id);
    cJSON *m = cJSON_CreateObject();
    cJSON_AddNumberToObject(m, "v", PROTOCOL);
    cJSON_AddStringToObject(m, "id", id);
    cJSON_AddStringToObject(m, "author", r->user_id);
    cJSON_AddStringToObject(m, "nick", r->nickname);
    cJSON_AddStringToObject(m, "body", body);
    cJSON_AddNumberToObject(m, "ts", (double)now_ms);
    if (img != NULL)
    {
        cJSON_AddItemToObject(m, "img", cJSON_Duplicate(img, true));
    }

    const cJSON *stored = remember(r, m);
    if (stored == NULL)
    {
        cJSON_Delete(m);
        return false;
    }
    r->hooks.on_message(r->hooks.ui, stored);
    long ref = r->hooks.send(r->hooks.send_ctx, "msg", m);
    if (ref == 0)
    {
        r->hooks.on_failed(r->hooks.ui, m);
    }
    else
    {
        add_pending(r, ref, m);
    }
    cJSON_Delete(m);
    return ref != 0;
}

int room_online(const room *r)
{
    return cJSON_GetArraySize(r->presence);
}

bool room_waiting_history(const room *r)
{
    if (r->got_history)
    {
        return false;
    }
    if (r->presence_known == false || r->asked)
    {
        return true;
    }
    return eldest_client(r, r->client) != NULL; // someone is here: the request is about to go out
}

const cJSON *room_presence(const room *r)
{
    return r->presence;
}
