#include "keys.h"
#include "platform.h"

#include <cJSON.h>

#include <sodium.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static const char *or_null(const char *s)
{
    return s != NULL
        ? s
        : "(null)";
}

static void check_str(const char *what, const char *input, const char *got, const char *want)
{
    if (got != NULL && want != NULL && strcmp(got, want) == 0)
    {
        return;
    }
    ++failures;
    fprintf(stderr, "FAIL %s for \"%s\":\n  got  %s\n  want %s\n", what, input, or_null(got), or_null(want));
}

static const char *str(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v)
        ? v->valuestring
        : NULL;
}

static int check_keys(const cJSON *vectors)
{
    int n = 0;
    const cJSON *v;
    cJSON_ArrayForEach(v, vectors)
    {
        const char *input = str(v, "input");
        char norm[EUROS_KEY_MAX];
        keys_normalize(input, norm, sizeof norm);
        check_str("normalized", input, norm, str(v, "normalized"));

        euros_keys k;
        if (keys_derive(input, &k) == false)
        {
            ++failures;
            fprintf(stderr, "FAIL derive \"%s\"\n", input);
            continue;
        }
        char seed_hex[65];
        sodium_bin2hex(seed_hex, sizeof seed_hex, k.box_seed, sizeof k.box_seed);
        check_str("email", input, k.email, str(v, "email"));
        check_str("password", input, k.password, str(v, "password"));
        check_str("box_seed", input, seed_hex, str(v, "box_seed_hex"));
        check_str("public_key", input, k.public_key, str(v, "public_key"));
        ++n;
    }
    return n;
}

static int check_letters(const cJSON *letters)
{
    int n = 0;
    const cJSON *v;
    cJSON_ArrayForEach(v, letters)
    {
        euros_keys from, to;
        keys_derive(str(v, "from"), &from);
        keys_derive(str(v, "to"), &to);
        unsigned char nonce[crypto_box_NONCEBYTES];
        sodium_hex2bin(nonce, sizeof nonce, str(v, "nonce_hex"), 48, NULL, NULL, NULL);

        char *ct = letter_seal(str(v, "body"), to.public_key, from.box_seed, nonce);
        check_str("letter ciphertext", str(v, "body"), ct, str(v, "ciphertext"));
        char *body = letter_open(str(v, "ciphertext"), from.public_key, to.box_seed);
        check_str("letter open", str(v, "ciphertext"), body, str(v, "body"));
        char *wrong = letter_open(str(v, "ciphertext"), to.public_key, to.box_seed); // wrong sender
        if (wrong != NULL)
        {
            ++failures;
            fprintf(stderr, "FAIL letter opened with the wrong sender key\n");
        }
        free(ct);
        free(body);
        free(wrong);
        ++n;
    }
    return n;
}

static void check_generated(void)
{
    char key[EUROS_KEY_MAX];
    for (int i = 0; i < 100; ++i)
    {
        keys_generate(key, sizeof key);
        char norm[EUROS_KEY_MAX];
        keys_normalize(key, norm, sizeof norm);
        if (strcmp(norm, key) != 0 || keys_is_invite(key))
        {
            ++failures;
            fprintf(stderr, "FAIL generated key %s\n", key);
        }
    }
    if (keys_is_invite("632bc55b7197") == false || keys_is_invite("632BC55B7197") || keys_is_invite("lynx-moss"))
    {
        ++failures;
        fprintf(stderr, "FAIL keys_is_invite\n");
    }
}

int main(int argc, char **argv)
{
    platform_init();
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s crypto-vectors.json\n", argv[0]);
        return 2;
    }
    if (keys_init() == false)
    {
        fprintf(stderr, "libsodium init failed\n");
        return 2;
    }
    char *text = platform_read_file(argv[1], NULL);
    cJSON *root = text != NULL
        ? cJSON_Parse(text)
        : NULL;
    free(text);
    if (root == NULL)
    {
        fprintf(stderr, "can't read %s\n", argv[1]);
        return 2;
    }

    int n = check_keys(cJSON_GetObjectItemCaseSensitive(root, "vectors"));
    n += check_letters(cJSON_GetObjectItemCaseSensitive(root, "letters"));
    check_generated();
    cJSON_Delete(root);

    const char *verdict = failures > 0
        ? "FAILED"
        : "all match";
    printf("%d vectors, %s\n", n, verdict);
    return failures > 0
        ? 1
        : 0;
}
