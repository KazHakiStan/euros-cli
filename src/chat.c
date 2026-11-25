#include "api.h"
#include "chat.h"
#include "forum.h"
#include "input.h"
#include "letters.h"
#include "lineedit.h"
#include "notify.h"
#include "picture.h"
#include "platform.h"
#include "realtime.h"
#include "room.h"
#include "session.h"
#include "term.h"

#include <cJSON.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TOKEN_CHECK_MS 30000
#define RECONNECT_MS 3000
#define HISTORY_WAIT_MS 2500
#define PRESENCE_SETTLE_MS 1200 // the first presence snapshot can miss people who show up in a diff right after
#define QUIET_AFTER_JOIN_MS 5000 // late presence diffs for people already here must not read as "joined"
#define HEADER_LABEL_WIDTH 26
#define PICTURE_COLS 48
#define PICTURE_ROWS 12
#define RECENT_MAX 200 // messages kept to fill the full screen when it opens
#define MAX_WATCHED 32  // other rooms listened to for notifications

typedef struct chat chat;

// Another room of the user: listened to without presence (nobody sees us there, we don't answer
// for its memory), only to notice messages and mentions.
typedef struct
{
    chat *chat;
    rt_channel *ch;
    bool closed;
    char topic[64];
    char name[64];
} watched_room;

struct chat
{
    euros_session session;
    rt_socket *sock; // one WebSocket for every channel below
    rt_channel *ch;
    bool ch_closed;    // the server dropped the room channel: join again
    rt_channel *inbox; // inbox:<me>: new letters and read receipts
    bool inbox_closed;
    watched_room watched[MAX_WATCHED];
    int watched_count;
    int focus; // the last focus state seen (le_focus)
    room *room;
    cJSON *nicks; // user id → nickname
    char topic[64];
    char room_name[64];
    bool color;
    bool keyboard; // raw keyboard with a pinned input line
    picture_protocol pictures;
    bool in_header;
    cJSON *held; // messages that arrived while the header was being printed
    long long quiet_until; // no join/leave lines before this moment
    cJSON *recent; // the last RECENT_MAX messages printed
    long long printed; // messages printed so far
    long long fs_from; // `printed` when the full screen opened
    bool replaying;
};

static bool same_text_ci(const char *a, const char *b)
{
    for (; *a != '\0' && *b != '\0'; ++a, ++b)
    {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
        {
            return false;
        }
    }
    return *a == *b;
}

static const char *sty(const chat *c, const char *code)
{
    return c->color
        ? code
        : "";
}

static void color_on(const chat *c, const char *rgb, bool bold)
{
    if (c->color)
    {
        const char *weight = bold
            ? "1;"
            : "";
        printf("\033[%s38;2;%sm", weight, rgb);
    }
}

static void out_begin(void)
{
    le_hide();
}

static void out_end(void)
{
    fflush(stdout);
    le_show();
}

static void load_nicks(chat *c)
{
    cJSON *rows = api_select(&c->session, "profiles?select=id,nickname", NULL);
    if (cJSON_IsArray(rows) == false)
    {
        cJSON_Delete(rows);
        return;
    }
    cJSON_Delete(c->nicks);
    c->nicks = cJSON_CreateObject();
    const cJSON *row;
    cJSON_ArrayForEach(row, rows)
    {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(row, "id");
        const cJSON *nick = cJSON_GetObjectItemCaseSensitive(row, "nickname");
        if (cJSON_IsString(id) && cJSON_IsString(nick))
        {
            cJSON_AddStringToObject(c->nicks, id->valuestring, nick->valuestring);
        }
    }
    cJSON_Delete(rows);
}

static const char *nick_of(chat *c, const char *user_id)
{
    const cJSON *nick = cJSON_GetObjectItemCaseSensitive(c->nicks, user_id);
    if (cJSON_IsString(nick) == false)
    {
        load_nicks(c); // someone may have joined since we came in
        nick = cJSON_GetObjectItemCaseSensitive(c->nicks, user_id);
    }
    return cJSON_IsString(nick)
        ? nick->valuestring
        : NULL;
}

static const char *nick_or_unknown(chat *c, const char *user_id)
{
    const char *nick = nick_of(c, user_id);
    return nick != NULL
        ? nick
        : "???";
}

#define PAINTED_SIZE 96

// A nickname in its own color, for system lines; control characters dropped.
static const char *paint(const chat *c, const char *nick, char out[PAINTED_SIZE])
{
    char clean[48];
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)nick; *p != '\0' && n + 1 < sizeof clean; ++p)
    {
        if (*p >= 0x20 && *p != 0x7f)
        {
            clean[n++] = (char)*p;
        }
    }
    clean[n] = '\0';
    if (c->color)
    {
        snprintf(out, PAINTED_SIZE, "\033[1;38;2;%sm%s\033[0m", term_nick_color(clean), clean);
    }
    else
    {
        snprintf(out, PAINTED_SIZE, "%s", clean);
    }
    return out;
}

static const char *painted_nick(chat *c, const char *user_id, char out[PAINTED_SIZE])
{
    return paint(c, nick_or_unknown(c, user_id), out);
}

