#include "keys.h"

#include <cJSON.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PWHASH_SALT "euros-pwhash-v1!" // 16 bytes = crypto_pwhash_SALTBYTES
#define KDF_CONTEXT "euros_v1"         // 8 bytes  = crypto_kdf_CONTEXTBYTES

enum
{
    KDF_LOGIN_ID = 1,
    KDF_LOGIN_PW = 2,
    KDF_BOX_SEED = 3
};

// Index = random byte value.
static const char *const WORDS[256] =
{
    "able","acid","aged","arch","area","army","atom","aunt","baby","back","bake","ball","band","bank","barn","base",
    "bath","bead","beam","bean","bear","beef","bell","belt","bike","bird","blue","boat","body","bold","bolt","bone",
    "book","boot","bowl","brew","bulb","bull","burn","bush","cafe","cage","cake","calm","camp","cape","card","cart",
    "cash","cave","cell","chef","chin","city","clay","clip","club","coal","coat","code","coin","cold","cone","cook",
    "cool","copy","cord","corn","cove","crab","crew","crop","crow","cube","curl","dark","dawn","deer","desk","dial",
    "dice","dish","dock","dome","door","dove","drum","dune","dust","duty","east","echo","edge","epic","exit","face",
    "fair","farm","fern","film","fire","fish","flag","flat","flow","foam","folk","font","food","fork","fort","frog",
    "fuel","game","gate","gear","gift","glow","glue","goat","gold","golf","gown","grid","grip","gull","hail","hall",
    "halo","hand","harp","hawk","heat","herb","hero","hill","hive","hood","hook","horn","host","hunt","icon","idea",
    "inch","iron","isle","jade","jazz","jeep","jest","joke","jump","jury","kale","keen","kelp","kick","kind","king",
    "kite","kiwi","knee","knot","lace","lake","lamp","land","lane","lava","lawn","leaf","lens","lily","lime","lion",
    "list","loaf","lock","loft","loop","lord","lute","lynx","mail","malt","mask","mast","maze","meal","melt","menu",
    "mesh","mild","milk","mill","mint","mist","moat","mole","monk","moon","moss","moth","mule","muse","nail","navy",
    "neck","nest","nice","node","noon","nose","note","nova","oath","oboe","oval","oven","pack","page","palm","park",
    "path","peak","pear","pine","pipe","plum","poem","pole","pond","pony","pool","port","rain","reef","rice","ring",
    "road","rock","roof","rope","rose","ruby","sage","sail","salt","sand","seal","seed","ship","silk","snow","soup",
};

bool keys_init(void)
{
    return sodium_init() >= 0;
}

void keys_normalize(const char *in, char *out, size_t size)
{
    if (size == 0)
    {
        return;
    }
    while (*in != '\0' && isspace((unsigned char)*in) != 0)
    {
        ++in;
    }
    const char *end = in + strlen(in);
    while (end > in && isspace((unsigned char)end[-1]) != 0)
    {
        --end;
    }

    size_t n = 0;
    bool sep = false;
    for (const char *p = in; p < end && n + 1 < size; ++p)
    {
        unsigned char c = (unsigned char)*p;
        if (isspace(c) != 0 || c == '_')
        {
            sep = true;
            continue;
        }
        if (sep && n + 1 < size)
        {
            out[n++] = '-';
            sep = false;
        }
        if (n + 1 < size)
        {
            out[n++] = (char)tolower(c);
        }
    }
    out[n] = '\0';
}

bool keys_is_invite(const char *s)
{
    if (strlen(s) != 12)
    {
        return false;
    }
    for (const char *p = s; *p != '\0'; ++p)
    {
        if (isxdigit((unsigned char)*p) == 0 || isupper((unsigned char)*p) != 0)
        {
            return false;
        }
    }
    return true;
}

void keys_generate(char *out, size_t size)
{
    snprintf(out, size, "%s-%s-%s-%s-%u",
             WORDS[randombytes_uniform(256)], WORDS[randombytes_uniform(256)],
             WORDS[randombytes_uniform(256)], WORDS[randombytes_uniform(256)],
             100 + randombytes_uniform(900));
}

bool keys_derive(const char *key, euros_keys *out)
{
    char norm[EUROS_KEY_MAX];
    keys_normalize(key, norm, sizeof norm);

    unsigned char master[32];
    if (crypto_pwhash(master, sizeof master, norm, strlen(norm), (const unsigned char *)PWHASH_SALT,
                      crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE,
                      crypto_pwhash_ALG_ARGON2ID13) != 0)
    {
        sodium_memzero(norm, sizeof norm);
        return false; // out of memory
    }
    sodium_memzero(norm, sizeof norm);

    unsigned char login_id[16], login_pw[32];
    crypto_kdf_derive_from_key(login_id, sizeof login_id, KDF_LOGIN_ID, KDF_CONTEXT, master);
    crypto_kdf_derive_from_key(login_pw, sizeof login_pw, KDF_LOGIN_PW, KDF_CONTEXT, master);
    crypto_kdf_derive_from_key(out->box_seed, sizeof out->box_seed, KDF_BOX_SEED, KDF_CONTEXT, master);
    sodium_memzero(master, sizeof master);

    unsigned char pk[crypto_box_PUBLICKEYBYTES], sk[crypto_box_SECRETKEYBYTES];
    crypto_box_seed_keypair(pk, sk, out->box_seed);
    sodium_memzero(sk, sizeof sk);

    char hex[33];
    sodium_bin2hex(hex, sizeof hex, login_id, sizeof login_id);
    snprintf(out->email, sizeof out->email, "%s@euros.local", hex);
    sodium_bin2hex(out->password, sizeof out->password, login_pw, sizeof login_pw);
    sodium_bin2base64(out->public_key, sizeof out->public_key, pk, sizeof pk, sodium_base64_VARIANT_ORIGINAL);
    sodium_memzero(login_pw, sizeof login_pw);
    return true;
}

