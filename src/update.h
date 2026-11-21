#ifndef EUROS_UPDATE_H
#define EUROS_UPDATE_H

#include <stdbool.h>
#include <stddef.h>

// Latest release tag ("v1.2.3") from GitHub. False if there is none or it can't be checked.
bool update_latest(char *tag, size_t size, long timeout_s);

// At most once a day: tells on stderr that a newer release is out.
// Silent on any error, when stderr is not a terminal, in dev builds and with EUROS_NO_UPDATE_CHECK set.
void update_notice(void);

// `euros version`
int update_version_cmd(void);

// `euros update [--yes]`: checks, asks, then runs the installer of that release.
int update_run(int argc, char **argv);

#endif
