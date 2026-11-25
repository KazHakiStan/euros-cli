#include "notify.h"
#include "platform.h"

#include <cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_ITEMS 300
#define MAX_SEEN 400
#define MAX_TEXT 200 // bytes kept of a mention
#define RECENT_LINES 30

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

static bool is_read(const cJSON *item)
{
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "read"));
}

static void set_read(cJSON *item)
{
    cJSON_ReplaceItemInObjectCaseSensitive(item, "read", cJSON_CreateTrue());
}

// One line, no control characters (other people's words end up in the terminal's status bar),
// cut at a UTF-8 boundary.
static void clean(const char *in, char *out, size_t size)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p != '\0' && n + 1 < size; ++p)
    {
        if (*p == '\n' || *p == '\t' || *p == '\r')
        {
            if (n > 0 && out[n - 1] != ' ')
            {
                out[n++] = ' ';
            }
        }
        else if (*p >= 0x20 && *p != 0x7f)
        {
            out[n++] = (char)*p;
        }
    }
    // a multi-byte character cut short at the end goes away whole
    size_t start = n;
    while (start > 0 && ((unsigned char)out[start - 1] & 0xc0) == 0x80)
    {
        --start;
    }
    if (start > 0 && ((unsigned char)out[start - 1] & 0xc0) == 0xc0)
    {
        unsigned char lead = (unsigned char)out[start - 1];
        size_t want = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : 2;
        if (n - (start - 1) < want)
        {
            n = start - 1;
        }
    }
    out[n] = '\0';
}

cJSON *notify_doc_new(void)
{
    cJSON *doc = cJSON_CreateObject();
    cJSON_AddNumberToObject(doc, "v", 1);
    cJSON_AddNumberToObject(doc, "unread", 0);
    cJSON_AddNumberToObject(doc, "mentions", 0);
    cJSON_AddNumberToObject(doc, "next", 1);
    cJSON_AddArrayToObject(doc, "items");
    cJSON_AddArrayToObject(doc, "seen");
    return doc;
}

// A document from the file may be old or damaged: give it whatever is missing.
static void repair(cJSON *doc)
{
    static const char *const NUMBERS[] = { "v", "unread", "mentions", "next" };
    for (size_t i = 0; i < sizeof NUMBERS / sizeof *NUMBERS; ++i)
    {
        if (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(doc, NUMBERS[i])) == false)
        {
            cJSON_DeleteItemFromObjectCaseSensitive(doc, NUMBERS[i]);
            cJSON_AddNumberToObject(doc, NUMBERS[i], i == 0 || i == 3 ? 1 : 0);
        }
    }
    static const char *const ARRAYS[] = { "items", "seen" };
    for (size_t i = 0; i < sizeof ARRAYS / sizeof *ARRAYS; ++i)
    {
        if (cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(doc, ARRAYS[i])) == false)
        {
            cJSON_DeleteItemFromObjectCaseSensitive(doc, ARRAYS[i]);
            cJSON_AddArrayToObject(doc, ARRAYS[i]);
        }
    }
}

static bool seen_id(const cJSON *doc, const char *id)
{
    const cJSON *s;
    cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(doc, "seen"))
    {
        if (cJSON_IsString(s) && strcmp(s->valuestring, id) == 0)
        {
            return true;
        }
    }
    return false;
}

static void remember_id(cJSON *doc, const char *id)
{
    cJSON *seen = cJSON_GetObjectItemCaseSensitive(doc, "seen");
    cJSON_AddItemToArray(seen, cJSON_CreateString(id));
    while (cJSON_GetArraySize(seen) > MAX_SEEN)
    {
        cJSON_DeleteItemFromArray(seen, 0);
    }
}

// Too many: the oldest read ones go first, then the oldest of all.
static void trim(cJSON *doc)
{
    cJSON *items = cJSON_GetObjectItemCaseSensitive(doc, "items");
    for (int i = 0; i < cJSON_GetArraySize(items) && cJSON_GetArraySize(items) > MAX_ITEMS;)
    {
        if (is_read(cJSON_GetArrayItem(items, i)))
        {
            cJSON_DeleteItemFromArray(items, i);
        }
        else
        {
            ++i;
        }
    }
    while (cJSON_GetArraySize(items) > MAX_ITEMS)
    {
        cJSON_DeleteItemFromArray(items, 0);
    }
}

void notify_doc_recount(cJSON *doc)
{
    int unread = 0;
    int mentions = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(doc, "items"))
    {
        if (is_read(item) == false)
        {
            ++unread;
            mentions += strcmp(str_of(item, "kind"), "mention") == 0;
        }
    }
    cJSON_ReplaceItemInObjectCaseSensitive(doc, "unread", cJSON_CreateNumber(unread));
    cJSON_ReplaceItemInObjectCaseSensitive(doc, "mentions", cJSON_CreateNumber(mentions));
}

static void add_clean(cJSON *item, const char *key, const char *value, size_t max)
{
    if (value == NULL)
    {
        return;
    }
    char buf[MAX_TEXT + 1];
    clean(value, buf, max + 1 < sizeof buf ? max + 1 : sizeof buf);
    cJSON_AddStringToObject(item, key, buf);
}