void keys_secret_from_seed(const unsigned char seed[32], unsigned char sk[crypto_box_SECRETKEYBYTES])
{
    unsigned char pk[crypto_box_PUBLICKEYBYTES];
    crypto_box_seed_keypair(pk, sk, seed);
}

static bool decode_pk(const char *b64, unsigned char pk[crypto_box_PUBLICKEYBYTES])
{
    size_t len = 0;
    return sodium_base642bin(pk, crypto_box_PUBLICKEYBYTES, b64, strlen(b64), NULL, &len, NULL,
                             sodium_base64_VARIANT_ORIGINAL) == 0
        && len == crypto_box_PUBLICKEYBYTES;
}

char *letter_seal(const char *body, const char *recipient_pk_b64, const unsigned char sender_seed[32],
                  const unsigned char *nonce_in)
{
    unsigned char pk[crypto_box_PUBLICKEYBYTES], sk[crypto_box_SECRETKEYBYTES];
    if (decode_pk(recipient_pk_b64, pk) == false)
    {
        return NULL;
    }

    // Key order (v, body) matters: the ciphertext is compared byte for byte.
    // cJSON keeps non-ASCII as raw UTF-8.
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "v", 1);
    cJSON_AddStringToObject(obj, "body", body);
    char *plain = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (plain == NULL)
    {
        return NULL;
    }
    size_t plain_len = strlen(plain);

    size_t raw_len = crypto_box_NONCEBYTES + crypto_box_MACBYTES + plain_len;
    unsigned char *raw = malloc(raw_len);
    if (raw == NULL)
    {
        free(plain);
        return NULL;
    }
    if (nonce_in != NULL)
    {
        memcpy(raw, nonce_in, crypto_box_NONCEBYTES);
    }
    else
    {
        randombytes_buf(raw, crypto_box_NONCEBYTES);
    }

    keys_secret_from_seed(sender_seed, sk);
    int rc = crypto_box_easy(raw + crypto_box_NONCEBYTES, (unsigned char *)plain, plain_len, raw, pk, sk);
    sodium_memzero(sk, sizeof sk);
    sodium_memzero(plain, plain_len);
    free(plain);
    if (rc != 0)
    {
        free(raw);
        return NULL;
    }

    size_t b64_len = sodium_base64_ENCODED_LEN(raw_len, sodium_base64_VARIANT_ORIGINAL);
    char *b64 = malloc(b64_len);
    if (b64 != NULL)
    {
        sodium_bin2base64(b64, b64_len, raw, raw_len, sodium_base64_VARIANT_ORIGINAL);
    }
    free(raw);
    return b64;
}

char *letter_open(const char *ciphertext_b64, const char *sender_pk_b64, const unsigned char my_seed[32])
{
    unsigned char pk[crypto_box_PUBLICKEYBYTES], sk[crypto_box_SECRETKEYBYTES];
    if (decode_pk(sender_pk_b64, pk) == false)
    {
        return NULL;
    }

    size_t cap = strlen(ciphertext_b64);
    unsigned char *raw = malloc(cap + 1);
    if (raw == NULL)
    {
        return NULL;
    }
    size_t raw_len = 0;
    if (sodium_base642bin(raw, cap, ciphertext_b64, cap, NULL, &raw_len, NULL, sodium_base64_VARIANT_ORIGINAL) != 0
        || raw_len <= crypto_box_NONCEBYTES + crypto_box_MACBYTES)
    {
        free(raw);
        return NULL;
    }

    size_t plain_len = raw_len - crypto_box_NONCEBYTES - crypto_box_MACBYTES;
    char *plain = malloc(plain_len + 1);
    if (plain == NULL)
    {
        free(raw);
        return NULL;
    }
    keys_secret_from_seed(my_seed, sk);
    int rc = crypto_box_open_easy((unsigned char *)plain, raw + crypto_box_NONCEBYTES,
                                  raw_len - crypto_box_NONCEBYTES, raw, pk, sk);
    sodium_memzero(sk, sizeof sk);
    free(raw);
    if (rc != 0)
    {
        free(plain);
        return NULL;
    }
    plain[plain_len] = '\0';

    char *body = NULL;
    cJSON *obj = cJSON_Parse(plain);
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, "v");
    const cJSON *b = cJSON_GetObjectItemCaseSensitive(obj, "body");
    if (cJSON_IsNumber(v) && v->valuedouble == 1 && cJSON_IsString(b))
    {
        body = strdup(b->valuestring);
    }
    cJSON_Delete(obj);
    sodium_memzero(plain, plain_len);
    free(plain);
    return body;
}
