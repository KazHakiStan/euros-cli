#ifndef EUROS_INPUT_H
#define EUROS_INPUT_H

#include <stdbool.h>
#include <stddef.h>

// Starts a background thread that reads lines from stdin.
bool input_start(void);

// Non-blocking: copies the next typed line into out. False if none is waiting.
bool input_next(char *out, size_t size);

// True once stdin is closed and every line has been taken.
bool input_finished(void);

#endif
