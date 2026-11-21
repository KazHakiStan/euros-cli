#ifndef EUROS_PLATFORM_H
#define EUROS_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>

// Call first: UTF-8 console on Windows.
void platform_init(void);

// Per-user config directory, created if missing:
//   Windows: %APPDATA%\euros    Unix: $XDG_CONFIG_HOME/euros or ~/.config/euros
// Returns false if it can't be determined/created.
bool platform_config_dir(char *out, size_t size);

// Reads a line from the terminal without echoing it.
// Falls back to a visible prompt if stdin is not a console. Strips the newline.
bool platform_read_secret(const char *prompt, char *out, size_t size);

// Writes a file readable only by the current user. Atomic replace.
bool platform_write_private(const char *path, const char *data, size_t len);

// Reads a whole file; caller frees. NULL if missing.
char *platform_read_file(const char *path, size_t *len_out);

bool platform_remove(const char *path);

// Like platform_read_file, for a UTF-8 path the user typed or passed (any language on Windows too).
char *platform_read_user_file(const char *utf8_path, size_t *len_out);

// Windows: replaces argv with UTF-8 copies of the real (UTF-16) arguments. Elsewhere: no-op.
void platform_utf8_args(int *argc, char ***argv);

// Reads one line of UTF-8 text from stdin (wide console API on Windows, so non-ASCII input works).
// Strips the newline. False on EOF.
bool platform_read_line(char *out, size_t size);

// Wall clock, milliseconds since the Unix epoch.
long long platform_now_ms(void);

void platform_sleep_ms(int ms);

// Flushes stdout/stderr and ends the process at once. Needed while another thread is blocked
// reading stdin: a normal exit would wait for that thread's lock on the stream.
_Noreturn void platform_exit_now(int code);

#endif
