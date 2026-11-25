#include "notify.h"

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

static int count_of(const cJSON *doc, const char *key)
{
    return (int)cJSON_GetObjectItemCaseSensitive(doc, key)->valuedouble;
}

typedef struct
{
    int n;
    char lines[16][400];
} collected;

static void collect(void *ctx, const char *line)
{
    collected *c = ctx;
    if (c->n < 16)
    {
        snprintf(c->lines[c->n++], sizeof c->lines[0], "%s", line);
    }
}

static note message(const char *id, const char *topic, const char *room, const char *from, long long ts)
{
    note n = { .kind = "message", .id = id, .topic = topic, .room = room, .from = from, .ts = ts };
    return n;
}

static void test_add_and_dedupe(void)
{
    cJSON *doc = notify_doc_new();
    note a = message("m1", "room:main", "#everyone", "Ann", 1000);
    CHECK(notify_doc_add(doc, &a, "7"));
    CHECK(notify_doc_add(doc, &a, "7") == false); // a second euros saw the same message
    CHECK(count_of(doc, "unread") == 1);
    CHECK(count_of(doc, "mentions") == 0);
    const cJSON *item = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(doc, "items"), 0);
    CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(item, "pane")->valuestring, "7") == 0);
    CHECK(cJSON_GetObjectItemCaseSensitive(item, "text") == NULL); // plain messages keep no words

    note m = { .kind = "mention", .id = "m2", .topic = "room:g1", .room = "devs", .from = "Bob",
               .text = "hey @me\nlook\033[31m", .ts = 2000 };
    CHECK(notify_doc_add(doc, &m, NULL));
    CHECK(count_of(doc, "unread") == 2);
    CHECK(count_of(doc, "mentions") == 1);
    item = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(doc, "items"), 1);
    CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(item, "text")->valuestring, "hey @me look[31m") == 0);
    cJSON_Delete(doc);
}

static void test_seen_and_read(void)
{
    cJSON *doc = notify_doc_new();
    notify_doc_seen(doc, "m1"); // looked at in one window first...
    note a = message("m1", "room:main", "#everyone", "Ann", 1000);
    CHECK(notify_doc_add(doc, &a, NULL) == false); // ...so another window doesn't count it
    CHECK(count_of(doc, "unread") == 0);

    note b = message("m2", "room:main", "#everyone", "Ann", 1100);
    note c = message("m3", "room:g1", "devs", "Bob", 1200);
    notify_doc_add(doc, &b, NULL);
    notify_doc_add(doc, &c, NULL);
    notify_doc_seen(doc, "m2"); // seen after it was added: read
    CHECK(count_of(doc, "unread") == 1);
    CHECK(notify_doc_mark_read(doc, "room:main") == 0);
    CHECK(notify_doc_mark_read(doc, "room:g1") == 1);
    CHECK(count_of(doc, "unread") == 0);

    note l = { .kind = "letter", .id = "L1", .from = "Cid", .ts = 1300 };
    note l2 = { .kind = "letter", .id = "L2", .from = "Cid", .ts = 1400 };
    notify_doc_add(doc, &l, NULL);
    notify_doc_add(doc, &l2, NULL);
    double n = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(doc, "items"), 3),
                                                "n")->valuedouble;
    CHECK(notify_doc_mark_item(doc, n) == 1);
    CHECK(notify_doc_mark_item(doc, n) == 0);
    CHECK(notify_doc_mark_read(doc, NULL) == 1);
    cJSON_Delete(doc);
}

static void test_lines(void)
{
    cJSON *doc = notify_doc_new();
    note a = message("m1", "room:main", "#everyone", "Ann", 1000);
    note b = message("m2", "room:main", "#everyone", "Bob", 3000);
    note c = message("m3", "room:g1", "devs", "Cid", 2000);
    note m = { .kind = "mention", .id = "m4", .topic = "room:g1", .room = "devs", .from = "Dan", .text = "@me hi",
               .ts = 4000 };
    note l = { .kind = "letter", .id = "L1", .from = "Eve", .ts = 500 };
    notify_doc_add(doc, &a, NULL);
    notify_doc_add(doc, &b, NULL);
    notify_doc_add(doc, &c, NULL);
    notify_doc_add(doc, &m, NULL);
    notify_doc_add(doc, &l, NULL);

    collected got = { 0 };
    CHECK(notify_doc_lines(doc, true, collect, &got) == 4);
    // oldest first: the letter, devs (one message), #everyone (two, last from Bob), the mention
    CHECK(strstr(got.lines[0], "a letter from Eve") != NULL);
    CHECK(strstr(got.lines[1], "devs · 1 new message, last from Cid") != NULL);
    CHECK(strstr(got.lines[2], "#everyone · 2 new messages, last from Bob") != NULL);
    CHECK(strstr(got.lines[3], "@ devs · Dan: @me hi") != NULL);

    notify_doc_mark_read(doc, "room:main");
    collected after = { 0 };
    CHECK(notify_doc_lines(doc, true, collect, &after) == 3);
    collected all = { 0 };
    CHECK(notify_doc_lines(doc, false, collect, &all) == 4); // read ones too
    cJSON_Delete(doc);
}

static void test_trim_and_utf8(void)
{
    cJSON *doc = notify_doc_new();
    for (int i = 0; i < 350; ++i)
    {
        char id[16];
        snprintf(id, sizeof id, "x%d", i);
        note n = message(id, "room:main", "#everyone", "Ann", i);
        notify_doc_add(doc, &n, NULL);
    }
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(doc, "items")) == 300);
    CHECK(count_of(doc, "unread") == 300);

    // 199 ASCII bytes + a 2-byte letter: it doesn't fit whole into 200, so it goes
    char text[256];
    memset(text, 'a', 199);
    strcpy(text + 199, "я");
    note m = { .kind = "mention", .id = "u", .topic = "room:main", .room = "#everyone", .from = "Bo", .text = text,
               .ts = 1 };
    notify_doc_add(doc, &m, NULL);
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(doc, "items");
    const cJSON *last = cJSON_GetArrayItem(items, cJSON_GetArraySize(items) - 1);
    CHECK(strlen(cJSON_GetObjectItemCaseSensitive(last, "text")->valuestring) == 199);
    cJSON_Delete(doc);
}

int main(void)
{
    test_add_and_dedupe();
    test_seen_and_read();
    test_lines();
    test_trim_and_utf8();
    if (failures > 0)
    {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("notify: ok\n");
    return 0;
}