// Newlines in other people's text continue on an indented line.
static void put_clean(const char *s)
{
    term_put_clean(s, "\n      ");
}

static void put_time(const chat *c, long long ts_ms)
{
    time_t t = (time_t)(ts_ms / 1000);
    char when[8];
    strftime(when, sizeof when, "%H:%M", localtime(&t));
    printf("%s%s%s ", sty(c, "\033[2m"), when, sty(c, "\033[0m"));
}

// "HH:MM -!- text": joins, leaves, connection state.
static void system_line(chat *c, const char *fmt, ...)
{
    out_begin();
    put_time(c, platform_now_ms());
    printf("%s-!-%s ", sty(c, "\033[36m"), sty(c, "\033[0m"));
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    putchar('\n');
    out_end();
}

static bool mentions_me(const chat *c, const char *body)
{
    size_t len = strlen(c->session.nickname);
    for (const char *at = strchr(body, '@'); at != NULL; at = strchr(at + 1, '@'))
    {
        bool name_matches = true;
        for (size_t i = 0; i < len; ++i)
        {
            if (tolower((unsigned char)at[1 + i]) != tolower((unsigned char)c->session.nickname[i]))
            {
                name_matches = false;
                break;
            }
        }
        if (name_matches)
        {
            return true;
        }
    }
    return false;
}

// Someone else's message becomes a notification, unless the user is looking at it: then it is only
// marked seen, so another euros running elsewhere doesn't count it either.
static void note_message(chat *c, const char *topic, const char *room_name, const cJSON *m, bool looking)
{
    const char *author = cJSON_GetObjectItemCaseSensitive(m, "author")->valuestring;
    const char *id = cJSON_GetObjectItemCaseSensitive(m, "id")->valuestring;
    if (strcmp(author, c->session.user_id) == 0)
    {
        return;
    }
    if (looking)
    {
        notify_seen(id);
        return;
    }
    const char *body = cJSON_GetObjectItemCaseSensitive(m, "body")->valuestring;
    const char *known = nick_of(c, author);
    char nick[48];
    snprintf(nick, sizeof nick, known != NULL ? "%s" : "%s?",
             known != NULL ? known : cJSON_GetObjectItemCaseSensitive(m, "nick")->valuestring);
    bool mention = mentions_me(c, body);
    note n = {
        .kind = mention ? "mention" : "message",
        .id = id,
        .topic = topic,
        .room = room_name,
        .from = nick,
        .text = mention ? body : NULL,
        .ts = platform_now_ms(),
    };
    notify_add(&n);
}

static void print_picture(const chat *c, const cJSON *img)
{
    int w = (int)cJSON_GetObjectItemCaseSensitive(img, "w")->valuedouble;
    int h = (int)cJSON_GetObjectItemCaseSensitive(img, "h")->valuedouble;
    printf("%s", sty(c, "\033[2m"));
    if (cJSON_IsString(cJSON_GetObjectItemCaseSensitive(img, "data")))
    {
        printf("[picture %dx%d]", w, h);
    }
    else
    {
        printf("[picture: seen only by those who were here]");
    }
    printf("%s", sty(c, "\033[0m"));
}

// "HH:MM <Nick> text", or "HH:MM  * Nick: [picture]" for pictures.
static void print_message(chat *c, const cJSON *m)
{
    const char *author = cJSON_GetObjectItemCaseSensitive(m, "author")->valuestring;
    const char *body = cJSON_GetObjectItemCaseSensitive(m, "body")->valuestring;
    const char *claimed = cJSON_GetObjectItemCaseSensitive(m, "nick")->valuestring;
    const cJSON *img = cJSON_GetObjectItemCaseSensitive(m, "img");
    long long ts = (long long)cJSON_GetObjectItemCaseSensitive(m, "ts")->valuedouble;

    const char *known = nick_of(c, author);
    char nick[48];
    if (known != NULL)
    {
        snprintf(nick, sizeof nick, "%s", known);
    }
    else
    {
        snprintf(nick, sizeof nick, "%s?", claimed); // not a known member
    }
    bool mention = strcmp(author, c->session.user_id) != 0 && mentions_me(c, body);
    bool picture = cJSON_IsObject(img);

    out_begin();
    put_time(c, ts);
    if (picture)
    {
        printf(" * ");
    }
    else
    {
        printf("%s<%s", sty(c, "\033[2m"), sty(c, "\033[0m"));
    }
    color_on(c, term_nick_color(nick), true);
    put_clean(nick);
    printf("%s", sty(c, "\033[0m"));
    if (picture)
    {
        printf(": ");
        print_picture(c, img);
        if (body[0] != '\0')
        {
            putchar(' ');
        }
    }
    else
    {
        printf("%s>%s ", sty(c, "\033[2m"), sty(c, "\033[0m"));
    }
    if (mention)
    {
        printf("%s", sty(c, "\033[1;33m"));
    }
    put_clean(body);
    printf("%s\n", sty(c, "\033[0m"));
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(img, "data");
    if (picture && cJSON_IsString(data) && c->pictures != PICTURES_OFF)
    {
        printf("      ");
        if (picture_show(c->pictures, data->valuestring, PICTURE_COLS, PICTURE_ROWS) == false)
        {
            putchar('\r');
        }
    }
    out_end();
    if (c->replaying == false)
    {
        cJSON_AddItemToArray(c->recent, cJSON_Duplicate(m, true));
        if (cJSON_GetArraySize(c->recent) > RECENT_MAX)
        {
            cJSON_DeleteItemFromArray(c->recent, 0);
        }
        ++c->printed;
    }
}

