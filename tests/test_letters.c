#include "cli.h"
#include "keys.h"
#include "letters.h"

#include <cJSON.h>

#include <sodium.h>

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

static void test_time(void)
{
    CHECK(cli_parse_time("1970-01-01T00:00:00Z") == 0);
    CHECK(cli_parse_time("2026-11-21T01:02:03+00:00") == 1795222923LL);
    CHECK(cli_parse_time("2026-11-21T01:02:03.123456+00:00") == 1795222923LL);
    CHECK(cli_parse_time("2026-11-21T03:02:03+02:00") == 1795222923LL);
    CHECK(cli_parse_time("2024-02-29T12:00:00Z") == 1709208000LL);
    CHECK(cli_parse_time("2026-11-21") == -1);
    CHECK(cli_parse_time("garbage") == -1);
    CHECK(cli_parse_time(NULL) == -1);

    long long now = cli_parse_time("2026-11-21T00:00:00Z");
    CHECK(letters_days_left("2026-11-28T00:00:00Z", now) == 7);
    CHECK(letters_days_left("2026-11-27T23:00:00Z", now) == 7);
    CHECK(letters_days_left("2026-11-21T05:00:00Z", now) == 1);
    CHECK(letters_days_left("2026-11-20T00:00:00Z", now) == 1);
    CHECK(letters_days_left(NULL, now) == 1);
}

static void test_body(void)
{
    CHECK(letters_valid_body("hi"));
    CHECK(letters_valid_body("  привет  "));
    CHECK(letters_valid_body("") == false);
    CHECK(letters_valid_body(" \n\t ") == false);

    char *big = malloc(LETTER_MAX_BODY + 2);
    memset(big, 'a', LETTER_MAX_BODY);
    big[LETTER_MAX_BODY] = '\0';
    CHECK(letters_valid_body(big));
    big[LETTER_MAX_BODY] = 'a';
    big[LETTER_MAX_BODY + 1] = '\0';
    CHECK(letters_valid_body(big) == false);
    free(big);
}

static void public_key_b64(const unsigned char seed[32], char out[45])
{
    unsigned char pk[crypto_box_PUBLICKEYBYTES], sk[crypto_box_SECRETKEYBYTES];
    crypto_box_seed_keypair(pk, sk, seed);
    sodium_memzero(sk, sizeof sk);
    sodium_bin2base64(out, 45, pk, sizeof pk, sodium_base64_VARIANT_ORIGINAL);
}

static void test_round_trip(void)
{
    unsigned char alice[32], bob[32], eve[32];
    randombytes_buf(alice, sizeof alice);
    randombytes_buf(bob, sizeof bob);
    randombytes_buf(eve, sizeof eve);
    char alice_pk[45], bob_pk[45], eve_pk[45];
    public_key_b64(alice, alice_pk);
    public_key_b64(bob, bob_pk);
    public_key_b64(eve, eve_pk);

    const char *text = "первое письмо\n\"quoted\" 🔥";
    char *sealed = letter_seal(text, bob_pk, alice, NULL);
    CHECK(sealed != NULL);

    char *opened = letter_open(sealed, alice_pk, bob);
    CHECK(opened != NULL && strcmp(opened, text) == 0);
    free(opened);

    CHECK(letter_open(sealed, eve_pk, bob) == NULL);
    CHECK(letter_open(sealed, alice_pk, eve) == NULL);

    sealed[10] = sealed[10] == 'A'
        ? 'B'
        : 'A';
    CHECK(letter_open(sealed, alice_pk, bob) == NULL);
    free(sealed);

    char *again = letter_seal(text, bob_pk, alice, NULL);
    char *twice = letter_seal(text, bob_pk, alice, NULL);
    CHECK(again != NULL && twice != NULL && strcmp(again, twice) != 0);
    free(again);
    free(twice);
}

static void test_binding(void)
{
    cJSON *list = letters_changes_binding("u-1");
    const cJSON *b = cJSON_GetArrayItem(list, 0);
    CHECK(cJSON_GetArraySize(list) == 1);
    CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(b, "table")->valuestring, "letters") == 0);
    CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(b, "event")->valuestring, "INSERT") == 0);
    CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(b, "filter")->valuestring, "to_id=eq.u-1") == 0);
    cJSON_Delete(list);
}

int main(void)
{
    if (keys_init() == false)
    {
        fprintf(stderr, "libsodium init failed\n");
        return 2;
    }
    test_time();
    test_body();
    test_round_trip();
    test_binding();
    const char *verdict = failures > 0
        ? "FAILED"
        : "ok";
    printf("letters: %s\n", verdict);
    return failures > 0
        ? 1
        : 0;
}
