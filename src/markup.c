#include "markup.h"

#include <stdlib.h>
#include <string.h>

typedef struct
{
    char *data;
    size_t len;
    size_t cap;
    bool failed;
} sbuf;

static void put(sbuf *b, const char *s, size_t n)
{
    if (b->failed)
    {
        return;
    }
    if (b->len + n + 1 > b->cap)
    {
        size_t cap = (b->cap + n + 1) * 2;
        char *grown = realloc(b->data, cap);
        if (grown == NULL)
        {
            b->failed = true;
            return;
        }
        b->data = grown;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void put_str(sbuf *b, const char *s)
{
    put(b, s, strlen(s));
}

// C0 and C1 control characters and DEL → one space each.
static size_t clean_line(const char *in, size_t n, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; ++i)
    {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x20 || c == 0x7f)
        {
            out[o++] = ' ';
        }
        else if (c == 0xc2 && i + 1 < n && (unsigned char)in[i + 1] >= 0x80 && (unsigned char)in[i + 1] <= 0x9f)
        {
            out[o++] = ' ';
            ++i;
        }
        else
        {
            out[o++] = (char)c;
        }
    }
    return o;
}

static bool url_stop(const char *s, size_t i, size_t n)
{
    unsigned char c = (unsigned char)s[i];
    if (c == ' ' || c == '<' || c == '>' || c == '"' || c == '\'')
    {
        return true;
    }
    return c == 0xc2 && i + 1 < n && (unsigned char)s[i + 1] == 0xa0; // no-break space
}

// Length of the link at s (0 if none), without trailing punctuation.
static size_t link_length(const char *s, size_t n)
{
    size_t scheme = 0;
    if (n >= 8 && memcmp(s, "https://", 8) == 0)
    {
        scheme = 8;
    }
    else if (n >= 7 && memcmp(s, "http://", 7) == 0)
    {
        scheme = 7;
    }
    if (scheme == 0)
    {
        return 0;
    }
    size_t end = scheme;
    while (end < n && url_stop(s, end, n) == false)
    {
        ++end;
    }
    if (end == scheme)
    {
        return 0;
    }
    for (;;)
    {
        bool raquo = end >= 2 && (unsigned char)s[end - 2] == 0xc2 && (unsigned char)s[end - 1] == 0xbb;
        if (raquo)
        {
            end -= 2;
            continue;
        }
        if (end > 0 && strchr(".,;:!?)]}\"'", s[end - 1]) != NULL)
        {
            --end;
            continue;
        }
        break;
    }
    return end;
}

static void emit_links(sbuf *b, const char *s, size_t n)
{
    size_t start = 0;
    size_t i = 0;
    while (i < n)
    {
        size_t len = s[i] == 'h'
            ? link_length(s + i, n - i)
            : 0;
        if (len == 0)
        {
            ++i;
            continue;
        }
        put(b, s + start, i - start);
        put_str(b, "\033]8;;");
        put(b, s + i, len);
        put_str(b, "\033\\\033[4m");
        put(b, s + i, len);
        put_str(b, "\033[24m\033]8;;\033\\");
        i += len;
        start = i;
    }
    put(b, s + start, n - start);
}

// "**bold**" needs at least one character inside; an unmatched "**" stays as text.
static void emit_inline(sbuf *b, const char *s, size_t n)
{
    size_t start = 0;
    size_t i = 0;
    while (i + 1 < n)
    {
        if (s[i] != '*' || s[i + 1] != '*')
        {
            ++i;
            continue;
        }
        size_t close = 0;
        for (size_t j = i + 3; j + 1 < n; ++j)
        {
            if (s[j] == '*' && s[j + 1] == '*')
            {
                close = j;
                break;
            }
        }
        if (close == 0)
        {
            ++i;
            continue;
        }
        emit_links(b, s + start, i - start);
        put_str(b, "\033[1m");
        emit_links(b, s + i + 2, close - i - 2);
        put_str(b, "\033[22m");
        i = close + 2;
        start = i;
    }
    emit_links(b, s + start, n - start);
}

static void emit_line(sbuf *b, const char *line, size_t n, bool color, const char *indent)
{
    put_str(b, indent);
    bool quote = n > 0 && line[0] == '>';
    if (quote)
    {
        ++line;
        --n;
        if (n > 0 && line[0] == ' ')
        {
            ++line;
            --n;
        }
    }
    if (color == false)
    {
        if (quote)
        {
            put_str(b, "> ");
        }
        put(b, line, n);
    }
    else
    {
        if (quote)
        {
            put_str(b, "\033[2m│\033[22m ");
        }
        emit_inline(b, line, n);
    }
    put_str(b, "\n");
}

char *markup_render(const char *text, bool color, const char *indent)
{
    sbuf b = { 0 };
    put(&b, "", 0);
    char *clean = malloc(strlen(text) + 1);
    if (clean == NULL)
    {
        free(b.data);
        return NULL;
    }
    const char *p = text;
    for (;;)
    {
        size_t n = strcspn(p, "\r\n");
        size_t len = clean_line(p, n, clean);
        emit_line(&b, clean, len, color, indent);
        p += n;
        if (*p == '\0')
        {
            break;
        }
        if (p[0] == '\r' && p[1] == '\n')
        {
            ++p;
        }
        ++p;
    }
    free(clean);
    if (b.failed)
    {
        free(b.data);
        return NULL;
    }
    return b.data;
}
