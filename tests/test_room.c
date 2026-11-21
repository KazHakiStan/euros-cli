#include "keys.h"
#include "room.h"

#include <cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((cond) == false)                                                                                           \
        {                                                                                                              \
            ++failures;                                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                           \
        }                                                                                                              \
    } while (0)

// A fake transport: remembers the last thing each room sent.
typedef struct
{
    char event[32];
    cJSON *payload;
    int messages;
    int history_batches;
    int history_size;
    char first_history_body[64];
    int failed;
} probe;

static long fake_send(void *ctx, const char *event, const cJSON *payload)
{
    probe *p = ctx;
    snprintf(p->event, sizeof p->event, "%s", event);
    cJSON_Delete(p->payload);
    p->payload = cJSON_Duplicate(payload, true);
    return 1;
}

static void on_message(void *ui, const cJSON *msg)
{
    ++((probe *)ui)->messages;
}

static void on_history(void *ui, const cJSON *msgs)
{
    probe *p = ui;
    ++p->history_batches;
    p->history_size = cJSON_GetArraySize(msgs);
    const cJSON *first = cJSON_GetArrayItem(msgs, 0);
    snprintf(p->first_history_body, sizeof p->first_history_body, "%s",
             cJSON_GetObjectItemCaseSensitive(first, "body")->valuestring);
}

static void on_presence(void *ui, int online)
{
}

static void on_failed(void *ui, const cJSON *msg)
{
    ++((probe *)ui)->failed;
}

static room *make_room(const char *user, probe *p)
{
    room_hooks hooks =
    {
        .send = fake_send,
        .send_ctx = p,
        .on_message = on_message,
        .on_history = on_history,
        .on_presence = on_presence,
        .on_failed = on_failed,
        .ui = p,
    };
    return room_create(user, user, hooks);
}

static cJSON *message(const char *id, const char *body, double ts)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddNumberToObject(m, "v", 1);
    cJSON_AddStringToObject(m, "id", id);
    cJSON_AddStringToObject(m, "author", "u1");
    cJSON_AddStringToObject(m, "nick", "Silent_Raven");
    cJSON_AddStringToObject(m, "body", body);
    cJSON_AddNumberToObject(m, "ts", ts);
    return m;
}

static cJSON *broadcast(const char *event, const cJSON *inner)
{
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "event", event);
    cJSON_AddItemToObject(b, "payload", cJSON_Duplicate(inner, true));
    return b;
}

static void test_validation(void)
{
    cJSON *m = message("a1", "hi", 1795260000000);
    CHECK(room_valid_message(m));

    cJSON_ReplaceItemInObject(m, "v", cJSON_CreateNumber(2));
    CHECK(room_valid_message(m) == false);
    cJSON_ReplaceItemInObject(m, "v", cJSON_CreateNumber(1));

    cJSON_ReplaceItemInObject(m, "body", cJSON_CreateString("   "));
    CHECK(room_valid_message(m) == false);

    char *long_body = malloc(ROOM_MAX_BODY + 2);
    memset(long_body, 'x', ROOM_MAX_BODY + 1);
    long_body[ROOM_MAX_BODY + 1] = '\0';
    cJSON_ReplaceItemInObject(m, "body", cJSON_CreateString(long_body));
    CHECK(room_valid_message(m) == false);
    long_body[ROOM_MAX_BODY] = '\0';
    cJSON_ReplaceItemInObject(m, "body", cJSON_CreateString(long_body));
    CHECK(room_valid_message(m));
    free(long_body);

    cJSON_ReplaceItemInObject(m, "body", cJSON_CreateString(""));
    cJSON *img = cJSON_AddObjectToObject(m, "img");
    cJSON_AddNumberToObject(img, "w", 10);
    cJSON_AddNumberToObject(img, "h", 10);
    CHECK(room_valid_message(m)); // stub without data
    cJSON_AddStringToObject(img, "mime", "image/jpeg");
    cJSON_AddStringToObject(img, "data", "/9j/4AAQ");
    CHECK(room_valid_message(m));
    cJSON_ReplaceItemInObject(img, "mime", cJSON_CreateString("image/webp"));
    CHECK(room_valid_message(m) == false);
    cJSON_ReplaceItemInObject(img, "mime", cJSON_CreateString("image/jpeg"));
    cJSON_ReplaceItemInObject(img, "data", cJSON_CreateString("not base64!"));
    CHECK(room_valid_message(m) == false);
    cJSON_ReplaceItemInObject(img, "data", cJSON_CreateString("/9j/4AAQ"));
    cJSON_ReplaceItemInObject(img, "w", cJSON_CreateNumber(5000));
    CHECK(room_valid_message(m) == false);
    cJSON_Delete(m);

    CHECK(room_utf16_length("abc") == 3);
    CHECK(room_utf16_length("жж") == 2);
    CHECK(room_utf16_length("🔥") == 2);
}

static cJSON *presence_with(const char *key_a, const char *client_a, double since_a, const char *key_b,
                            const char *client_b, double since_b)
{
    cJSON *state = cJSON_CreateObject();
    const char *keys[] =
    {
        key_a,
        key_b,
    };
    const char *clients[] =
    {
        client_a,
        client_b,
    };
    double since[] =
    {
        since_a,
        since_b,
    };
    for (int i = 0; i < 2; ++i)
    {
        cJSON *slot = cJSON_AddObjectToObject(state, keys[i]);
        cJSON *metas = cJSON_AddArrayToObject(slot, "metas");
        cJSON *meta = cJSON_CreateObject();
        cJSON_AddStringToObject(meta, "client", clients[i]);
        cJSON_AddNumberToObject(meta, "since", since[i]);
        cJSON_AddStringToObject(meta, "phx_ref", clients[i]);
        cJSON_AddItemToArray(metas, meta);
    }
    return state;
}