// Prints the last `count` remembered messages again.
static void replay(chat *c, long long count)
{
    int size = cJSON_GetArraySize(c->recent);
    int from = count < size
        ? size - (int)count
        : 0;
    c->replaying = true;
    for (int i = from; i < size; ++i)
    {
        print_message(c, cJSON_GetArrayItem(c->recent, i));
    }
    c->replaying = false;
}

// The full screen starts with the recent messages; closing it prints what came meanwhile, so
// the normal scrollback misses nothing.
static void toggle_fullscreen(chat *c)
{
    if (c->keyboard == false)
    {
        system_line(c, "full screen needs a terminal keyboard");
        return;
    }
    if (le_fullscreen())
    {
        le_set_fullscreen(false, NULL);
        replay(c, c->printed - c->fs_from);
        return;
    }
    char title[96];
    snprintf(title, sizeof title, "euros · %s", c->room_name);
    le_set_fullscreen(true, title);
    c->fs_from = c->printed;
    replay(c, RECENT_MAX);
    system_line(c, "full screen · /fs or :fs to leave");
}

static void update_prompt(chat *c)
{
    char prompt[128];
    snprintf(prompt, sizeof prompt, "[%s · %d] > ", c->room_name, room_online(c->room));
    le_set_prompt(prompt);
}

static long hook_send(void *ctx, const char *event, const cJSON *payload)
{
    chat *c = ctx;
    return c->ch != NULL
        ? rt_broadcast(c->ch, event, payload)
        : 0;
}

static void hook_message(void *ui, const cJSON *msg)
{
    chat *c = ui;
    if (c->in_header)
    {
        cJSON_AddItemToArray(c->held, cJSON_Duplicate(msg, true));
        return;
    }
    print_message(c, msg);
    note_message(c, c->topic, c->room_name, msg, c->focus != 0);
}

static void hook_history(void *ui, const cJSON *msgs)
{
    chat *c = ui;
    const cJSON *m;
    if (c->in_header)
    {
        cJSON_ArrayForEach(m, msgs)
        {
            cJSON_AddItemToArray(c->held, cJSON_Duplicate(m, true));
        }
        return;
    }
    system_line(c, "the room remembers:");
    cJSON_ArrayForEach(m, msgs)
    {
        print_message(c, m);
    }
}

static void hook_presence(void *ui, int online)
{
    update_prompt(ui);
}

static bool quiet(const chat *c)
{
    return c->in_header || platform_now_ms() < c->quiet_until;
}

static void hook_join(void *ui, const char *user_id)
{
    chat *c = ui;
    if (quiet(c) == false)
    {
        char nick[PAINTED_SIZE];
        system_line(c, "%s joined %s", painted_nick(c, user_id, nick), c->room_name);
    }
}

static void hook_leave(void *ui, const char *user_id)
{
    chat *c = ui;
    if (quiet(c) == false)
    {
        char nick[PAINTED_SIZE];
        system_line(c, "%s left", painted_nick(c, user_id, nick));
    }
}

static void hook_failed(void *ui, const cJSON *msg)
{
    chat *c = ui;
    out_begin();
    put_time(c, platform_now_ms());
    printf("%s-!- not sent:%s ", sty(c, "\033[31m"), sty(c, "\033[0m"));
    put_clean(cJSON_GetObjectItemCaseSensitive(msg, "body")->valuestring);
    putchar('\n');
    out_end();
}

static void on_channel(void *ctx, const char *event, const cJSON *payload)
{
    chat *c = ctx;
    if (strcmp(event, "closed") == 0)
    {
        c->ch_closed = true;
        return;
    }
    room_on_event(c->room, event, payload, platform_now_ms());
}

static bool connect_room(chat *c, char *err, size_t err_size)
{
    c->ch_closed = false;
    c->ch = rt_join(c->sock, c->topic, c->session.user_id, NULL, c->session.access_token, on_channel, c, err,
                    err_size);
    if (c->ch == NULL)
    {
        return false;
    }
    cJSON *meta = room_presence_meta(c->room);
    rt_track(c->ch, meta);
    cJSON_Delete(meta);
    room_joined(c->room, platform_now_ms());
    c->quiet_until = platform_now_ms() + QUIET_AFTER_JOIN_MS;
    return true;
}

