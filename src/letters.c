#include "api.h"
#include "cli.h"
#include "keys.h"
#include "letters.h"
#include "platform.h"
#include "room.h"
#include "session.h"
#include "term.h"

#include <cJSON.h>

#include <sodium.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const int TTL_DAYS[] =
{
    1,
    7,
    30,
};

static const char *WRITE_USAGE = "usage: euros write <nick> [--ttl 1|7|30]\n";

int letters_days_left(const char *expires_iso, long long now_s)
{
    long long expires = cli_parse_time(expires_iso);
    long long left = expires - now_s;
    if (expires < 0 || left <= CLI_DAY_S)
    {
        return 1;
    }
    return (int)((left + CLI_DAY_S - 1) / CLI_DAY_S);
}

bool letters_valid_body(const char *body)
{
    return cli_has_text(body) && room_utf16_length(body) <= LETTER_MAX_BODY;
}

cJSON *letters_changes_binding(const char *user_id)
{
    char filter[64];
    snprintf(filter, sizeof filter, "to_id=eq.%s", user_id);
    cJSON *binding = cJSON_CreateObject();
    cJSON_AddStringToObject(binding, "event", "INSERT");
    cJSON_AddStringToObject(binding, "schema", "public");
    cJSON_AddStringToObject(binding, "table", "letters");
    cJSON_AddStringToObject(binding, "filter", filter);
    cJSON *list = cJSON_CreateArray();
    cJSON_AddItemToArray(list, binding);
    return list;
}

cJSON *letters_fetch(euros_session *s, bool with_ciphertext, api_error *err)
{
    const char *columns = with_ciphertext
        ? "id,from_id,created_at,expires_at,ciphertext"
        : "id,from_id,created_at,expires_at";
    char query[128];
    snprintf(query, sizeof query, "letters?select=%s&order=created_at", columns);
    cJSON *rows = api_select(s, query, err);
    if (cJSON_IsArray(rows) == false)
    {
        cJSON_Delete(rows);
        return NULL;
    }
    return rows;
}

static void days_text(int days, char *out, size_t size)
{
    const char *unit = days == 1
        ? "day"
        : "days";
    snprintf(out, size, "%d %s", days, unit);
}

// The letter key: from the session file, or derived from the personal password typed now.
static bool letter_seed(const euros_session *s, const cJSON *profiles, unsigned char seed[32])
{
    if (s->has_seed)
    {
        memcpy(seed, s->box_seed, 32);
        return true;
    }
    const char *my_pk = cli_str(cli_profile_by(profiles, "id", s->user_id, false), "public_key");
    if (my_pk == NULL)
    {
        fprintf(stderr, "Your profile has no letter key\n");
        return false;
    }
    fprintf(stderr, "Letters are encrypted, and the key is not kept on this device.\n");
    char raw[EUROS_KEY_MAX];
    if (platform_read_secret("personal password: ", raw, sizeof raw) == false)
    {
        return false;
    }
    euros_keys k;
    bool derived = keys_derive(raw, &k);
    sodium_memzero(raw, sizeof raw);
    if (derived == false)
    {
        fprintf(stderr, "Out of memory\n");
        return false;
    }
    bool match = strcmp(k.public_key, my_pk) == 0;
    if (match)
    {
        memcpy(seed, k.box_seed, 32);
    }
    else
    {
        fprintf(stderr, "Wrong password\n");
    }
    sodium_memzero(&k, sizeof k);
    return match;
}

int letters_list(void)
{
    euros_session s;
    if (cli_open_session(&s) == false)
    {
        return 1;
    }
    api_error err = { 0 };
    cJSON *profiles = cli_load_profiles(&s, &err);
    cJSON *rows = profiles != NULL
        ? letters_fetch(&s, false, &err)
        : NULL;
    session_wipe(&s);
    if (rows == NULL)
    {
        cJSON_Delete(profiles);
        return api_fail(&err);
    }

    if (cJSON_GetArraySize(rows) == 0)
    {
        printf("No letters.\n");
    }
    long long now = (long long)time(NULL);
    int n = 0;
    const cJSON *row;
    cJSON_ArrayForEach(row, rows)
    {
        char left[16];
        days_text(letters_days_left(cli_str(row, "expires_at"), now), left, sizeof left);
        printf("%2d  ✉ ", ++n);
        cli_put_nick(cli_nick(profiles, cli_str(row, "from_id")));
        printf("  %sburns unread in %s%s\n", cli_sty("\033[2m"), left, cli_sty("\033[0m"));
    }
    if (n > 0)
    {
        fprintf(stderr, "Read one with `euros read [n]`. It burns when you close it.\n");
    }
    cJSON_Delete(rows);
    cJSON_Delete(profiles);
    return 0;
}

