#include "api.h"
#include "chat.h"
#include "cli.h"
#include "forum.h"
#include "http.h"
#include "keys.h"
#include "letters.h"
#include "picture.h"
#include "platform.h"
#include "session.h"
#include "term.h"
#include "update.h"
#include "version.h"

#include <cJSON.h>

#include <sodium.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ANSI styling only when writing to a terminal (not into a pipe or file).
static const char *sty(const char *code)
{
    return term_stdout_is_tty()
        ? code
        : "";
}

static const char *USAGE =
    "euros: the Euros terminal client\n"
    "\n"
    "  euros login [--not-my-device]   sign in with your personal password, or join with an invite\n"
    "  euros whoami                    who you are signed in as\n"
    "  euros invite                    create an invite for a friend\n"
    "  euros invites                   list your invites\n"
    "  euros chat [group]              the live room (#everyone, or one of your groups)\n"
    "  euros rooms                     #everyone and the groups you are in\n"
    "  euros letters                   your unread letters\n"
    "  euros read [n]                  read letter n (default: the oldest); it burns when you close it\n"
    "  euros write <nick> [--ttl D]    write a letter; unread, it burns after D = 1, 7 (default) or 30 days\n"
    "  euros forum [...]               boards, threads, replies (`euros forum help` for the rest)\n"
    "  euros show <file>               show a picture here the way the chat would (checks your terminal)\n"
    "  euros logout                   sign out and forget this device\n"
    "  euros version                   this version, and whether a newer one is out\n"
    "  euros update [--yes]            download and install the latest release\n"
    "\n"
    "--not-my-device: don't keep the letter key on disk (you'll be asked for the password to read letters).\n"
    "write: type the letter, then a line with a single dot (or Ctrl+D / Ctrl+Z). A pipe works too.\n";

static const char *star(const char *rank)
{
    return strcmp(rank, "founder") == 0
        ? "★ "
        : "";
}

// Error code from a failed redeem-invite response.
static void join_error(bool sent, const http_response *r, api_error *err)
{
    if (sent == false)
    {
        snprintf(err->code, sizeof err->code, "network");
        snprintf(err->detail, sizeof err->detail, "%s", r->error);
        return;
    }
    cJSON *res = cJSON_Parse(r->body);
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(res, "error");
    const char *name = cJSON_IsString(code)
        ? code->valuestring
        : "join_failed";
    snprintf(err->code, sizeof err->code, "%s", name);
    cJSON_Delete(res);
}

// Join with an invite code: the key is generated locally, only the derived login and public key are sent.
static int join(const char *code, bool remember)
{
    char key[EUROS_KEY_MAX];
    keys_generate(key, sizeof key);
    euros_keys k;
    if (keys_derive(key, &k) == false)
    {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "code", code);
    cJSON_AddStringToObject(body, "email", k.email);
    cJSON_AddStringToObject(body, "password", k.password);
    cJSON_AddStringToObject(body, "public_key", k.public_key);
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    http_response r;
    bool sent = http_request("POST", "/functions/v1/redeem-invite", NULL, json, NULL, &r);
    sodium_memzero(json, strlen(json));
    cJSON_free(json);
    api_error err = { 0 };
    if (sent == false || r.status != 200)
    {
        join_error(sent, &r, &err);
        http_free(&r);
        return api_fail(&err);
    }
    http_free(&r);

    euros_session s;
    if (api_sign_in(&k, remember, &s, &err) == false)
    {
        return api_fail(&err);
    }
    printf("\nWelcome. Here, you are ");
    cli_put_nick(s.nickname);
    printf(".\n\n");
    printf("This is your personal password. It is shown only once:\n\n    %s%s%s\n\n",
           sty("\033[1;36m"), key, sty("\033[0m"));
    printf("Write it down: it is the only way to sign in on another device.\n");
    sodium_memzero(key, sizeof key);
    session_wipe(&s);
    return 0;
}