static void on_inbox(void *ctx, const char *event, const cJSON *payload)
{
    chat *c = ctx;
    if (strcmp(event, "closed") == 0)
    {
        c->inbox_closed = true;
        return;
    }
    if (strcmp(event, "postgres_changes") == 0)
    {
        const cJSON *data = cJSON_GetObjectItemCaseSensitive(payload, "data");
        const cJSON *record = cJSON_GetObjectItemCaseSensitive(data, "record");
        const cJSON *from = cJSON_GetObjectItemCaseSensitive(record, "from_id");
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(record, "id");
        char painted[PAINTED_SIZE];
        const char *nick = cJSON_IsString(from)
            ? painted_nick(c, from->valuestring, painted)
            : "someone";
        system_line(c, "✉ a letter from %s · /letters", nick);
        // a letter waits to be read wherever the user looks: it stays until `euros read` burns it
        note n = {
            .kind = "letter",
            .id = cJSON_IsString(id) ? id->valuestring : NULL,
            .from = cJSON_IsString(from) ? nick_or_unknown(c, from->valuestring) : NULL,
            .ts = platform_now_ms(),
        };
        notify_add(&n);
        return;
    }
    if (strcmp(event, "broadcast") != 0)
    {
        return;
    }
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(payload, "event");
    const cJSON *body = cJSON_GetObjectItemCaseSensitive(payload, "payload");
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(body, "v");
    const cJSON *by = cJSON_GetObjectItemCaseSensitive(body, "by");
    bool receipt = cJSON_IsString(name) && strcmp(name->valuestring, "letter_read") == 0;
    if (receipt && cJSON_IsNumber(v) && v->valuedouble == 1 && cJSON_IsString(by))
    {
        const char *nick = nick_of(c, by->valuestring);
        if (nick != NULL)
        {
            char painted[PAINTED_SIZE];
            system_line(c, "✉ %s read your letter", paint(c, nick, painted));
            const cJSON *letter = cJSON_GetObjectItemCaseSensitive(body, "id");
            char id[96];
            snprintf(id, sizeof id, "read:%s", cJSON_IsString(letter) ? letter->valuestring : by->valuestring);
            if (c->focus != 0)
            {
                notify_seen(id);
            }
            else
            {
                note n = { .kind = "receipt", .id = id, .from = nick, .ts = platform_now_ms() };
                notify_add(&n);
            }
        }
    }
}

static bool connect_inbox(chat *c, char *err, size_t err_size)
{
    char topic[64];
    snprintf(topic, sizeof topic, "inbox:%s", c->session.user_id);
    cJSON *changes = letters_changes_binding(c->session.user_id);
    c->inbox_closed = false;
    c->inbox = rt_join(c->sock, topic, NULL, changes, c->session.access_token, on_inbox, c, err, err_size);
    cJSON_Delete(changes);
    return c->inbox != NULL;
}

static void on_watched(void *ctx, const char *event, const cJSON *payload)
{
    watched_room *w = ctx;
    if (strcmp(event, "closed") == 0)
    {
        w->closed = true;
        return;
    }
    if (strcmp(event, "broadcast") != 0)
    {
        return; // presence is off; history requests are for those who are really there
    }
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(payload, "event");
    const cJSON *msg = cJSON_GetObjectItemCaseSensitive(payload, "payload");
    if (cJSON_IsString(name) && strcmp(name->valuestring, "msg") == 0 && room_valid_message(msg))
    {
        note_message(w->chat, w->topic, w->name, msg, false);
    }
}

// #everyone and every group of the user, except the room we are in.
static void load_watched(chat *c)
{
    c->watched_count = 0;
    cJSON *rows = api_select(&c->session, "groups?select=id,name&order=created_at", NULL);
    cJSON *all = cJSON_CreateArray();
    cJSON *main_room = cJSON_CreateObject();
    cJSON_AddStringToObject(main_room, "topic", "room:main");
    cJSON_AddStringToObject(main_room, "name", "#everyone");
    cJSON_AddItemToArray(all, main_room);
    const cJSON *row;
    cJSON_ArrayForEach(row, rows)
    {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(row, "id");
        const cJSON *group = cJSON_GetObjectItemCaseSensitive(row, "name");
        if (cJSON_IsString(id) && cJSON_IsString(group))
        {
            char topic[64];
            snprintf(topic, sizeof topic, "room:%s", id->valuestring);
            cJSON *entry = cJSON_CreateObject();
            cJSON_AddStringToObject(entry, "topic", topic);
            cJSON_AddStringToObject(entry, "name", group->valuestring);
            cJSON_AddItemToArray(all, entry);
        }
    }
    cJSON_Delete(rows);
    const cJSON *entry;
    cJSON_ArrayForEach(entry, all)
    {
        const char *topic = cJSON_GetObjectItemCaseSensitive(entry, "topic")->valuestring;
        if (strcmp(topic, c->topic) == 0 || c->watched_count == MAX_WATCHED)
        {
            continue;
        }
        watched_room *w = &c->watched[c->watched_count++];
        memset(w, 0, sizeof *w);
        w->chat = c;
        snprintf(w->topic, sizeof w->topic, "%s", topic);
        snprintf(w->name, sizeof w->name, "%s", cJSON_GetObjectItemCaseSensitive(entry, "name")->valuestring);
    }
    cJSON_Delete(all);
}

