#ifndef EUROS_MARKUP_H
#define EUROS_MARKUP_H

#include <stdbool.h>

// Forum markup → terminal text: lines starting with ">" are quotes, **bold**, bare http(s) links.
// Control characters become spaces first, so nothing from the text can reach the terminal as an
// escape sequence. color: ANSI styles and OSC 8 hyperlinks; otherwise the text as written.
// Every line starts with indent and ends with "\n". Caller frees; NULL if out of memory.
char *markup_render(const char *text, bool color, const char *indent);

#endif
