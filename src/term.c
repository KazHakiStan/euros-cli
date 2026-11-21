#include "term.h"

#include <stdio.h>

#ifdef _WIN32
#  include <io.h>
#  define isatty _isatty
#  define fileno _fileno
#else
#  include <unistd.h>
#endif

static const char *const PALETTE[TERM_PALETTE_SIZE] =
{
    "0;255;0",
    "255;0;255",
    "0;255;255",
    "255;128;0",
    "255;32;32",
    "255;255;0",
    "255;255;255",
};

const char *term_palette(int i)
{
    return PALETTE[i % TERM_PALETTE_SIZE];
}

const char *term_nick_color(const char *nick)
{
    unsigned int h = 0;
    const unsigned char *p = (const unsigned char *)nick;
    while (*p != '\0')
    {
        // Decode one UTF-8 code point: the hash runs over code points, not bytes.
        unsigned int cp = *p;
        int extra = 0;
        if (cp >= 0xf0)
        {
            cp &= 0x07u;
            extra = 3;
        }
        else if (cp >= 0xe0)
        {
            cp &= 0x0fu;
            extra = 2;
        }
        else if (cp >= 0xc0)
        {
            cp &= 0x1fu;
            extra = 1;
        }
        ++p;
        for (int i = 0; i < extra && (*p & 0xc0) == 0x80; ++i, ++p)
        {
            cp = (cp << 6) | (*p & 0x3fu);
        }
        h = h * 31u + cp;
    }
    return PALETTE[h % TERM_PALETTE_SIZE];
}

void term_put_clean(const char *s, const char *newline)
{
    const unsigned char *p = (const unsigned char *)s;
    for (; *p != '\0'; ++p)
    {
        if (*p == '\n')
        {
            fputs(newline, stdout);
        }
        else if (*p < 0x20 || *p == 0x7f)
        {
            putchar(' ');
        }
        else if (*p == 0xc2 && p[1] >= 0x80 && p[1] <= 0x9f)
        {
            putchar(' '); // C1 control characters
            ++p;
        }
        else
        {
            putchar(*p);
        }
    }
}

bool term_stdout_is_tty(void)
{
    return isatty(fileno(stdout)) != 0;
}

bool term_stdin_is_tty(void)
{
    return isatty(fileno(stdin)) != 0;
}

bool term_stderr_is_tty(void)
{
    return isatty(fileno(stderr)) != 0;
}