// Joins the watched rooms not joined yet. Returns how many are listened to now.
static int connect_watched(chat *c)
{
    int listening = 0;
    for (int i = 0; i < c->watched_count; ++i)
    {
        watched_room *w = &c->watched[i];
        if (w->ch == NULL)
        {
            char err[256];
            w->closed = false;
            w->ch = rt_join(c->sock, w->topic, NULL, NULL, c->session.access_token, on_watched, w, err, sizeof err);
        }
        listening += w->ch != NULL;
    }
    return listening;
}

// Channels the server closed are left, to be joined again.
static void drop_closed(chat *c)
{
    if (c->ch_closed)
    {
        rt_leave(c->ch);
        c->ch = NULL;
        c->ch_closed = false;
    }
    if (c->inbox_closed)
    {
        rt_leave(c->inbox);
        c->inbox = NULL;
        c->inbox_closed = false;
    }
    for (int i = 0; i < c->watched_count; ++i)
    {
        if (c->watched[i].closed)
        {
            rt_leave(c->watched[i].ch);
            c->watched[i].ch = NULL;
            c->watched[i].closed = false;
        }
    }
}

// The socket is gone: so is every channel on it.
static void disconnect(chat *c)
{
    rt_close(c->sock);
    c->sock = NULL;
    c->ch = NULL;
    c->inbox = NULL;
    c->ch_closed = false;
    c->inbox_closed = false;
    for (int i = 0; i < c->watched_count; ++i)
    {
        c->watched[i].ch = NULL;
        c->watched[i].closed = false;
    }
}

// "Nick, Nick, Nick" for everyone in the room.
static void who_is_here(chat *c, char *out, size_t size)
{
    out[0] = '\0';
    const cJSON *slot;
    cJSON_ArrayForEach(slot, room_presence(c->room))
    {
        size_t used = strlen(out);
        if (used > 0)
        {
            snprintf(out + used, size - used, ", ");
            used = strlen(out);
        }
        char nick[PAINTED_SIZE];
        snprintf(out + used, size - used, "%s", painted_nick(c, slot->string, nick));
    }
}

// "  > label ........ status"
static void header_step(const chat *c, const char *label, const char *status, bool ok)
{
    printf("  %s>%s %s ", sty(c, "\033[2m"), sty(c, "\033[0m"), label);
    for (int i = (int)strlen(label); i < HEADER_LABEL_WIDTH; ++i)
    {
        putchar('.');
    }
    const char *tone = ok
        ? "\033[32m"
        : "\033[31m";
    printf(" %s%s%s\n", sty(c, tone), status, sty(c, "\033[0m"));
    fflush(stdout);
}

static void print_logo(const chat *c)
{
    static const char LETTERS[] = "EUROS";
    printf("\n  %s▼%s ", sty(c, "\033[1m"), sty(c, "\033[0m"));
    for (int i = 0; LETTERS[i] != '\0'; ++i)
    {
        color_on(c, term_palette(i), true);
        printf("%c%s ", LETTERS[i], sty(c, "\033[0m"));
    }
    printf("\n");
}

// Logo, connection steps, room memory. False if the room couldn't be entered.
static bool enter_with_header(chat *c)
{
    print_logo(c);
    char err[256] = "";
    char failure[300];
    c->sock = rt_connect(err, sizeof err);
    if (c->sock == NULL)
    {
        snprintf(failure, sizeof failure, "fail (%s)", err);
        header_step(c, "connecting relay", failure, false);
        return false;
    }
    header_step(c, "connecting relay", "ok", true);
    if (connect_room(c, err, sizeof err) == false)
    {
        snprintf(failure, sizeof failure, "fail (%s)", err);
        header_step(c, "private channel", failure, false);
        return false;
    }
    header_step(c, "private channel", "ok", true);

    long long start = platform_now_ms();
    for (;;)
    {
        long long elapsed = platform_now_ms() - start;
        bool settling = elapsed < PRESENCE_SETTLE_MS;
        if (elapsed >= HISTORY_WAIT_MS || (settling == false && room_waiting_history(c->room) == false))
        {
            break;
        }
        if (rt_pump(c->sock, 50) == false)
        {
            break;
        }
    }
    int remembered = cJSON_GetArraySize(c->held);
    char memory[32] = "empty";
    if (remembered > 0)
    {
        snprintf(memory, sizeof memory, "%d msgs", remembered);
    }
    header_step(c, "room memory", memory, true);

    char inbox_err[256] = "";
    if (connect_inbox(c, inbox_err, sizeof inbox_err))
    {
        header_step(c, "personal inbox", "ok", true);
    }
    else
    {
        snprintf(failure, sizeof failure, "fail (%s)", inbox_err);
        header_step(c, "personal inbox", failure, false);
    }
    cJSON *letters = letters_fetch(&c->session, false, NULL);
    bool loaded = letters != NULL;
    int unread = cJSON_GetArraySize(letters);
    cJSON_Delete(letters);
    char letters_status[32] = "none";
    if (loaded == false)
    {
        snprintf(letters_status, sizeof letters_status, "?");
    }
    else if (unread > 0)
    {
        snprintf(letters_status, sizeof letters_status, "%d unread", unread);
    }
    header_step(c, "letters", letters_status, loaded);

    load_watched(c);
    int listening = connect_watched(c);
    char watch_status[48] = "none";
    if (c->watched_count > 0)
    {
        snprintf(watch_status, sizeof watch_status, "%d/%d", listening, c->watched_count);
    }
    header_step(c, "other rooms", watch_status, listening == c->watched_count);

    int fresh = forum_unread(&c->session);
    char forum_status[32] = "nothing new";
    if (fresh < 0)
    {
        snprintf(forum_status, sizeof forum_status, "?");
    }
    else if (fresh > 0)
    {
        snprintf(forum_status, sizeof forum_status, "%d new", fresh);
    }
    header_step(c, "forum", forum_status, fresh >= 0);
    putchar('\n');

    c->in_header = false;
    char who[1024];
    who_is_here(c, who, sizeof who);
    system_line(c, "%s · %s", c->room_name, who);
    const cJSON *m;
    cJSON_ArrayForEach(m, c->held)
    {
        print_message(c, m);
    }
    cJSON_Delete(c->held);
    c->held = cJSON_CreateArray();
    return true;
}