static void leave_alt_screen(int sig)
{
    fputs("\033[?1049l", stdout);
    fflush(stdout);
    _Exit(130);
}

// On a terminal the letter is shown on the alternate screen, so it leaves no scrollback.
static void show_letter(const char *nick, const char *created_iso, const char *body)
{
    bool interactive = term_stdout_is_tty() && term_stdin_is_tty();
    if (interactive)
    {
        signal(SIGINT, leave_alt_screen);
        printf("\033[?1049h\033[H\033[2J");
    }
    char when[32];
    cli_local_time(created_iso, when, sizeof when);
    printf("\n  ✉ from ");
    cli_put_nick(nick);
    printf("  %s%s%s\n\n  ", cli_sty("\033[2m"), when, cli_sty("\033[0m"));
    if (body != NULL)
    {
        term_put_clean(body, "\n  ");
    }
    else
    {
        printf("%sThis letter cannot be opened: it is damaged or not really from this sender.%s",
               cli_sty("\033[2m"), cli_sty("\033[0m"));
    }
    printf("\n\n");
    if (interactive == false)
    {
        return;
    }
    printf("  %sWhen you close it, the letter is gone. Everywhere.%s\n", cli_sty("\033[2m"), cli_sty("\033[0m"));
    printf("  Enter: burn it ");
    fflush(stdout);
    char line[256];
    platform_read_line(line, sizeof line);
    printf("\033[?1049l");
    fflush(stdout);
    signal(SIGINT, SIG_DFL);
}

static void send_read_receipt(euros_session *s, const char *letter_id, const char *sender_id)
{
    char topic[64];
    snprintf(topic, sizeof topic, "inbox:%s", sender_id);
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(payload, "v", 1);
    cJSON_AddStringToObject(payload, "id", letter_id);
    cJSON_AddStringToObject(payload, "by", s->user_id);
    api_broadcast(s, topic, "letter_read", payload, NULL);
    cJSON_Delete(payload);
}

static int read_one(euros_session *s, const cJSON *profiles, const cJSON *letter)
{
    const char *id = cli_str(letter, "id");
    const char *from = cli_str(letter, "from_id");
    const char *ciphertext = cli_str(letter, "ciphertext");
    if (id == NULL || from == NULL || ciphertext == NULL)
    {
        fprintf(stderr, "Bad letter from the server\n");
        return 1;
    }
    unsigned char seed[32];
    if (letter_seed(s, profiles, seed) == false)
    {
        return 1;
    }
    const char *nick = cli_nick(profiles, from);
    const char *sender_pk = cli_str(cli_profile_by(profiles, "id", from, false), "public_key");
    char *body = sender_pk != NULL
        ? letter_open(ciphertext, sender_pk, seed)
        : NULL;
    sodium_memzero(seed, sizeof seed);
    bool opened = body != NULL;

    show_letter(nick, cli_str(letter, "created_at"), body);
    if (body != NULL)
    {
        sodium_memzero(body, strlen(body));
        free(body);
    }

    char query[96];
    snprintf(query, sizeof query, "letters?id=eq.%s", id);
    api_error err = { 0 };
    if (api_delete(s, query, &err) == false)
    {
        fprintf(stderr, "Could not burn the letter: ");
        return api_fail(&err);
    }
    printf("✉ burned.\n");
    if (opened)
    {
        send_read_receipt(s, id, from);
    }
    return 0;
}

int letters_read(int argc, char **argv)
{
    int index = 1;
    if (argc > 1 || (argc == 1 && cli_parse_number(argv[0], &index) == false))
    {
        fprintf(stderr, "usage: euros read [n]\n");
        return 2;
    }
    euros_session s;
    if (cli_open_session(&s) == false)
    {
        return 1;
    }
    api_error err = { 0 };
    cJSON *profiles = cli_load_profiles(&s, &err);
    cJSON *rows = profiles != NULL
        ? letters_fetch(&s, true, &err)
        : NULL;
    int rc = 0;
    int count = cJSON_GetArraySize(rows);
    if (rows == NULL)
    {
        rc = api_fail(&err);
    }
    else if (count == 0)
    {
        printf("No letters.\n");
    }
    else if (index > count)
    {
        const char *verb = count == 1
            ? "is"
            : "are";
        fprintf(stderr, "There %s only %d (see `euros letters`)\n", verb, count);
        rc = 1;
    }
    else
    {
        rc = read_one(&s, profiles, cJSON_GetArrayItem(rows, index - 1));
    }
    cJSON_Delete(rows);
    cJSON_Delete(profiles);
    session_wipe(&s);
    return rc;
}

