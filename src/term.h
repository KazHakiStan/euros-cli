#ifndef EUROS_TERM_H
#define EUROS_TERM_H

#include <stdbool.h>

#define TERM_PALETTE_SIZE 7

// "r;g;b" for a 24-bit color escape.
const char *term_palette(int i);

// Deterministic: a nickname always gets the same palette color.
const char *term_nick_color(const char *nick);

// Prints text from other people with control characters removed, so no escape sequences
// can reach the terminal. Each newline is printed as `newline`.
void term_put_clean(const char *s, const char *newline);

bool term_stdout_is_tty(void);
bool term_stdin_is_tty(void);
bool term_stderr_is_tty(void);

#endif