// "@Nick" in a sent message → a push to those people (the server checks they can see the room).
static void notify_mentions(chat *c, const char *body)
{
    cJSON *to = cJSON_CreateArray();
    for (const char *at = strchr(body, '@'); at != NULL; at = strchr(at + 1, '@'))
    {
        char name[33];
        size_t n = 0;
        for (const char *p = at + 1; n < 32 && (isalnum((unsigned char)*p) != 0 || *p == '_'); ++p)
        {
            name[n++] = *p;
        }
        name[n] = '\0';
        if (n < 3)
        {
            continue;
        }
        const cJSON *entry;
        cJSON_ArrayForEach(entry, c->nicks)
        {
            bool other = strcmp(entry->string, c->session.user_id) != 0;
            if (other && same_text_ci(entry->valuestring, name))
            {
                cJSON_AddItemToArray(to, cJSON_CreateString(entry->string));
            }
        }
    }
    if (cJSON_GetArraySize(to) > 0)
    {
        cJSON *body_json = cJSON_CreateObject();
        cJSON_AddStringToObject(body_json, "type", "mention");
        cJSON_AddStringToObject(body_json, "room", c->topic);
        cJSON_AddItemToObject(body_json, "to", cJSON_Duplicate(to, true));
        char *text = cJSON_PrintUnformatted(body_json);
        cJSON_Delete(body_json);
        cJSON_Delete(api_function(&c->session, "push", text, NULL));
        cJSON_free(text);
    }
    cJSON_Delete(to);
}

static void print_who(chat *c)
{
    char who[1024];
    who_is_here(c, who, sizeof who);
    system_line(c, "in %s: %s", c->room_name, who);
}

static void emit_notification(void *ctx, const char *text)
{
    system_line(ctx, "%s", text);
}

// ":n": what came while the user wasn't looking (":n all": the recent ones too); then all read.
static void print_notifications(chat *c, bool all)
{
    cJSON *doc = notify_load();
    if (notify_doc_lines(doc, all == false, emit_notification, c) == 0)
    {
        system_line(c, "no notifications");
    }
    cJSON_Delete(doc);
    notify_mark_read(NULL);
}

static void print_letters(chat *c)
{
    cJSON *rows = letters_fetch(&c->session, false, NULL);
    if (rows == NULL)
    {
        system_line(c, "could not load letters");
        return;
    }
    if (cJSON_GetArraySize(rows) == 0)
    {
        system_line(c, "no letters");
    }
    long long now = (long long)time(NULL);
    int n = 0;
    const cJSON *row;
    cJSON_ArrayForEach(row, rows)
    {
        const cJSON *from = cJSON_GetObjectItemCaseSensitive(row, "from_id");
        const cJSON *expires = cJSON_GetObjectItemCaseSensitive(row, "expires_at");
        char painted[PAINTED_SIZE];
        const char *nick = cJSON_IsString(from)
            ? painted_nick(c, from->valuestring, painted)
            : "???";
        const char *expires_at = cJSON_IsString(expires)
            ? expires->valuestring
            : NULL;
        system_line(c, "%d ✉ %s · burns unread in %dd", ++n, nick, letters_days_left(expires_at, now));
    }
    if (n > 0)
    {
        system_line(c, "read with `euros read [n]` outside the chat: it burns when you close it");
    }
    cJSON_Delete(rows);
}