static int sign_in(const char *key, bool remember)
{
    euros_keys k;
    euros_session s;
    api_error err = { 0 };
    if (keys_derive(key, &k) == false)
    {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }
    int rc;
    if (api_sign_in(&k, remember, &s, &err))
    {
        printf("Signed in as %s", star(s.rank));
        cli_put_nick(s.nickname);
        putchar('\n');
        rc = 0;
    }
    else
    {
        rc = api_fail(&err);
    }
    sodium_memzero(&k, sizeof k);
    session_wipe(&s);
    return rc;
}

static int cmd_login(int argc, char **argv)
{
    bool remember = true;
    for (int i = 0; i < argc; ++i)
    {
        if (strcmp(argv[i], "--not-my-device") == 0)
        {
            remember = false;
        }
        else
        {
            fprintf(stderr, "%s", USAGE);
            return 2;
        }
    }
    char raw[EUROS_KEY_MAX], value[EUROS_KEY_MAX];
    if (platform_read_secret("password: ", raw, sizeof raw) == false)
    {
        return 1;
    }
    keys_normalize(raw, value, sizeof value);
    sodium_memzero(raw, sizeof raw);
    if (value[0] == '\0')
    {
        return 1;
    }

    int rc;
    if (keys_is_invite(value))
    {
        rc = join(value, remember);
    }
    else
    {
        rc = sign_in(value, remember);
    }
    sodium_memzero(value, sizeof value);
    return rc;
}

static int cmd_whoami(void)
{
    euros_session s;
    if (api_load_session(&s) == false)
    {
        return 1;
    }
    api_error err = { 0 };
    if (api_ensure_fresh(&s, &err) == false)
    {
        return api_fail(&err);
    }
    const char *note = s.has_seed
        ? ""
        : "  (letter key not kept on this device)";
    printf("%s", star(s.rank));
    cli_put_nick(s.nickname);
    printf("%s\n", note);
    session_wipe(&s);
    return 0;
}

static int cmd_invite(void)
{
    euros_session s;
    if (api_load_session(&s) == false)
    {
        return 1;
    }
    api_error err = { 0 };
    cJSON *code = api_rpc(&s, "create_invite", NULL, &err);
    session_wipe(&s);
    if (cJSON_IsString(code) == false)
    {
        cJSON_Delete(code);
        return api_fail(&err);
    }
    printf("%s\n", code->valuestring);
    fprintf(stderr, "Give it to a friend. It works once, for 7 days.\n");
    cJSON_Delete(code);
    return 0;
}

static const char *invite_state(const cJSON *row, const char *now)
{
    const cJSON *used = cJSON_GetObjectItemCaseSensitive(row, "used_at");
    const cJSON *exp = cJSON_GetObjectItemCaseSensitive(row, "expires_at");
    if (cJSON_IsString(used))
    {
        return "used";
    }
    // ISO timestamps compare correctly as strings when both are UTC.
    if (cJSON_IsString(exp) && strncmp(exp->valuestring, now, 19) < 0)
    {
        return "expired";
    }
    return "active";
}

static int cmd_invites(void)
{
    euros_session s;
    if (api_load_session(&s) == false)
    {
        return 1;
    }
    api_error err = { 0 };
    cJSON *rows = api_select(&s, "invites?select=code,used_at,expires_at&order=created_at.desc", &err);
    session_wipe(&s);
    if (cJSON_IsArray(rows) == false)
    {
        cJSON_Delete(rows);
        return api_fail(&err);
    }
    if (cJSON_GetArraySize(rows) == 0)
    {
        printf("No invites yet. Create one with `euros invite`.\n");
    }
    char now[32];
    time_t t = time(NULL);
    strftime(now, sizeof now, "%Y-%m-%dT%H:%M:%S", gmtime(&t));
    const cJSON *row;
    cJSON_ArrayForEach(row, rows)
    {
        printf("%s  %s\n", cJSON_GetObjectItemCaseSensitive(row, "code")->valuestring, invite_state(row, now));
    }
    cJSON_Delete(rows);
    return 0;
}