bool notify_doc_add(cJSON *doc, const note *n, const char *pane)
{
    if (n->id != NULL)
    {
        if (seen_id(doc, n->id))
        {
            return false;
        }
        remember_id(doc, n->id);
    }
    double next = num_of(doc, "next");
    cJSON *item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "n", next);
    cJSON_AddNumberToObject(item, "ts", (double)n->ts);
    cJSON_AddStringToObject(item, "kind", n->kind);
    add_clean(item, "room", n->room, 64);
    add_clean(item, "topic", n->topic, 64);
    add_clean(item, "from", n->from, 48);
    add_clean(item, "text", n->text, MAX_TEXT);
    add_clean(item, "id", n->id, 64);
    if (pane != NULL && pane[0] != '\0')
    {
        add_clean(item, "pane", pane, 16);
    }
    cJSON_AddBoolToObject(item, "read", false);
    cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(doc, "items"), item);
    cJSON_ReplaceItemInObjectCaseSensitive(doc, "next", cJSON_CreateNumber(next + 1));
    trim(doc);
    notify_doc_recount(doc);
    return true;
}

void notify_doc_seen(cJSON *doc, const char *id)
{
    if (seen_id(doc, id) == false)
    {
        remember_id(doc, id);
    }
    cJSON *item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(doc, "items"))
    {
        if (strcmp(str_of(item, "id"), id) == 0)
        {
            set_read(item);
        }
    }
    notify_doc_recount(doc);
}

int notify_doc_mark_read(cJSON *doc, const char *topic)
{
    int marked = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(doc, "items"))
    {
        if (is_read(item) == false && (topic == NULL || strcmp(str_of(item, "topic"), topic) == 0))
        {
            set_read(item);
            ++marked;
        }
    }
    notify_doc_recount(doc);
    return marked;
}

int notify_doc_mark_item(cJSON *doc, double n)
{
    int marked = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(doc, "items"))
    {
        if (is_read(item) == false && num_of(item, "n") == n)
        {
            set_read(item);
            ++marked;
        }
    }
    notify_doc_recount(doc);
    return marked;
}

static void when(long long ts_ms, char *out, size_t size)
{
    time_t t = (time_t)(ts_ms / 1000);
    time_t now = time(NULL);
    struct tm then_tm = *localtime(&t);
    struct tm now_tm = *localtime(&now);
    bool today = then_tm.tm_year == now_tm.tm_year && then_tm.tm_yday == now_tm.tm_yday;
    strftime(out, size, today ? "%H:%M" : "%d.%m %H:%M", &then_tm);
}

typedef struct
{
    double ts;
    char text[400];
} line;

static int by_ts(const void *a, const void *b)
{
    double ta = ((const line *)a)->ts;
    double tb = ((const line *)b)->ts;
    return (ta > tb) - (ta < tb);
}

int notify_doc_lines(const cJSON *doc, bool only_unread, void (*emit)(void *ctx, const char *line), void *ctx)
{
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(doc, "items");
    int total = cJSON_GetArraySize(items);
    int first = 0;
    if (only_unread == false && total > RECENT_LINES)
    {
        first = total - RECENT_LINES;
    }
    line *lines = calloc((size_t)(total > 0 ? total : 1), sizeof *lines);
    if (lines == NULL)
    {
        return 0;
    }
    int count = 0;
    for (int i = first; i < total; ++i)
    {
        const cJSON *item = cJSON_GetArrayItem(items, i);
        if (only_unread && is_read(item))
        {
            continue;
        }
        const char *kind = str_of(item, "kind");
        double ts = num_of(item, "ts");
        char at[24];
        when((long long)ts, at, sizeof at);
        const char *from = str_of(item, "from")[0] != '\0'
            ? str_of(item, "from")
            : "someone";
        line *l = &lines[count];
        if (strcmp(kind, "message") == 0)
        {
            // one line per room, at its newest message: how many, and who wrote last
            const char *topic = str_of(item, "topic");
            bool newer_follows = false;
            int messages = 0;
            for (int j = first; j < total; ++j)
            {
                const cJSON *other = cJSON_GetArrayItem(items, j);
                bool same_room = (only_unread == false || is_read(other) == false)
                    && strcmp(str_of(other, "kind"), "message") == 0 && strcmp(str_of(other, "topic"), topic) == 0;
                messages += same_room;
                newer_follows = newer_follows || (same_room && j > i);
            }
            if (newer_follows)
            {
                continue;
            }
            const char *noun = messages == 1
                ? "message"
                : "messages";
            snprintf(l->text, sizeof l->text, "%s %s · %d new %s, last from %s", at, str_of(item, "room"), messages,
                     noun, from);
        }
        else if (strcmp(kind, "mention") == 0)
        {
            snprintf(l->text, sizeof l->text, "%s @ %s · %s: %s", at, str_of(item, "room"), from,
                     str_of(item, "text"));
        }
        else if (strcmp(kind, "letter") == 0)
        {
            snprintf(l->text, sizeof l->text, "%s ✉ a letter from %s · euros read", at, from);
        }
        else if (strcmp(kind, "receipt") == 0)
        {
            snprintf(l->text, sizeof l->text, "%s ✉ %s read your letter", at, from);
        }
        else
        {
            continue;
        }
        l->ts = ts;
        ++count;
    }
    qsort(lines, (size_t)count, sizeof *lines, by_ts);
    int emitted = 0;
    for (int i = 0; i < count; ++i)
    {
        emit(ctx, lines[i].text);
        ++emitted;
    }
    free(lines);
    return emitted;
}