static bool valid_ttl(int days)
{
    for (size_t i = 0; i < sizeof TTL_DAYS / sizeof *TTL_DAYS; ++i)
    {
        if (TTL_DAYS[i] == days)
        {
            return true;
        }
    }
    return false;
}

static int seal_and_send(euros_session *s, const cJSON *to, const char *body, const unsigned char seed[32], int ttl)
{
    const char *nick = cli_str(to, "nickname");
    char *ciphertext = letter_seal(body, cli_str(to, "public_key"), seed, NULL);
    if (ciphertext == NULL)
    {
        fprintf(stderr, "Could not encrypt the letter\n");
        return 1;
    }
    char expires[32];
    time_t when = time(NULL) + (time_t)(ttl * CLI_DAY_S);
    strftime(expires, sizeof expires, "%Y-%m-%dT%H:%M:%SZ", gmtime(&when));

    cJSON *row = cJSON_CreateObject();
    cJSON_AddStringToObject(row, "to_id", cli_str(to, "id"));
    cJSON_AddStringToObject(row, "ciphertext", ciphertext);
    cJSON_AddStringToObject(row, "expires_at", expires);
    char *json = cJSON_PrintUnformatted(row);
    cJSON_Delete(row);
    free(ciphertext);

    api_error err = { 0 };
    bool ok = json != NULL && api_insert(s, "letters", json, &err);
    cJSON_free(json);
    if (ok == false)
    {
        return api_fail(&err);
    }
    char left[16];
    days_text(ttl, left, sizeof left);
    printf("✉ sent to ");
    cli_put_nick(nick);
    printf(". Unread, it burns in %s.\n", left);
    return 0;
}

static int send_letter(euros_session *s, const cJSON *profiles, const char *wanted, int ttl)
{
    const cJSON *to = cli_profile_by(profiles, "nickname", wanted, true);
    const char *to_id = cli_str(to, "id");
    const char *nick = cli_str(to, "nickname");
    if (to_id == NULL || nick == NULL)
    {
        fprintf(stderr, "No such member: %s\n", wanted);
        return 1;
    }
    if (strcmp(to_id, s->user_id) == 0)
    {
        fprintf(stderr, "You can't write a letter to yourself\n");
        return 1;
    }
    if (cli_str(to, "public_key") == NULL)
    {
        fprintf(stderr, "%s can't receive letters yet\n", nick);
        return 1;
    }
    unsigned char seed[32];
    if (letter_seed(s, profiles, seed) == false)
    {
        return 1;
    }
    char left[16];
    days_text(ttl, left, sizeof left);
    char intro[160];
    snprintf(intro, sizeof intro, "Letter to %s. Unread, it burns in %s. No copy is kept.", nick, left);
    bool too_long = false;
    char *body = cli_read_text(intro, &too_long);
    int rc = 1;
    if (body == NULL || letters_valid_body(body) == false)
    {
        fprintf(stderr, "Not sent: the letter is empty or longer than %d characters\n", LETTER_MAX_BODY);
    }
    else
    {
        rc = seal_and_send(s, to, body, seed, ttl);
    }
    sodium_memzero(seed, sizeof seed);
    cli_free_text(body);
    return rc;
}

int letters_write(int argc, char **argv)
{
    const char *wanted = NULL;
    int ttl = 7;
    for (int i = 0; i < argc; ++i)
    {
        if (strcmp(argv[i], "--ttl") == 0 && i + 1 < argc)
        {
            if (cli_parse_number(argv[++i], &ttl) == false)
            {
                ttl = 0;
            }
        }
        else if (wanted == NULL && argv[i][0] != '-')
        {
            wanted = argv[i];
        }
        else
        {
            fputs(WRITE_USAGE, stderr);
            return 2;
        }
    }
    if (wanted == NULL || valid_ttl(ttl) == false)
    {
        fputs(WRITE_USAGE, stderr);
        return 2;
    }
    if (wanted[0] == '@')
    {
        ++wanted;
    }

    euros_session s;
    if (cli_open_session(&s) == false)
    {
        return 1;
    }
    api_error err = { 0 };
    cJSON *profiles = cli_load_profiles(&s, &err);
    int rc = profiles != NULL
        ? send_letter(&s, profiles, wanted, ttl)
        : api_fail(&err);
    cJSON_Delete(profiles);
    session_wipe(&s);
    return rc;
}
