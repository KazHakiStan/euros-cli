#include "http.h"
#include "platform.h"
#include "term.h"
#include "update.h"
#include "version.h"

#include <cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK_EVERY_S (24LL * 3600)
#define NOTICE_TIMEOUT_S 3L
#define COMMAND_TIMEOUT_S 10L

typedef struct
{
    long long checked_at;
    long long told_at;
    char latest[48];
} update_cache;

bool update_latest(char *tag, size_t size, long timeout_s)
{
    char url[256];
    snprintf(url, sizeof url, "https://api.github.com/repos/%s/releases/latest", version_repo());
    http_response r;
    bool ok = http_get_url(url, "application/vnd.github+json", timeout_s, &r) && r.status == 200;
    cJSON *json = ok
        ? cJSON_Parse(r.body)
        : NULL;
    http_free(&r);
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(json, "tag_name");
    ok = cJSON_IsString(name) && version_valid_tag(name->valuestring) && strlen(name->valuestring) < size;
    if (ok)
    {
        snprintf(tag, size, "%s", name->valuestring);
    }
    cJSON_Delete(json);
    return ok;
}

static bool cache_path(char *out, size_t size)
{
    char dir[1024];
    if (platform_config_dir(dir, sizeof dir) == false)
    {
        return false;
    }
#ifdef _WIN32
    return (size_t)snprintf(out, size, "%s\\update.json", dir) < size;
#else
    return (size_t)snprintf(out, size, "%s/update.json", dir) < size;
#endif
}

static void cache_load(update_cache *c)
{
    memset(c, 0, sizeof *c);
    char path[1100];
    char *text = cache_path(path, sizeof path)
        ? platform_read_file(path, NULL)
        : NULL;
    cJSON *json = cJSON_Parse(text);
    free(text);
    const cJSON *checked = cJSON_GetObjectItemCaseSensitive(json, "checked_at");
    const cJSON *told = cJSON_GetObjectItemCaseSensitive(json, "told_at");
    const cJSON *latest = cJSON_GetObjectItemCaseSensitive(json, "latest");
    if (cJSON_IsNumber(checked))
    {
        c->checked_at = (long long)checked->valuedouble;
    }
    if (cJSON_IsNumber(told))
    {
        c->told_at = (long long)told->valuedouble;
    }
    const cJSON *repo = cJSON_GetObjectItemCaseSensitive(json, "repo");
    bool same_repo = cJSON_IsString(repo) && strcmp(repo->valuestring, version_repo()) == 0;
    if (same_repo && cJSON_IsString(latest) && version_valid_tag(latest->valuestring))
    {
        snprintf(c->latest, sizeof c->latest, "%s", latest->valuestring);
    }
    cJSON_Delete(json);
}

static void cache_save(const update_cache *c)
{
    char path[1100];
    if (cache_path(path, sizeof path) == false)
    {
        return;
    }
    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "checked_at", (double)c->checked_at);
    cJSON_AddNumberToObject(json, "told_at", (double)c->told_at);
    cJSON_AddStringToObject(json, "latest", c->latest);
    cJSON_AddStringToObject(json, "repo", version_repo());
    char *text = cJSON_Print(json);
    cJSON_Delete(json);
    if (text != NULL)
    {
        platform_write_private(path, text, strlen(text));
        free(text);
    }
}

static bool newer(const char *tag)
{
    return tag[0] != '\0' && version_compare(tag, version_current()) > 0;
}

void update_notice(void)
{
    if (version_is_dev() || getenv("EUROS_NO_UPDATE_CHECK") != NULL || term_stderr_is_tty() == false)
    {
        return;
    }
    update_cache c;
    cache_load(&c);
    long long now = (long long)time(NULL);
    bool changed = false;
    if (now - c.checked_at >= CHECK_EVERY_S || now < c.checked_at)
    {
        char tag[48] = "";
        if (update_latest(tag, sizeof tag, NOTICE_TIMEOUT_S))
        {
            snprintf(c.latest, sizeof c.latest, "%s", tag);
        }
        c.checked_at = now;
        changed = true;
    }
    if (newer(c.latest) && now - c.told_at >= CHECK_EVERY_S)
    {
        fprintf(stderr, "\033[33mA new euros is out: %s (you have %s). Run `euros update`.\033[0m\n", c.latest,
                version_current());
        c.told_at = now;
        changed = true;
    }
    if (changed)
    {
        cache_save(&c);
    }
}

int update_version_cmd(void)
{
    printf("euros %s\n", version_current());
    char tag[48];
    if (update_latest(tag, sizeof tag, NOTICE_TIMEOUT_S) == false)
    {
        return 0;
    }
    if (newer(tag))
    {
        printf("A new version is out: %s. Run `euros update`.\n", tag);
    }
    else
    {
        printf("This is the latest release.\n");
    }
    return 0;
}

static bool ask_yes(const char *question)
{
    fputs(question, stdout);
    fflush(stdout);
    char answer[16];
    if (platform_read_line(answer, sizeof answer) == false)
    {
        return false;
    }
    return strcmp(answer, "y") == 0 || strcmp(answer, "Y") == 0 || strcmp(answer, "yes") == 0;
}

// The installer of that very release replaces the program, keeping it on PATH.
static int run_installer(const char *tag)
{
    const char *repo = version_repo();
    char cmd[1024];
#ifdef _WIN32
    snprintf(cmd, sizeof cmd,
             "powershell -NoProfile -ExecutionPolicy Bypass -Command \"$env:EUROS_VERSION='%s'; "
             "irm https://raw.githubusercontent.com/%s/%s/install.ps1 | iex\"",
             tag, repo, tag);
#else
    snprintf(cmd, sizeof cmd,
             "(curl -fsSL https://raw.githubusercontent.com/%s/%s/install.sh"
             " || wget -qO- https://raw.githubusercontent.com/%s/%s/install.sh) | EUROS_VERSION='%s' sh",
             repo, tag, repo, tag, tag);
#endif
    fflush(stdout);
    if (system(cmd) != 0)
    {
        fprintf(stderr, "The update failed. Get it by hand: https://github.com/%s/releases/tag/%s\n", repo, tag);
        return 1;
    }
    return 0;
}

int update_run(int argc, char **argv)
{
    bool yes = false;
    for (int i = 0; i < argc; ++i)
    {
        if (strcmp(argv[i], "--yes") == 0 || strcmp(argv[i], "-y") == 0)
        {
            yes = true;
        }
        else
        {
            fprintf(stderr, "usage: euros update [--yes]\n");
            return 2;
        }
    }
    printf("This is euros %s.\n", version_current());
    char tag[48];
    if (update_latest(tag, sizeof tag, COMMAND_TIMEOUT_S) == false)
    {
        fprintf(stderr, "Could not check for updates: https://github.com/%s/releases\n", version_repo());
        return 1;
    }
    update_cache c;
    cache_load(&c);
    c.checked_at = (long long)time(NULL);
    snprintf(c.latest, sizeof c.latest, "%s", tag);
    cache_save(&c);

    if (newer(tag) == false)
    {
        printf("It is the latest release (%s).\n", tag);
        return 0;
    }
    printf("New version: %s\nWhat's new: https://github.com/%s/releases/tag/%s\n", tag, version_repo(), tag);
    if (yes == false)
    {
        if (term_stdin_is_tty() == false)
        {
            printf("Run `euros update --yes` to install it.\n");
            return 0;
        }
        if (ask_yes("Download and install it now? [y/N] ") == false)
        {
            return 0;
        }
    }
    return run_installer(tag);
}
