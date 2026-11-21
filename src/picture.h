#ifndef EUROS_PICTURE_H
#define EUROS_PICTURE_H

#include <cJSON.h>

#include <stdbool.h>
#include <stddef.h>

typedef enum
{
    PICTURES_OFF,
    PICTURES_KITTY, // kitty graphics protocol: kitty, Ghostty, WezTerm with enable_kitty_graphics
    PICTURES_ITERM, // iTerm2 inline images (OSC 1337): WezTerm, iTerm2
} picture_protocol;

// EUROS_IMAGES=kitty|iterm|off, else guessed from the terminal. Off when stdout is not a terminal.
picture_protocol picture_detect(void);

const char *picture_protocol_name(picture_protocol p);

// Shows a base64 JPEG (as sent in chat) at most max_cols × max_rows cells, followed by a newline.
// False if it is not a JPEG that decodes.
bool picture_show(picture_protocol p, const char *jpeg_b64, int max_cols, int max_rows);

// An image file (JPEG, PNG, GIF, BMP) → the chat's { "mime", "w", "h", "data" }: a JPEG that fits one
// message, shrunk step by step like the web client. NULL with the reason in err.
cJSON *picture_from_file(const char *path, char *err, size_t err_size);
cJSON *picture_from_memory(const unsigned char *bytes, size_t len, char *err, size_t err_size);

// Box-filter downscale of 3-channel pixels. Caller frees.
unsigned char *picture_shrink(const unsigned char *src, int sw, int sh, int dw, int dh);

// kitty graphics escape sequence for RGB pixels shown `rows` cells high (zlib, 4096-byte chunks,
// no replies from the terminal). Caller frees.
char *picture_kitty_sequence(const unsigned char *rgb, int w, int h, int rows);

#endif
