#include "version.h"

#include <stdio.h>
#include <stdlib.h>

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

static void test_compare(void)
{
    CHECK(version_compare("v0.2.0", "0.1.0") > 0);
    CHECK(version_compare("0.1.0", "v0.2.0") < 0);
    CHECK(version_compare("v1.0.0", "1.0.0") == 0);
    CHECK(version_compare("v1.10.0", "v1.9.9") > 0);
    CHECK(version_compare("v1.2", "1.2.0") == 0);
    CHECK(version_compare("v2", "1.99.99") > 0);
    CHECK(version_compare("v0.1.0", "0.1.0-dev") > 0);
    CHECK(version_compare("0.0.0-dev", "v0.1.0") < 0);
    CHECK(version_compare("v1.0.0-rc.2", "v1.0.0-rc.1") > 0);
}

static void test_tags(void)
{
    CHECK(version_valid_tag("v1"));
    CHECK(version_valid_tag("v0.1.0"));
    CHECK(version_valid_tag("v1.2.3-rc.1"));
    CHECK(version_valid_tag("1.2.3") == false);
    CHECK(version_valid_tag("v") == false);
    CHECK(version_valid_tag("v1.") == false);
    CHECK(version_valid_tag("v1.2.3.4") == false);
    CHECK(version_valid_tag("v1.2.3-") == false);
    CHECK(version_valid_tag("v1.2.3'; rm -rf ~") == false);
    CHECK(version_valid_tag("v1.2.3-a$(id)") == false);
    CHECK(version_valid_tag("v1.2.3 && x") == false);
    CHECK(version_valid_tag(NULL) == false);
}

static void test_repos(void)
{
    CHECK(version_valid_repo("KazHakiStan/euros-cli"));
    CHECK(version_valid_repo("a.b/c_d"));
    CHECK(version_valid_repo("noslash") == false);
    CHECK(version_valid_repo("/x") == false);
    CHECK(version_valid_repo("x/") == false);
    CHECK(version_valid_repo("a/b/c") == false);
    CHECK(version_valid_repo("a/b;id") == false);
}

int main(void)
{
    test_compare();
    test_tags();
    test_repos();
    const char *verdict = failures > 0
        ? "FAILED"
        : "ok";
    printf("version: %s\n", verdict);
    return failures > 0
        ? 1
        : 0;
}