// "/img <path>": the path may be quoted (drag and drop into a terminal often adds quotes).
static void send_picture(chat *c, const char *arg)
{
    char path[2048];
    while (*arg == ' ')
    {
        ++arg;
    }
    snprintf(path, sizeof path, "%s", arg);
    size_t n = strlen(path);
    while (n > 0 && path[n - 1] == ' ')
    {
        path[--n] = '\0';
    }
    char *start = path;
    bool quoted = n >= 2 && (path[0] == '"' || path[0] == '\'') && path[n - 1] == path[0];
    if (quoted)
    {
        path[n - 1] = '\0';
        ++start;
    }
    if (start[0] == '\0')
    {
        system_line(c, "/img <file>: a JPEG, PNG, GIF or BMP");
        return;
    }
    system_line(c, "preparing the picture…");
    char err[128];
    cJSON *img = picture_from_file(start, err, sizeof err);
    if (img == NULL)
    {
        system_line(c, "picture not sent: %s", err);
        return;
    }
    room_send(c->room, "", img, platform_now_ms());
    cJSON_Delete(img);
}

static bool is_one_of(const char *s, const char *const *names)
{
    for (; *names != NULL; ++names)
    {
        if (strcmp(s, *names) == 0)
        {
            return true;
        }
    }
    return false;
}

// "/name args" or ":name args" (vim-like). Returns false when the user wants to leave.
static bool run_command(chat *c, char lead, const char *cmd)
{
    static const char *const QUIT[] = { "quit", "exit", NULL };
    static const char *const VIM_QUIT[] = { "q", "q!", "qa", "qa!", "wq", "x", NULL };
    static const char *const FULLSCREEN[] = { "fs", "fullscreen", NULL };
    static const char *const NOTIFICATIONS[] = { "n", "notifs", "notifications", NULL };
    static const char *const ALL_NOTIFICATIONS[] = { "n all", "notifs all", "notifications all", NULL };
    if (is_one_of(cmd, QUIT) || (lead == ':' && is_one_of(cmd, VIM_QUIT)))
    {
        return false;
    }
    if (strcmp(cmd, "who") == 0)
    {
        print_who(c);
    }
    else if (strcmp(cmd, "letters") == 0)
    {
        print_letters(c);
    }
    else if (strncmp(cmd, "img ", 4) == 0 || strcmp(cmd, "img") == 0)
    {
        send_picture(c, cmd + 3);
    }
    else if (is_one_of(cmd, NOTIFICATIONS) || is_one_of(cmd, ALL_NOTIFICATIONS))
    {
        print_notifications(c, is_one_of(cmd, ALL_NOTIFICATIONS));
    }
    else if (is_one_of(cmd, FULLSCREEN))
    {
        toggle_fullscreen(c);
    }
    else if (lead == ':' && cmd[0] != '\0' && strcmp(cmd, "help") != 0)
    {
        system_line(c, "not a command: :%s · :help · ::text sends \":text\"", cmd);
    }
    else if (cmd[0] != '\0' || lead == '/')
    {
        system_line(c, "/who: who is here · /letters: your letters · /img <file>: send a picture · "
                       "/fs: full screen on/off · /n: notifications (/n all) · /quit: leave · the same with \":\" (:q, :fs) · "
                       "@Nick: mention (they get a notification)");
    }
    return true;
}

// Returns false when the user wants to leave.
static bool handle_line(chat *c, const char *line)
{
    if (line[0] == '\0')
    {
        return true;
    }
    if (strncmp(line, "::", 2) == 0)
    {
        ++line; // "::)" sends ":)"
    }
    else if (line[0] == '/' || line[0] == ':')
    {
        return run_command(c, line[0], line + 1);
    }
    if (room_utf16_length(line) > ROOM_MAX_BODY)
    {
        system_line(c, "too long (max %d characters)", ROOM_MAX_BODY);
        return true;
    }
    if (room_send_text(c->room, line, platform_now_ms()))
    {
        notify_mentions(c, line);
    }
    return true;
}

static void keep_token_fresh(chat *c)
{
    char before[sizeof c->session.access_token];
    snprintf(before, sizeof before, "%s", c->session.access_token);
    if (api_ensure_fresh(&c->session, NULL) == false || strcmp(before, c->session.access_token) == 0)
    {
        return;
    }
    if (c->sock != NULL)
    {
        rt_set_token(c->sock, c->session.access_token);
    }
}

static bool find_group(chat *c, const char *name)
{
    cJSON *rows = api_select(&c->session, "groups?select=id,name", NULL);
    bool found = false;
    const cJSON *row;
    cJSON_ArrayForEach(row, rows)
    {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(row, "id");
        const cJSON *group = cJSON_GetObjectItemCaseSensitive(row, "name");
        if (cJSON_IsString(id) && cJSON_IsString(group) && same_text_ci(group->valuestring, name))
        {
            snprintf(c->topic, sizeof c->topic, "room:%s", id->valuestring);
            snprintf(c->room_name, sizeof c->room_name, "%s", group->valuestring);
            found = true;
            break;
        }
    }
    cJSON_Delete(rows);
    return found;
}

// Next typed line from the raw keyboard or the reader thread. False when the user leaves.
static bool next_line(chat *c, char *line, size_t size, bool *got)
{
    *got = false;
    if (c->keyboard)
    {
        le_result r = le_poll(line, size);
        *got = r == LE_LINE;
        return r != LE_QUIT;
    }
    *got = input_next(line, size);
    return *got || input_finished() == false;
}