// ---- the file ----

static bool paths(char *file, char *lock, size_t size)
{
    char dir[1024];
    if (platform_config_dir(dir, sizeof dir) == false)
    {
        return false;
    }
    snprintf(file, size, "%s/notifications.json", dir);
    snprintf(lock, size, "%s/notifications.lock", dir);
    return true;
}

static cJSON *load_from(const char *file)
{
    size_t len = 0;
    char *text = platform_read_file(file, &len);
    cJSON *doc = text != NULL
        ? cJSON_ParseWithLength(text, len)
        : NULL;
    free(text);
    if (cJSON_IsObject(doc) == false)
    {
        cJSON_Delete(doc);
        return notify_doc_new();
    }
    repair(doc);
    return doc;
}

cJSON *notify_load(void)
{
    char file[1100];
    char lock[1100];
    if (paths(file, lock, sizeof file) == false)
    {
        return notify_doc_new();
    }
    return load_from(file);
}

typedef enum
{
    OP_ADD,
    OP_SEEN,
    OP_READ,
    OP_READ_ITEM,
    OP_CLEAR,
} op_kind;

// Load, change, save: under the lock, so two chats never lose each other's notifications.
static bool update(op_kind op, const note *n, const char *arg)
{
    char file[1100];
    char lock_path[1100];
    if (paths(file, lock_path, sizeof file) == false)
    {
        return false;
    }
    intptr_t lock = platform_lock(lock_path);
    cJSON *doc = load_from(file);
    bool changed = true;
    switch (op)
    {
    case OP_ADD:
        changed = notify_doc_add(doc, n, getenv("WEZTERM_PANE"));
        break;
    case OP_SEEN:
        notify_doc_seen(doc, arg);
        break;
    case OP_READ:
        changed = notify_doc_mark_read(doc, arg) > 0;
        break;
    case OP_READ_ITEM:
        changed = notify_doc_mark_item(doc, strtod(arg, NULL)) > 0;
        break;
    case OP_CLEAR:
        cJSON_Delete(doc);
        doc = notify_doc_new();
        break;
    }
    bool ok = true;
    if (changed)
    {
        char *text = cJSON_PrintUnformatted(doc);
        ok = text != NULL && platform_write_private(file, text, strlen(text));
        cJSON_free(text);
    }
    cJSON_Delete(doc);
    platform_unlock(lock);
    return ok;
}

bool notify_add(const note *n)
{
    return update(OP_ADD, n, NULL);
}

bool notify_seen(const char *id)
{
    return update(OP_SEEN, NULL, id);
}

bool notify_mark_read(const char *topic)
{
    return update(OP_READ, NULL, topic);
}

bool notify_mark_item(double n)
{
    char arg[32];
    snprintf(arg, sizeof arg, "%.0f", n);
    return update(OP_READ_ITEM, NULL, arg);
}

bool notify_clear(void)
{
    return update(OP_CLEAR, NULL, NULL);
}

static void print_line(void *ctx, const char *text)
{
    (void)ctx;
    printf("  %s\n", text);
}

int notify_cli(int argc, char **argv)
{
    bool all = false;
    bool keep = false;
    for (int i = 0; i < argc; ++i)
    {
        if (strcmp(argv[i], "read") == 0 && i == 0 && argc <= 2)
        {
            // read: all; read room:<id>: that room; read 17: the item numbered 17
            const char *what = argc == 2 ? argv[1] : NULL;
            bool number = what != NULL && what[0] != '\0' && strspn(what, "0123456789") == strlen(what);
            bool ok = number ? notify_mark_item(strtod(what, NULL)) : notify_mark_read(what);
            return ok ? 0 : 1;
        }
        if (strcmp(argv[i], "clear") == 0 && argc == 1)
        {
            return notify_clear() ? 0 : 1;
        }
        if (strcmp(argv[i], "--all") == 0)
        {
            all = true;
        }
        else if (strcmp(argv[i], "--keep") == 0)
        {
            keep = true;
        }
        else
        {
            fprintf(stderr, "usage: euros notifs [--all] [--keep] | read [<room topic> | <n>] | clear\n");
            return 2;
        }
    }
    cJSON *doc = notify_load();
    if (notify_doc_lines(doc, all == false, print_line, NULL) == 0)
    {
        printf("  nothing new\n");
    }
    cJSON_Delete(doc);
    if (keep == false)
    {
        notify_mark_read(NULL);
    }
    return 0;
}
