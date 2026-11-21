#include "api.h"
#include "chat.h"
#include "forum.h"
#include "input.h"
#include "letters.h"
#include "lineedit.h"
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

typedef struct
{
    euros_session session;
    rt_channel *ch;
    rt_channel *inbox; // inbox:<me>: new letters and read receipts
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
} chat;

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
    room_on_event(c->room, event, payload, platform_now_ms());
}

static bool connect_room(chat *c, bool *connected, char *err, size_t err_size)
{
    c->ch = rt_join(c->topic, c->session.user_id, NULL, c->session.access_token, on_channel, c, connected, err,
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
    if (strcmp(event, "postgres_changes") == 0)
    {
        const cJSON *data = cJSON_GetObjectItemCaseSensitive(payload, "data");
        const cJSON *record = cJSON_GetObjectItemCaseSensitive(data, "record");
        const cJSON *from = cJSON_GetObjectItemCaseSensitive(record, "from_id");
        char painted[PAINTED_SIZE];
        const char *nick = cJSON_IsString(from)
            ? painted_nick(c, from->valuestring, painted)
            : "someone";
        system_line(c, "✉ a letter from %s · /letters", nick);
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
        }
    }
}

static bool connect_inbox(chat *c, char *err, size_t err_size)
{
    char topic[64];
    snprintf(topic, sizeof topic, "inbox:%s", c->session.user_id);
    cJSON *changes = letters_changes_binding(c->session.user_id);
    c->inbox = rt_join(topic, NULL, changes, c->session.access_token, on_inbox, c, NULL, err, err_size);
    cJSON_Delete(changes);
    return c->inbox != NULL;
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
    bool connected = false;
    char err[256] = "";
    bool joined = connect_room(c, &connected, err, sizeof err);
    char failure[300];
    snprintf(failure, sizeof failure, "fail (%s)", err);
    if (connected == false)
    {
        header_step(c, "connecting relay", failure, false);
        return false;
    }
    header_step(c, "connecting relay", "ok", true);
    if (joined == false)
    {
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
        if (rt_pump(c->ch, 50) == false)
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

// Returns false when the user wants to leave.
static bool handle_line(chat *c, const char *line)
{
    if (line[0] == '\0')
    {
        return true;
    }
    if (line[0] == '/')
    {
        if (strcmp(line, "/quit") == 0 || strcmp(line, "/exit") == 0)
        {
            return false;
        }
        if (strcmp(line, "/who") == 0)
        {
            print_who(c);
        }
        else if (strcmp(line, "/letters") == 0)
        {
            print_letters(c);
        }
        else if (strncmp(line, "/img ", 5) == 0)
        {
            send_picture(c, line + 5);
        }
        else
        {
            system_line(c, "/who: who is here · /letters: your letters · /img <file>: send a picture · "
                           "/quit: leave · @Nick: mention (they get a notification)");
        }
        return true;
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
    if (c->ch != NULL)
    {
        rt_set_token(c->ch, c->session.access_token);
    }
    if (c->inbox != NULL)
    {
        rt_set_token(c->inbox, c->session.access_token);
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
        if (c->ch != NULL && rt_pump(c->ch, wait_ms) == false)
        {
            rt_leave(c->ch);
            c->ch = NULL;
            system_line(c, "connection lost, reconnecting…");
        }
        if (c->inbox != NULL && rt_pump(c->inbox, 0) == false)
        {
            rt_leave(c->inbox);
            c->inbox = NULL;
        }
        if (c->ch == NULL)
        {
            platform_sleep_ms(RECONNECT_MS);
            keep_token_fresh(c);
            char err[256];
            if (connect_room(c, NULL, err, sizeof err))
            {
                system_line(c, "back in %s", c->room_name);
            }
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
        }
    }
}

static void cleanup(chat *c)
{
    le_stop();
    rt_leave(c->ch);
    rt_leave(c->inbox);
    room_destroy(c->room);
    cJSON_Delete(c->nicks);
    cJSON_Delete(c->held);
    session_wipe(&c->session);
}

int chat_run(int argc, char **argv)
{
    chat c = { 0 };
    c.color = term_stdout_is_tty();
    c.pictures = picture_detect();
    c.in_header = true;
    c.held = cJSON_CreateArray();
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
