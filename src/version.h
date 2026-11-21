#ifndef EUROS_VERSION_H
#define EUROS_VERSION_H

#include <stdbool.h>

#ifndef EUROS_VERSION
#  define EUROS_VERSION "0.0.0-dev"
#endif

#ifndef EUROS_REPO
#  define EUROS_REPO "KazHakiStan/euros-cli"
#endif

// "1.2.3", or "1.2.3-dev" for local builds.
const char *version_current(void);

// GitHub "owner/name" the releases come from: EUROS_REPO env, else the built-in one.
const char *version_repo(void);

// Numeric compare of "v1.2.3" / "1.2.3" (missing parts are 0). A pre-release suffix ("-dev", "-rc1")
// ranks below the same numbers without one. <0, 0, >0 like strcmp.
int version_compare(const char *a, const char *b);

// True for a release tag that is safe to put into a URL or a shell command: v1, v1.2, v1.2.3, v1.2.3-rc.1
bool version_valid_tag(const char *tag);

// True for "owner/name" made of [A-Za-z0-9._-].
bool version_valid_repo(const char *repo);

bool version_is_dev(void);

#endif
