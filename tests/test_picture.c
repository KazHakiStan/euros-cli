#include "keys.h"
#include "picture.h"
#include "room.h"

#include <cJSON.h>

#include <sodium.h>

#include <stb_image.h>

#include <stb_image_write.h>

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

typedef struct
{
    unsigned char *data;
    size_t len;
} blob;

static void on_write(void *ctx, void *data, int size)
{
    blob *b = ctx;
    b->data = realloc(b->data, b->len + (size_t)size);
    memcpy(b->data + b->len, data, (size_t)size);
    b->len += (size_t)size;
}

static blob png_of(const unsigned char *px, int w, int h, int channels)
{
    blob b = { 0 };
    stbi_write_png_to_func(on_write, &b, w, h, channels, px, w * channels);
    return b;
}

static int number(const cJSON *obj, const char *key)
{
    return (int)cJSON_GetObjectItemCaseSensitive(obj, key)->valuedouble;
}

// Decodes the "data" JPEG of a chat picture; returns its pixels (RGB) or NULL.
static unsigned char *decode(const cJSON *img, int *w, int *h)
{
    const char *b64 = cJSON_GetObjectItemCaseSensitive(img, "data")->valuestring;
    size_t cap = strlen(b64);
    unsigned char *jpeg = malloc(cap);
    size_t len = 0;
    sodium_base642bin(jpeg, cap, b64, cap, NULL, &len, NULL, sodium_base64_VARIANT_ORIGINAL);
    bool is_jpeg = len > 3 && jpeg[0] == 0xff && jpeg[1] == 0xd8 && jpeg[2] == 0xff;
    int channels = 0;
    unsigned char *px = is_jpeg
        ? stbi_load_from_memory(jpeg, (int)len, w, h, &channels, 3)
        : NULL;
    free(jpeg);
    return px;
}

static void test_shrink(void)
{
    const unsigned char square[] =
    {
        0, 0, 0,       100, 100, 100,
        200, 200, 200, 100, 100, 100,
    };
    unsigned char *one = picture_shrink(square, 2, 2, 1, 1);
    CHECK(one[0] == 100 && one[1] == 100 && one[2] == 100);
    free(one);

    const unsigned char row[] =
    {
        10, 0, 0, 30, 0, 0, 50, 0, 0, 70, 0, 0,
    };
    unsigned char *half = picture_shrink(row, 4, 1, 2, 1);
    CHECK(half[0] == 20 && half[3] == 60);
    free(half);
}

static void test_big_gradient(void)
{
    int w = 2000, h = 1500;
    unsigned char *px = malloc((size_t)w * h * 3);
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            unsigned char *p = px + ((size_t)y * w + x) * 3;
            p[0] = (unsigned char)(x * 255 / w);
            p[1] = (unsigned char)(y * 255 / h);
            p[2] = 128;
        }
    }
    blob png = png_of(px, w, h, 3);
    free(px);
    char err[128] = "";
    cJSON *img = picture_from_memory(png.data, png.len, err, sizeof err);
    free(png.data);
    CHECK(img != NULL);
    CHECK(strcmp(cJSON_GetObjectItemCaseSensitive(img, "mime")->valuestring, "image/jpeg") == 0);
    CHECK(number(img, "w") == 1280 && number(img, "h") == 960);
    CHECK(strlen(cJSON_GetObjectItemCaseSensitive(img, "data")->valuestring) <= ROOM_MAX_IMAGE_B64);
    int dw = 0, dh = 0;
    unsigned char *back = decode(img, &dw, &dh);
    CHECK(back != NULL && dw == 1280 && dh == 960);
    stbi_image_free(back);
    cJSON_Delete(img);
}

static void test_small_and_transparent(void)
{
    unsigned char clear[10 * 10 * 4];
    memset(clear, 255, sizeof clear);
    for (size_t i = 3; i < sizeof clear; i += 4)
    {
        clear[i] = 0;
    }
    blob png = png_of(clear, 10, 10, 4);
    char err[128] = "";
    cJSON *img = picture_from_memory(png.data, png.len, err, sizeof err);
    free(png.data);
    CHECK(img != NULL && number(img, "w") == 10 && number(img, "h") == 10);
    int dw = 0, dh = 0;
    unsigned char *back = decode(img, &dw, &dh);
    CHECK(back != NULL && back[0] < 16 && back[1] < 16 && back[2] < 16);
    stbi_image_free(back);
    cJSON_Delete(img);
}

static void test_noise_never_too_big(void)
{
    int w = 2400, h = 2400;
    unsigned char *px = malloc((size_t)w * h * 3);
    randombytes_buf(px, (size_t)w * h * 3);
    blob png = png_of(px, w, h, 3);
    free(px);
    char err[128] = "";
    cJSON *img = picture_from_memory(png.data, png.len, err, sizeof err);
    free(png.data);
    if (img != NULL)
    {
        CHECK(strlen(cJSON_GetObjectItemCaseSensitive(img, "data")->valuestring) <= ROOM_MAX_IMAGE_B64);
        CHECK(number(img, "w") <= 1280);
    }
    else
    {
        CHECK(err[0] != '\0');
    }
    cJSON_Delete(img);
}

static void test_not_a_picture(void)
{
    const unsigned char text[] = "hello, this is not an image";
    char err[128] = "";
    CHECK(picture_from_memory(text, sizeof text, err, sizeof err) == NULL);
    CHECK(err[0] != '\0');
    CHECK(picture_show(PICTURES_KITTY, "aGVsbG8=", 40, 10) == false);
    CHECK(picture_show(PICTURES_ITERM, "not base64 !", 40, 10) == false);
}

static void test_kitty_sequence(void)
{
    const unsigned char tiny[3 * 2 * 3] = { 0 };
    char *seq = picture_kitty_sequence(tiny, 3, 2, 4);
    const char *head = "\033_Ga=T,f=24,o=z,q=2,s=3,v=2,r=4,m=0;";
    CHECK(seq != NULL && strncmp(seq, head, strlen(head)) == 0);
    CHECK(seq != NULL && strcmp(seq + strlen(seq) - 2, "\033\\") == 0);
    free(seq);

    int w = 300, h = 300;
    unsigned char *noise = malloc((size_t)w * h * 3);
    randombytes_buf(noise, (size_t)w * h * 3);
    seq = picture_kitty_sequence(noise, w, h, 10);
    free(noise);
    CHECK(seq != NULL && strstr(seq, ",m=1;") != NULL);
    int chunks = 0;
    bool sizes_ok = true;
    const char *last = NULL;
    for (const char *p = seq; p != NULL && (p = strstr(p, "\033_G")) != NULL; ++p)
    {
        const char *payload = strchr(p, ';') + 1;
        const char *end = strstr(payload, "\033\\");
        sizes_ok = sizes_ok && end - payload <= 4096;
        last = p;
        ++chunks;
    }
    CHECK(chunks > 1);
    CHECK(sizes_ok);
    CHECK(last != NULL && strncmp(last, "\033_Gm=0;", 7) == 0);
    free(seq);
}

int main(void)
{
    if (keys_init() == false)
    {
        fprintf(stderr, "libsodium init failed\n");
        return 2;
    }
    test_shrink();
    test_big_gradient();
    test_small_and_transparent();
    test_noise_never_too_big();
    test_not_a_picture();
    test_kitty_sequence();
    const char *verdict = failures > 0
        ? "FAILED"
        : "ok";
    printf("picture: %s\n", verdict);
    return failures > 0
        ? 1
        : 0;
}
