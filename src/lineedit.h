#ifndef EUROS_LINEEDIT_H
#define EUROS_LINEEDIT_H

#include <stdbool.h>
#include <stddef.h>

typedef enum
{
    LE_NONE,
    LE_LINE,
    LE_QUIT,
} le_result;

// Raw keyboard mode with an input line pinned under the output. False if stdin isn't a terminal.
bool le_start(void);

// Restores the terminal.
void le_stop(void);

void le_set_prompt(const char *prompt);

// Non-blocking: handles the keys typed so far. LE_LINE: a finished line is in out.
// A line that starts with ":" is a vim-like command line: shown without the prompt, Esc drops it.
le_result le_poll(char *out, size_t size);

// Full screen (alternate screen, output above a rule titled `title`, input on the last row) or back.
// Whatever was shown in full screen is gone when it closes.
void le_set_fullscreen(bool on, const char *title);
bool le_fullscreen(void);

// Wrap any output: hide removes the input line, show draws it again below the new output.
void le_hide(void);
void le_show(void);

#endif
