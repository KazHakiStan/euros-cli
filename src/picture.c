#include "picture.h"
#include "platform.h"
#include "room.h"
#include "term.h"

#include <cJSON.h>

#include <sodium.h>

#include <stb_image.h>

#include <stb_image_write.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Defined (extern) in stb_image_write's implementation but not declared in its header.
unsigned char *stbi_zlib_compress(unsigned char *data, int data_len, int *out_len, int quality);

#define KITTY_CHUNK 4096
#define SHOW_MAX_SIDE 640
#define FILE_MAX_BYTES (64 * 1024 * 1024)

static const struct
{
    int side;
    int quality;
} LADDER[] =
{
    { 1280, 80 },
    { 1280, 60 },
    { 1024, 60 },
    { 800, 55 },
    { 640, 50 },
    { 480, 45 },
};

typedef struct
{
    unsigned char *data;
    size_t len;
    size_t cap;
    bool failed;
} bytes;

static void bytes_put(bytes *b, const void *src, size_t n)
{
    if (b->failed)
    {
        return;
    }
    if (b->len + n + 1 > b->cap)
    {
        size_t cap = (b->cap + n + 1) * 2;
        unsigned char *grown = realloc(b->data, cap);
        if (grown == NULL)
        {
            b->failed = true;
            return;
        }
        b->data = grown;
        b->cap = cap;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void bytes_str(bytes *b, const char *s)
{
    bytes_put(b, s, strlen(s));
}

static void on_jpeg(void *ctx, void *data, int size)
{
    bytes_put(ctx, data, (size_t)size);
}

static char *to_base64(const unsigned char *bin, size_t len)
{
    size_t size = sodium_base64_ENCODED_LEN(len, sodium_base64_VARIANT_ORIGINAL);
    char *out = malloc(size);
    if (out != NULL)
    {
        sodium_bin2base64(out, size, bin, len, sodium_base64_VARIANT_ORIGINAL);
    }
    return out;
}

static unsigned char *from_base64(const char *b64, size_t *len)
{
    size_t cap = strlen(b64);
    unsigned char *out = malloc(cap + 1);
    if (out == NULL)
    {
        return NULL;
    }
    if (sodium_base642bin(out, cap, b64, cap, NULL, len, NULL, sodium_base64_VARIANT_ORIGINAL) != 0)
    {
        free(out);
        return NULL;
    }
    return out;
}

static bool contains(const char *haystack, const char *needle)
{
    return haystack != NULL && strstr(haystack, needle) != NULL;
}

picture_protocol picture_detect(void)
{
    if (term_stdout_is_tty() == false)
    {
        return PICTURES_OFF;
    }
    const char *forced = getenv("EUROS_IMAGES");
    if (forced != NULL)
    {
        if (strcmp(forced, "kitty") == 0)
        {
            return PICTURES_KITTY;
        }
        if (strcmp(forced, "iterm") == 0)
        {
            return PICTURES_ITERM;
        }
        return PICTURES_OFF;
    }
    const char *term = getenv("TERM");
    const char *program = getenv("TERM_PROGRAM");
    if (getenv("KITTY_WINDOW_ID") != NULL || contains(term, "kitty") || contains(term, "ghostty")
        || contains(program, "ghostty"))
    {
        return PICTURES_KITTY;
    }
    // Inside tmux TERM_PROGRAM says "tmux"; what the outer terminal exported is still inherited.
    if (contains(program, "WezTerm") || contains(program, "iTerm.app") || contains(term, "wezterm")
        || getenv("WEZTERM_PANE") != NULL || contains(getenv("LC_TERMINAL"), "iTerm2"))
    {
        return PICTURES_ITERM;
    }
    return PICTURES_OFF;
}

const char *picture_protocol_name(picture_protocol p)
{
    if (p == PICTURES_KITTY)
    {
        return "kitty";
    }
    if (p == PICTURES_ITERM)
    {
        return "iterm";
    }
    return "off";
}

unsigned char *picture_shrink(const unsigned char *src, int sw, int sh, int dw, int dh)
{
    unsigned char *dst = malloc((size_t)dw * (size_t)dh * 3);
    if (dst == NULL)
    {
        return NULL;
    }
    for (int dy = 0; dy < dh; ++dy)
    {
        int y0 = (int)((long long)dy * sh / dh);
        int y1 = (int)((long long)(dy + 1) * sh / dh);
        if (y1 <= y0)
        {
            y1 = y0 + 1;
        }
        for (int dx = 0; dx < dw; ++dx)
        {
            int x0 = (int)((long long)dx * sw / dw);
            int x1 = (int)((long long)(dx + 1) * sw / dw);
            if (x1 <= x0)
            {
                x1 = x0 + 1;
            }
            unsigned long sum[3] = { 0, 0, 0 };
            for (int y = y0; y < y1; ++y)
            {
                const unsigned char *row = src + ((size_t)y * sw + x0) * 3;
                for (int x = x0; x < x1; ++x, row += 3)
                {
                    sum[0] += row[0];
                    sum[1] += row[1];
                    sum[2] += row[2];
                }
            }
            unsigned long count = (unsigned long)(y1 - y0) * (unsigned long)(x1 - x0);
            unsigned char *out = dst + ((size_t)dy * dw + dx) * 3;
            for (int c = 0; c < 3; ++c)
            {
                out[c] = (unsigned char)((sum[c] + count / 2) / count);
            }
        }
    }
    return dst;
}

// Largest size within side × side keeping the aspect ratio, never upscaled.
static void fit(int w, int h, int side, int *dw, int *dh)
{
    int longest = w > h
        ? w
        : h;
    if (longest <= side)
    {
        *dw = w;
        *dh = h;
        return;
    }
    *dw = (int)(((long long)w * side + longest / 2) / longest);
    *dh = (int)(((long long)h * side + longest / 2) / longest);
    if (*dw < 1)
    {
        *dw = 1;
    }
    if (*dh < 1)
    {
        *dh = 1;
    }
}

// RGBA → RGB on black, like the web client does for transparent PNGs.
static unsigned char *flatten(const unsigned char *rgba, int w, int h)
{
    size_t n = (size_t)w * (size_t)h;
    unsigned char *rgb = malloc(n * 3);
    if (rgb == NULL)
    {
        return NULL;
    }
    for (size_t i = 0; i < n; ++i)
    {
        unsigned int a = rgba[i * 4 + 3];
        for (int c = 0; c < 3; ++c)
        {
            rgb[i * 3 + c] = (unsigned char)((rgba[i * 4 + c] * a + 127) / 255);
        }
    }
    return rgb;
}

static void set_err(char *err, size_t size, const char *text)
{
    snprintf(err, size, "%s", text);
}

cJSON *picture_from_memory(const unsigned char *data, size_t len, char *err, size_t err_size)
{
    int w = 0, h = 0, channels = 0;
    if (len > (size_t)FILE_MAX_BYTES)
    {
        set_err(err, err_size, "the file is too big");
        return NULL;
    }
    unsigned char *rgba = stbi_load_from_memory(data, (int)len, &w, &h, &channels, 4);
    if (rgba == NULL)
    {
        set_err(err, err_size, "not a picture that can be read (JPEG, PNG, GIF, BMP)");
        return NULL;
    }
    unsigned char *rgb = flatten(rgba, w, h);
    stbi_image_free(rgba);
    if (rgb == NULL)
    {
        set_err(err, err_size, "out of memory");
        return NULL;
    }

    cJSON *img = NULL;
    for (size_t i = 0; i < sizeof LADDER / sizeof *LADDER && img == NULL; ++i)
    {
        int dw = 0, dh = 0;
        fit(w, h, LADDER[i].side, &dw, &dh);
        unsigned char *small = picture_shrink(rgb, w, h, dw, dh);
        bytes jpeg = { 0 };
        bool encoded = small != NULL && stbi_write_jpg_to_func(on_jpeg, &jpeg, dw, dh, 3, small, LADDER[i].quality) != 0;
        free(small);
        char *b64 = encoded && jpeg.failed == false
            ? to_base64(jpeg.data, jpeg.len)
            : NULL;
        free(jpeg.data);
        if (b64 != NULL && strlen(b64) <= ROOM_MAX_IMAGE_B64)
        {
            img = cJSON_CreateObject();
            cJSON_AddStringToObject(img, "mime", "image/jpeg");
            cJSON_AddNumberToObject(img, "w", dw);
            cJSON_AddNumberToObject(img, "h", dh);
            cJSON_AddStringToObject(img, "data", b64);
        }
        free(b64);
    }
    free(rgb);
    if (img == NULL)
    {
        set_err(err, err_size, "the picture is too big even when shrunk");
    }
    return img;
}

cJSON *picture_from_file(const char *path, char *err, size_t err_size)
{
    size_t len = 0;
    char *data = platform_read_user_file(path, &len);
    if (data == NULL)
    {
        set_err(err, err_size, "can't read the file");
        return NULL;
    }
    cJSON *img = picture_from_memory((const unsigned char *)data, len, err, err_size);
    free(data);
    return img;
}

char *picture_kitty_sequence(const unsigned char *rgb, int w, int h, int rows)
{
    int zlen = 0;
    unsigned char *z = stbi_zlib_compress((unsigned char *)rgb, w * h * 3, &zlen, 8);
    char *b64 = z != NULL
        ? to_base64(z, (size_t)zlen)
        : NULL;
    free(z);
    if (b64 == NULL)
    {
        return NULL;
    }
    bytes out = { 0 };
    size_t total = strlen(b64);
    size_t pos = 0;
    do
    {
        size_t n = total - pos;
        if (n > KITTY_CHUNK)
        {
            n = KITTY_CHUNK;
        }
        int more = pos + n < total
            ? 1
            : 0;
        char head[128];
        if (pos == 0)
        {
            snprintf(head, sizeof head, "\033_Ga=T,f=24,o=z,q=2,s=%d,v=%d,r=%d,m=%d;", w, h, rows, more);
        }
        else
        {
            snprintf(head, sizeof head, "\033_Gm=%d;", more);
        }
        bytes_str(&out, head);
        bytes_put(&out, b64 + pos, n);
        bytes_str(&out, "\033\\");
        pos += n;
    } while (pos < total);
    free(b64);
    if (out.failed)
    {
        free(out.data);
        return NULL;
    }
    return (char *)out.data;
}

// Rows to use so the picture fits max_cols × max_rows, assuming cells about twice as tall as wide.
static int rows_for(int w, int h, int max_cols, int max_rows)
{
    int rows = h / 24;
    if (rows < 3)
    {
        rows = 3;
    }
    if (rows > max_rows)
    {
        rows = max_rows;
    }
    while (rows > 1 && (long long)rows * 2 * w > (long long)max_cols * h)
    {
        --rows;
    }
    return rows;
}

static void put_sequence(const char *seq);

static bool show_kitty(const unsigned char *rgb, int w, int h, int rows)
{
    int dw = 0, dh = 0;
    fit(w, h, SHOW_MAX_SIDE, &dw, &dh);
    unsigned char *small = picture_shrink(rgb, w, h, dw, dh);
    char *seq = small != NULL
        ? picture_kitty_sequence(small, dw, dh, rows)
        : NULL;
    free(small);
    if (seq == NULL)
    {
        return false;
    }
    put_sequence(seq);
    free(seq);
    return true;
}

char *picture_tmux_wrap(const char *seq)
{
    bytes out = { 0 };
    const char *p = seq;
    while (*p != '\0')
    {
        const char *st = strstr(p + 1, "\033\\");
        const char *bel = strchr(p, '\a');
        const char *end = st != NULL && (bel == NULL || st < bel)
            ? st + 2
            : bel != NULL
                ? bel + 1
                : p + strlen(p);
        bytes_str(&out, "\033Ptmux;");
        for (const char *c = p; c < end; ++c)
        {
            if (*c == '\033')
            {
                bytes_put(&out, "\033", 1);
            }
            bytes_put(&out, c, 1);
        }
        bytes_str(&out, "\033\\");
        p = end;
    }
    if (out.failed || out.data == NULL)
    {
        free(out.data);
        return NULL;
    }
    return (char *)out.data;
}

// tmux swallows picture sequences unless they come wrapped and the user has
// `set -g allow-passthrough on` (tmux >= 3.3).
static void put_sequence(const char *seq)
{
    char *wrapped = getenv("TMUX") != NULL
        ? picture_tmux_wrap(seq)
        : NULL;
    fputs(wrapped != NULL
        ? wrapped
        : seq, stdout);
    free(wrapped);
}

static void show_iterm(const char *jpeg_b64, size_t jpeg_len, int rows)
{
    char head[128];
    snprintf(head, sizeof head, "\033]1337;File=inline=1;size=%zu;height=%d;preserveAspectRatio=1:", jpeg_len, rows);
    bytes seq = { 0 };
    bytes_str(&seq, head);
    bytes_str(&seq, jpeg_b64);
    bytes_str(&seq, "\a");
    if (seq.failed == false)
    {
        put_sequence((const char *)seq.data);
    }
    free(seq.data);
}

bool picture_show(picture_protocol p, const char *jpeg_b64, int max_cols, int max_rows)
{
    if (p == PICTURES_OFF || jpeg_b64 == NULL)
    {
        return false;
    }
    size_t len = 0;
    unsigned char *jpeg = from_base64(jpeg_b64, &len);
    bool is_jpeg = jpeg != NULL && len > 3 && jpeg[0] == 0xff && jpeg[1] == 0xd8 && jpeg[2] == 0xff;
    int w = 0, h = 0, channels = 0;
    unsigned char *rgb = is_jpeg
        ? stbi_load_from_memory(jpeg, (int)len, &w, &h, &channels, 3)
        : NULL;
    free(jpeg);
    if (rgb == NULL)
    {
        return false;
    }
    int rows = rows_for(w, h, max_cols, max_rows);
    bool shown = true;
    if (p == PICTURES_KITTY)
    {
        shown = show_kitty(rgb, w, h, rows);
    }
    else
    {
        show_iterm(jpeg_b64, len, rows);
    }
    stbi_image_free(rgb);
    if (shown)
    {
        putchar('\n');
    }
    fflush(stdout);
    return shown;
}
