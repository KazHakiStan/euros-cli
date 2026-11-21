#include "markup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((cond) == false)                                                                                           \
        {                                                                                                              \
            ++failures;                                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                           \
        }                                                                                                              \
    } while (0)

#define B "\033[1m"
#define UNB "\033[22m"
#define LINK(u) "\033]8;;" u "\033\\\033[4m" u "\033[24m\033]8;;\033\\"
#define BAR "\033[2m│\033[22m "

static void expect(const char *text, bool color, const char *indent, const char *want)
{
    char *got = markup_render(text, color, indent);
    bool same = got != NULL && strcmp(got, want) == 0;
    if (same == false)
    {
        ++failures;
        fprintf(stderr, "FAIL markup(\"%s\"):\n  got  [%s]\n  want [%s]\n", text, got, want);
    }
    free(got);
}

static void test_text(void)
{
    expect("<b>hi</b> & <script>", true, "", "<b>hi</b> & <script>\n");
    expect("", true, "", "\n");
    expect("two\nlines", true, "  ", "  two\n  lines\n");
    expect("crlf\r\nand\rcr", true, "", "crlf\nand\ncr\n");
}

static void test_bold(void)
{
    expect("a **b** c", true, "", "a " B "b" UNB " c\n");
    expect("**unclosed", true, "", "**unclosed\n");
    expect("****", true, "", "****\n");
    expect("***a**", true, "", B "*a" UNB "\n");
    expect("**a** and **b**", true, "", B "a" UNB " and " B "b" UNB "\n");
}

static void test_links(void)
{
    expect("see https://ex.com/a?b=1.", true, "", "see " LINK("https://ex.com/a?b=1") ".\n");
    expect("javascript:alert(1)", true, "", "javascript:alert(1)\n");
    expect("(http://x.org)", true, "", "(" LINK("http://x.org") ")\n");
    expect("**https://x.org**", true, "", B LINK("https://x.org") UNB "\n");
    expect("«https://x.org»", true, "", "«" LINK("https://x.org") "»\n");
    expect("https://", true, "", "https://\n");
    expect("a\"https://x.org\"b", true, "", "a\"" LINK("https://x.org") "\"b\n");
}

static void test_quotes(void)
{
    expect("> one\n>two\nreply", true, "", BAR "one\n" BAR "two\nreply\n");
    expect("> **b**", true, "", BAR B "b" UNB "\n");
    expect("> one\nreply", false, "  ", "  > one\n  reply\n");
}

static void test_control_characters(void)
{
    expect("a\033[31mred", true, "", "a [31mred\n");
    expect("https://x.org/\033]8;;evil\a", true, "", LINK("https://x.org/") " ]8;;evil \n");
    expect("c1\xc2\x9bx", true, "", "c1 x\n");
    expect("tab\there", false, "", "tab here\n");
}

int main(void)
{
    test_text();
    test_bold();
    test_links();
    test_quotes();
    test_control_characters();
    const char *verdict = failures > 0
        ? "FAILED"
        : "ok";
    printf("markup: %s\n", verdict);
    return failures > 0
        ? 1
        : 0;
}
