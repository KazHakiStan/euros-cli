#ifndef EUROS_KEYS_H
#define EUROS_KEYS_H

#include <sodium.h>

#include <stdbool.h>
#include <stddef.h>

#define EUROS_KEY_MAX 128

typedef struct
{
    char email[64];      // <32 hex>@euros.local
    char password[65];   // 64 hex
    char public_key[45]; // base64 (original, padded), 44 chars
    unsigned char box_seed[32];
} euros_keys;

// sodium_init() wrapper; false if libsodium is unusable.
bool keys_init(void);

// trim, ASCII lowercase, runs of whitespace/'_' → '-'. out must hold strlen(in)+1.
void keys_normalize(const char *in, char *out, size_t size);

// True for an invite code (12 lowercase hex chars) after normalization.
bool keys_is_invite(const char *normalized);

// Random "word-word-word-word-NNN" key.
void keys_generate(char *out, size_t size);

// Argon2id + crypto_kdf → login + key pair. Deterministic. ~0.1–0.5 s. False on OOM.
bool keys_derive(const char *key, euros_keys *out);

// X25519 secret key from the seed.
void keys_secret_from_seed(const unsigned char seed[32], unsigned char sk[crypto_box_SECRETKEYBYTES]);

// ciphertext = base64(nonce || crypto_box_easy(JSON {"v":1,"body":...})).
// Returns a malloc'd base64 string or NULL. nonce = NULL → random.
char *letter_seal(const char *body, const char *recipient_pk_b64, const unsigned char sender_seed[32],
                  const unsigned char *nonce);

// Returns the malloc'd body, or NULL if it can't be opened (wrong sender, tampered, not for me).
char *letter_open(const char *ciphertext_b64, const char *sender_pk_b64, const unsigned char my_seed[32]);

#endif