static void run_loop(chat *c)
{
    long long next_token_check = platform_now_ms() + TOKEN_CHECK_MS;
    int wait_ms = c->keyboard
        ? 30
        : 200;
    char line[8192];
    for (;;)
    {
        if (c->sock != NULL && rt_pump(c->sock, wait_ms) == false)
        {
            disconnect(c);
            system_line(c, "connection lost, reconnecting…");
        }
        drop_closed(c);
        if (c->ch == NULL)
        {
            if (c->sock == NULL)
            {
                platform_sleep_ms(RECONNECT_MS);
            }
            keep_token_fresh(c);
            char err[256];
            if (c->sock == NULL)
            {
                c->sock = rt_connect(err, sizeof err);
            }
            if (c->sock != NULL && connect_room(c, err, sizeof err))
            {
                system_line(c, "back in %s", c->room_name);
                if (c->inbox == NULL)
                {
                    connect_inbox(c, err, sizeof err);
                }
                connect_watched(c);
            }
            else
            {
                disconnect(c); // start over with a fresh socket
            }
        }
        int focus = le_focus();
        if (focus != c->focus)
        {
            if (focus == 1)
            {
                notify_mark_read(c->topic); // back at the window: this room's news are on screen
            }
            c->focus = focus;
        }
        for (;;)
        {
            bool got = false;
            if (next_line(c, line, sizeof line, &got) == false)
            {
                return;
            }
            if (got == false)
            {
                break;
            }
            if (handle_line(c, line) == false)
            {
                return;
            }
        }
        long long now = platform_now_ms();
        if (now >= next_token_check)
        {
            next_token_check = now + TOKEN_CHECK_MS;
            keep_token_fresh(c);
            if (c->inbox == NULL && c->ch != NULL)
            {
                char err[256];
                connect_inbox(c, err, sizeof err);
            }
            if (c->ch != NULL)
            {
                connect_watched(c);
            }
        }
    }
}

static void cleanup(chat *c)
{
    if (le_fullscreen())
    {
        toggle_fullscreen(c); // back to the normal screen with what came meanwhile
    }
    le_stop();
    rt_close(c->sock);
    room_destroy(c->room);
    cJSON_Delete(c->nicks);
    cJSON_Delete(c->held);
    cJSON_Delete(c->recent);
    session_wipe(&c->session);
}

int chat_run(int argc, char **argv)
{
    chat c = { 0 };
    c.color = term_stdout_is_tty();
    c.pictures = picture_detect();
    c.in_header = true;
    c.focus = -1;
    c.held = cJSON_CreateArray();
    c.recent = cJSON_CreateArray();
    if (session_load(&c.session) == false)
    {
        fprintf(stderr, "Not signed in. Run `euros login`.\n");
        return 1;
    }
    api_error err = { 0 };
    if (api_ensure_fresh(&c.session, &err) == false)
    {
        fprintf(stderr, "Session expired: run `euros login` again\n");
        return 1;
    }

    snprintf(c.topic, sizeof c.topic, "room:main");
    snprintf(c.room_name, sizeof c.room_name, "#everyone");
    if (argc > 0 && find_group(&c, argv[0]) == false)
    {
        fprintf(stderr, "No such group: %s (see `euros rooms`)\n", argv[0]);
        session_wipe(&c.session);
        return 1;
    }
    load_nicks(&c);

    room_hooks hooks =
    {
        .send = hook_send,
        .send_ctx = &c,
        .on_message = hook_message,
        .on_history = hook_history,
        .on_presence = hook_presence,
        .on_failed = hook_failed,
        .on_join = hook_join,
        .on_leave = hook_leave,
        .ui = &c,
    };
    c.room = room_create(c.session.user_id, c.session.nickname, hooks);
    if (c.room == NULL || enter_with_header(&c) == false)
    {
        cleanup(&c);
        return 1;
    }

    notify_mark_read(c.topic); // entering the room shows what is in it
    c.keyboard = le_start();
    if (c.keyboard)
    {
        update_prompt(&c);
    }
    else if (input_start() == false)
    {
        fprintf(stderr, "Could not read the keyboard\n");
        cleanup(&c);
        return 1;
    }
    run_loop(&c);
    cleanup(&c);
    platform_exit_now(0); // the input thread may still be blocked on stdin
}

int chat_list_rooms(void)
{
    euros_session s;
    if (session_load(&s) == false)
    {
        fprintf(stderr, "Not signed in. Run `euros login`.\n");
        return 1;
    }
    api_error err = { 0 };
    cJSON *rows = api_select(&s, "groups?select=name&order=created_at", &err);
    session_wipe(&s);
    if (cJSON_IsArray(rows) == false)
    {
        cJSON_Delete(rows);
        fprintf(stderr, "Could not load rooms\n");
        return 1;
    }
    printf("#everyone\n");
    const cJSON *row;
    cJSON_ArrayForEach(row, rows)
    {
        printf("%s\n", cJSON_GetObjectItemCaseSensitive(row, "name")->valuestring);
    }
    cJSON_Delete(rows);
    return 0;
}
