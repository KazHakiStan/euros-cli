#include "api.h"
#include "cli.h"
#include "platform.h"
#include "term.h"

#include <cJSON.h>

#include <sodium.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TEXT_CAP 65536
#define LINE_CAP 16384

bool cli_open_session(euros_session *s)
{
    if (api_load_session(s) == false)
    {
        return false;
    }
    api_error err = { 0 };
    if (api_ensure_fresh(s, &err) == false)
    {
        api_fail(&err);
        session_wipe(s);
        return false;
    }
    return true;
}

cJSON *cli_load_profiles(euros_session *s, api_error *err)
{
    cJSON *rows = api_select(s, "profiles?select=id,nickname,public_key", err);
    if (cJSON_IsArray(rows) == false)
    {
        cJSON_Delete(rows);
        return NULL;
    }
    return rows;
}

const char *cli_str(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v)
        ? v->valuestring
        : NULL;
}

bool cli_same_text_ci(const char *a, const char *b)
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

const cJSON *cli_profile_by(const cJSON *profiles, const char *key, const char *value, bool ignore_case)
{
    const cJSON *p;
    cJSON_ArrayForEach(p, profiles)
    {
        const char *field = cli_str(p, key);
        if (field == NULL || value == NULL)
        {
            continue;
        }
        bool same = ignore_case
            ? cli_same_text_ci(field, value)
            : strcmp(field, value) == 0;
        if (same)
        {
            return p;
        }
    }
    return NULL;
}

const char *cli_nick(const cJSON *profiles, const char *user_id)
{
    const char *nick = cli_str(cli_profile_by(profiles, "id", user_id, false), "nickname");
    return nick != NULL
        ? nick
        : "???";
}

const char *cli_sty(const char *code)
{
    return term_stdout_is_tty()
        ? code
        : "";
}

void cli_put_nick(const char *nick)
{
    if (term_stdout_is_tty())
    {
        printf("\033[1;38;2;%sm", term_nick_color(nick));
    }
    term_put_clean(nick, " ");
    printf("%s", cli_sty("\033[0m"));
}

static long long days_from_civil(long long y, int m, int d)
{
    if (m <= 2)
    {
        --y;
    }
    long long era = y / 400;
    long long yoe = y - era * 400;
    int mp = m > 2
        ? m - 3
        : m + 9;
    long long doy = (153 * mp + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

long long cli_parse_time(const char *iso)
{
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, used = 0;
    if (iso == NULL || sscanf(iso, "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &s, &used) != 6 || used == 0)
    {
        return -1;
    }
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60)
    {
        return -1;
    }
    const char *p = iso + used;
    if (*p == '.')
    {
        ++p;
        while (isdigit((unsigned char)*p) != 0)
        {
            ++p;
        }
    }
    long long offset = 0;
    if (*p == '+' || *p == '-')
    {
        int sign = *p == '-'
            ? -1
            : 1;
        int oh = 0, om = 0;
        if (sscanf(p + 1, "%2d:%2d", &oh, &om) < 1)
        {
            return -1;
        }
        offset = sign * (oh * 3600LL + om * 60LL);
    }
    else if (*p != 'Z' && *p != '\0')
    {
        return -1;
    }
    return days_from_civil(y, mo, d) * CLI_DAY_S + h * 3600LL + mi * 60LL + s - offset;
}

void cli_local_time(const char *iso, char *out, size_t size)
{
    long long t = cli_parse_time(iso);
    time_t when = (time_t)t;
    struct tm *tm = t >= 0
        ? localtime(&when)
        : NULL;
    if (tm == NULL || strftime(out, size, "%d.%m %H:%M", tm) == 0)
    {
        snprintf(out, size, "?");
    }
}

bool cli_parse_number(const char *text, int *out)
{
    char *end = NULL;
    long n = strtol(text, &end, 10);
    if (end == text || *end != '\0' || n < 1 || n > 100000)
    {
        return false;
    }
    *out = (int)n;
    return true;
}

size_t cli_codepoints(const char *utf8)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)utf8; *p != '\0'; ++p)
    {
        if ((*p & 0xc0) != 0x80)
        {
            ++n;
        }
    }
    return n;
}

bool cli_has_text(const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; ++p)
    {
        if (isspace(*p) == 0)
        {
            return true;
        }
    }
    return false;
}

char *cli_read_text(const char *intro, bool *too_long)
{
    *too_long = false;
    bool interactive = term_stdin_is_tty();
    if (interactive && intro != NULL)
    {
        fprintf(stderr, "%s\nEnd with a line with a single dot.\n", intro);
    }
    char *text = malloc(TEXT_CAP);
    char *line = malloc(LINE_CAP);
    if (text == NULL || line == NULL)
    {
        free(text);
        free(line);
        return NULL;
    }
    size_t len = 0;
    text[0] = '\0';
    bool first = true;
    while (platform_read_line(line, LINE_CAP))
    {
        if ((interactive && strcmp(line, ".") == 0) || line[0] == '\x1a')
        {
            break;
        }
        size_t n = strlen(line);
        if (len + n + 2 > TEXT_CAP)
        {
            *too_long = true;
            break;
        }
        if (first == false)
        {
            text[len++] = '\n';
        }
        memcpy(text + len, line, n + 1);
        len += n;
        first = false;
    }
    sodium_memzero(line, LINE_CAP);
    free(line);
    if (*too_long)
    {
        cli_free_text(text);
        return NULL;
    }
    return text;
}

void cli_free_text(char *text)
{
    if (text != NULL)
    {
        sodium_memzero(text, strlen(text));
        free(text);
    }
}