// The same steps a picture takes in chat: shrink to a JPEG that fits a message, then draw it.
static int cmd_show(const char *path)
{
    char err[128];
    cJSON *img = picture_from_file(path, err, sizeof err);
    if (img == NULL)
    {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(img, "data");
    int w = (int)cJSON_GetObjectItemCaseSensitive(img, "w")->valuedouble;
    int h = (int)cJSON_GetObjectItemCaseSensitive(img, "h")->valuedouble;
    picture_protocol p = picture_detect();
    fprintf(stderr, "%dx%d, %zu base64 chars, protocol: %s\n", w, h, strlen(data->valuestring),
            picture_protocol_name(p));
    int rc = 0;
    if (p == PICTURES_OFF)
    {
        fprintf(stderr, "No picture protocol for this terminal. Try EUROS_IMAGES=kitty or EUROS_IMAGES=iterm.\n");
    }
    else if (picture_show(p, data->valuestring, 60, 16) == false)
    {
        fprintf(stderr, "Could not draw it\n");
        rc = 1;
    }
    cJSON_Delete(img);
    return rc;
}

static int cmd_logout(void)
{
    euros_session s;
    if (session_load(&s))
    {
        api_sign_out(&s);
    }
    session_wipe(&s);
    if (session_clear() == false)
    {
        fprintf(stderr, "Could not remove the session file\n");
        return 1;
    }
    printf("Signed out.\n");
    return 0;
}

int main(int argc, char **argv)
{
    platform_init();
    platform_utf8_args(&argc, &argv);
    if (keys_init() == false || http_global_init() == false)
    {
        fprintf(stderr, "init failed\n");
        return 1;
    }
    int rc = 2;
    const char *cmd = argc > 1
        ? argv[1]
        : "";
    bool about_versions = strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0 || strcmp(cmd, "update") == 0;
    if (cmd[0] != '\0' && cmd[0] != '-' && about_versions == false && strcmp(cmd, "help") != 0)
    {
        update_notice();
    }
    if (strcmp(cmd, "login") == 0)
    {
        rc = cmd_login(argc - 2, argv + 2);
    }
    else if (strcmp(cmd, "whoami") == 0)
    {
        rc = cmd_whoami();
    }
    else if (strcmp(cmd, "invite") == 0)
    {
        rc = cmd_invite();
    }
    else if (strcmp(cmd, "invites") == 0)
    {
        rc = cmd_invites();
    }
    else if (strcmp(cmd, "chat") == 0)
    {
        rc = chat_run(argc - 2, argv + 2);
    }
    else if (strcmp(cmd, "rooms") == 0)
    {
        rc = chat_list_rooms();
    }
    else if (strcmp(cmd, "letters") == 0)
    {
        rc = letters_list();
    }
    else if (strcmp(cmd, "read") == 0)
    {
        rc = letters_read(argc - 2, argv + 2);
    }
    else if (strcmp(cmd, "write") == 0)
    {
        rc = letters_write(argc - 2, argv + 2);
    }
    else if (strcmp(cmd, "show") == 0)
    {
        rc = argc == 3
            ? cmd_show(argv[2])
            : 2;
        if (rc == 2)
        {
            fprintf(stderr, "usage: euros show <file>\n");
        }
    }
    else if (strcmp(cmd, "forum") == 0)
    {
        rc = forum_run(argc - 2, argv + 2);
    }
    else if (strcmp(cmd, "logout") == 0)
    {
        rc = cmd_logout();
    }
    else if (strcmp(cmd, "--version") == 0)
    {
        printf("euros %s\n", version_current());
        rc = 0;
    }
    else if (strcmp(cmd, "version") == 0)
    {
        rc = update_version_cmd();
    }
    else if (strcmp(cmd, "update") == 0)
    {
        rc = update_run(argc - 2, argv + 2);
    }
    else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0)
    {
        fputs(USAGE, stdout);
        rc = 0;
    }
    else
    {
        fputs(USAGE, stderr);
    }
    http_global_cleanup();
    return rc;
}