static void test_room_memory(void)
{
    probe pa = { 0 };
    room *a = make_room("ua", &pa);
    room_joined(a, 1000);

    // A has some memory, including a picture.
    cJSON *m1 = message("m1", "first", 10);
    cJSON *m2 = message("m2", "second", 20);
    cJSON *pic = message("m3", "", 30);
    cJSON *img = cJSON_AddObjectToObject(pic, "img");
    cJSON_AddNumberToObject(img, "w", 4);
    cJSON_AddNumberToObject(img, "h", 3);
    cJSON_AddStringToObject(img, "mime", "image/jpeg");
    cJSON_AddStringToObject(img, "data", "/9j/4AAQ");
    cJSON *msgs[] =
    {
        m2,
        m1,
        pic,
        m1,
    };
    for (size_t i = 0; i < sizeof msgs / sizeof *msgs; ++i)
    {
        cJSON *b = broadcast("msg", msgs[i]);
        room_on_event(a, "broadcast", b, 2000);
        cJSON_Delete(b);
    }
    CHECK(pa.messages == 3); // the duplicate is ignored

    // B (client "cb") asks; A ("ca"? no: A's real client id) is the eldest in presence.
    cJSON *meta = room_presence_meta(a);
    const char *a_client = cJSON_GetObjectItemCaseSensitive(meta, "client")->valuestring;
    cJSON *state = presence_with("ua", a_client, 1, "ub", "cb", 5);
    room_on_event(a, "presence_state", state, 2000);
    CHECK(room_online(a) == 2);

    cJSON *req = cJSON_CreateObject();
    cJSON_AddNumberToObject(req, "v", 1);
    cJSON_AddStringToObject(req, "from", "cb");
    cJSON *b = broadcast("hist_req", req);
    room_on_event(a, "broadcast", b, 2000);
    cJSON_Delete(b);
    CHECK(strcmp(pa.event, "hist") == 0);
    const cJSON *shared = cJSON_GetObjectItemCaseSensitive(pa.payload, "msgs");
    CHECK(cJSON_GetArraySize(shared) == 3);
    const cJSON *shared_pic = cJSON_GetArrayItem(shared, 2);
    const cJSON *shared_img = cJSON_GetObjectItemCaseSensitive(shared_pic, "img");
    CHECK(cJSON_GetObjectItemCaseSensitive(shared_img, "data") == NULL); // pictures are never passed on
    CHECK(cJSON_GetObjectItemCaseSensitive(shared_img, "w") != NULL);

    // Someone younger than A must not answer.
    probe pc = { 0 };
    room *c = make_room("uc", &pc);
    cJSON *meta_c = room_presence_meta(c);
    const char *c_client = cJSON_GetObjectItemCaseSensitive(meta_c, "client")->valuestring;
    cJSON *state_c = presence_with("ua", a_client, 1, "uc", c_client, 3);
    room_on_event(c, "presence_state", state_c, 2000);
    b = broadcast("hist_req", req);
    room_on_event(c, "broadcast", b, 2000);
    cJSON_Delete(b);
    CHECK(strcmp(pc.event, "hist") != 0);

    // B receives A's answer: oldest first, once.
    probe pb = { 0 };
    room *rb = make_room("ub", &pb);
    cJSON *meta_b = room_presence_meta(rb);
    const char *b_client = cJSON_GetObjectItemCaseSensitive(meta_b, "client")->valuestring;
    cJSON_ReplaceItemInObject(pa.payload, "to", cJSON_CreateString(b_client));
    b = broadcast("hist", pa.payload);
    room_on_event(rb, "broadcast", b, 2000);
    room_on_event(rb, "broadcast", b, 2000);
    cJSON_Delete(b);
    CHECK(pb.history_batches == 1);
    CHECK(pb.history_size == 3);
    CHECK(strcmp(pb.first_history_body, "first") == 0);

    // Own message: shown at once, reported if the server rejects it.
    CHECK(room_send_text(rb, "hello", 3000));
    CHECK(pb.messages == 1);
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddNumberToObject(reply, "ref", 1);
    cJSON_AddStringToObject(reply, "status", "error");
    room_on_event(rb, "reply", reply, 3000);
    CHECK(pb.failed == 1);
    CHECK(room_send_text(rb, "   ", 3000) == false);

    cJSON_Delete(reply);
    cJSON_Delete(meta_b);
    cJSON_Delete(meta_c);
    cJSON_Delete(state_c);
    cJSON_Delete(req);
    cJSON_Delete(state);
    cJSON_Delete(meta);
    cJSON_Delete(m1);
    cJSON_Delete(m2);
    cJSON_Delete(pic);
    cJSON_Delete(pa.payload);
    cJSON_Delete(pb.payload);
    cJSON_Delete(pc.payload);
    room_destroy(a);
    room_destroy(rb);
    room_destroy(c);
}

int main(void)
{
    if (keys_init() == false)
    {
        fprintf(stderr, "libsodium init failed\n");
        return 2;
    }
    test_validation();
    test_room_memory();
    const char *verdict = failures > 0
        ? "FAILED"
        : "ok";
    printf("room protocol: %s\n", verdict);
    return failures > 0
        ? 1
        : 0;
}
