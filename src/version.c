#include "version.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

const char *version_current(void)
{
    return EUROS_VERSION;
}

const char *version_repo(void)
{
    const char *env = getenv("EUROS_REPO");
    return env != NULL && version_valid_repo(env)
        ? env
        : EUROS_REPO;
}

bool version_is_dev(void)
{
    return strchr(EUROS_VERSION, '-') != NULL;
}

// Reads up to three dot-separated numbers; returns the pre-release suffix after '-', or NULL.
static const char *parse(const char *s, long parts[3])
{
    parts[0] = 0;
    parts[1] = 0;
    parts[2] = 0;
    if (*s == 'v' || *s == 'V')
    {
        ++s;
    }
    for (int i = 0; i < 3 && isdigit((unsigned char)*s) != 0; ++i)
    {
        char *end = NULL;
        parts[i] = strtol(s, &end, 10);
        s = end;
        if (*s != '.')
        {
            break;
        }
        ++s;
    }
    return *s == '-'
        ? s + 1
        : NULL;
}

int version_compare(const char *a, const char *b)
{
    long pa[3], pb[3];
    const char *pre_a = parse(a, pa);
    const char *pre_b = parse(b, pb);
    for (int i = 0; i < 3; ++i)
    {
        if (pa[i] != pb[i])
        {
            return pa[i] < pb[i]
                ? -1
                : 1;
        }
    }
    if (pre_a == NULL && pre_b == NULL)
    {
        return 0;
    }
    if (pre_a == NULL)
    {
        return 1;
    }
    if (pre_b == NULL)
    {
        return -1;
    }
    int c = strcmp(pre_a, pre_b);
    if (c == 0)
    {
        return 0;
    }
    return c < 0
        ? -1
        : 1;
}

bool version_valid_tag(const char *tag)
{
    if (tag == NULL || tag[0] != 'v' || strlen(tag) > 40)
    {
        return false;
    }
    const char *p = tag + 1;
    int numbers = 0;
    for (;;)
    {
        if (isdigit((unsigned char)*p) == 0)
        {
            return false;
        }
        while (isdigit((unsigned char)*p) != 0)
        {
            ++p;
        }
        ++numbers;
        if (*p != '.' || numbers == 3)
        {
            break;
        }
        ++p;
    }
    if (*p == '\0')
    {
        return true;
    }
    if (*p != '-' || p[1] == '\0')
    {
        return false;
    }
    for (++p; *p != '\0'; ++p)
    {
        if (isalnum((unsigned char)*p) == 0 && *p != '.')
        {
            return false;
        }
    }
    return true;
}

static bool repo_char(char c)
{
    return isalnum((unsigned char)c) != 0 || c == '.' || c == '_' || c == '-';
}

bool version_valid_repo(const char *repo)
{
    const char *slash = strchr(repo, '/');
    if (slash == NULL || slash == repo || slash[1] == '\0' || strlen(repo) > 100)
    {
        return false;
    }
    for (const char *p = repo; *p != '\0'; ++p)
    {
        if (p != slash && repo_char(*p) == false)
        {
            return false;
        }
    }
    return true;
}
