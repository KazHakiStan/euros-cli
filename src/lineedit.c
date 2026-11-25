#include "lineedit.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <sys/ioctl.h>
#  include <termios.h>
#  include <unistd.h>
#endif

#define LINE_BYTES 8192

static char line[LINE_BYTES];
static size_t line_len = 0;
static char prompt[128] = "> ";
static bool active = false;

// Full screen: the alternate screen with output scrolling above a rule and the input on the last row.
// The output position is kept with save/restore cursor while the cursor sits on the input row.
static bool fullscreen = false;
static bool in_input = false;
static int fs_rows = 0;
static int fs_cols = 0;
static char fs_title[128] = "";

#ifdef _WIN32
static HANDLE in_handle;
static DWORD saved_mode;
static wchar_t pending_high = 0; // first half of a UTF-16 surrogate pair
#else
static struct termios saved_termios;
static int escape_state = 0; // 0: none, 1: got ESC, 2: inside "ESC [" sequence
#endif

static int terminal_width(void)
{
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info) != 0)
    {
        return info.srWindow.Right - info.srWindow.Left + 1;
    }
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
    {
        return ws.ws_col;
    }
#endif
    return 80;
}

static int terminal_rows(void)
{
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info) != 0)
    {
        return info.srWindow.Bottom - info.srWindow.Top + 1;
    }
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
    {
        return ws.ws_row;
    }
#endif
    return 24;
}

static uint32_t decode(const unsigned char **p)
{
    uint32_t cp = **p;
    int extra = 0;
    if (cp >= 0xf0)
    {
        cp &= 0x07u;
        extra = 3;
    }
    else if (cp >= 0xe0)
    {
        cp &= 0x0fu;
        extra = 2;
    }
    else if (cp >= 0xc0)
    {
        cp &= 0x1fu;
        extra = 1;
    }
    ++*p;
    for (int i = 0; i < extra && (**p & 0xc0) == 0x80; ++i, ++*p)
    {
        cp = (cp << 6) | (**p & 0x3fu);
    }
    return cp;
}

// Columns a code point takes on screen: combining marks 0, East Asian wide and emoji 2.
static int columns(uint32_t cp)
{
    if (cp >= 0x300 && cp <= 0x36f)
    {
        return 0;
    }
    bool wide = (cp >= 0x1100 && cp <= 0x115f) || (cp >= 0x2e80 && cp <= 0xa4cf) || (cp >= 0xac00 && cp <= 0xd7a3)
        || (cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0xfe30 && cp <= 0xfe4f) || (cp >= 0xff00 && cp <= 0xff60)
        || (cp >= 0xffe0 && cp <= 0xffe6) || (cp >= 0x1f300 && cp <= 0x1faff) || (cp >= 0x20000 && cp <= 0x3fffd);
    return wide
        ? 2
        : 1;
}

static int text_columns(const char *s)
{
    int n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p != '\0')
    {
        n += columns(decode(&p));
    }
    return n;
}

// The tail of the line that fits in `room` columns.
static const char *visible_tail(int room)
{
    const char *start = line + line_len;
    int used = 0;
    while (start > line)
    {
        const char *prev = start - 1;
        while (prev > line && ((unsigned char)*prev & 0xc0) == 0x80)
        {
            --prev;
        }
        const unsigned char *p = (const unsigned char *)prev;
        int w = columns(decode(&p));
        if (used + w > room)
        {
            break;
        }
        used += w;
        start = prev;
    }
    return start;
}

static bool command_mode(void)
{
    return line[0] == ':';
}

static void to_input(void)
{
    if (in_input == false)
    {
        printf("\0337");
        in_input = true;
    }
    printf("\033[%d;1H", fs_rows);
}

static void to_output(void)
{
    if (in_input)
    {
        printf("\0338");
        in_input = false;
    }
}

// Scroll region above the rule; the cursor goes to the bottom of it, where output continues.
static void layout(void)
{
    fs_rows = terminal_rows();
    fs_cols = terminal_width();
    in_input = false;
    int bottom = fs_rows > 3
        ? fs_rows - 2
        : 1;
    printf("\033[1;%dr\033[%d;1H\033[2K\033[2m", bottom, bottom + 1);
    int used = 0;
    if (fs_title[0] != '\0')
    {
        printf("── %s ", fs_title);
        used = 4 + text_columns(fs_title);
    }
    for (int i = used; i < fs_cols; ++i)
    {
        fputs("─", stdout);
    }
    printf("\033[0m\033[%d;1H", bottom);
}

static void redraw(void)
{
    // ":" first: a vim-like command line, without the prompt.
    const char *shown_prompt = command_mode()
        ? ""
        : prompt;
    int room = terminal_width() - text_columns(shown_prompt) - 1;
    if (room < 1)
    {
        room = 1;
    }
    if (fullscreen)
    {
        to_input();
        printf("\033[K%s%s", shown_prompt, visible_tail(room));
    }
    else
    {
        printf("\r\033[K%s%s", shown_prompt, visible_tail(room));
    }
    fflush(stdout);
}

static void append_utf8(uint32_t cp)
{
    char buf[4];
    size_t n;
    if (cp < 0x80)
    {
        buf[0] = (char)cp;
        n = 1;
    }
    else if (cp < 0x800)
    {
        buf[0] = (char)(0xc0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3f));
        n = 2;
    }
    else if (cp < 0x10000)
    {
        buf[0] = (char)(0xe0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        buf[2] = (char)(0x80 | (cp & 0x3f));
        n = 3;
    }
    else
    {
        buf[0] = (char)(0xf0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
        buf[3] = (char)(0x80 | (cp & 0x3f));
        n = 4;
    }
    if (line_len + n < LINE_BYTES)
    {
        memcpy(line + line_len, buf, n);
        line_len += n;
        line[line_len] = '\0';
    }
}

static void erase_last(void)
{
    while (line_len > 0)
    {
        --line_len;
        if (((unsigned char)line[line_len] & 0xc0) != 0x80)
        {
            break; // removed a whole code point
        }
    }
    line[line_len] = '\0';
}

static le_result finish_line(char *out, size_t size)
{
    snprintf(out, size, "%s", line);
    line_len = 0;
    line[0] = '\0';
    if (fullscreen)
    {
        to_input();
        printf("\033[K");
    }
    else
    {
        printf("\r\033[K");
    }
    fflush(stdout);
    return LE_LINE;
}

// Control keys shared by both platforms. Returns LE_NONE if the key was something else.
static le_result control_key(uint32_t key, char *out, size_t size, bool *handled)
{
    *handled = true;
    switch (key)
    {
    case '\r':
    case '\n':
        return finish_line(out, size);
    case 3: // Ctrl+C
        return LE_QUIT;
    case 4: // Ctrl+D: leave on an empty line
        if (line_len == 0)
        {
            return LE_QUIT;
        }
        return LE_NONE;
    case 8:
    case 0x7f:
        erase_last();
        return LE_NONE;
    case 0x15: // Ctrl+U
        line_len = 0;
        line[0] = '\0';
        return LE_NONE;
    case 0x1b: // Esc leaves the command line
        if (command_mode())
        {
            line_len = 0;
            line[0] = '\0';
        }
        return LE_NONE;
    default:
        *handled = false;
        return LE_NONE;
    }
}

bool le_start(void)
{
#ifdef _WIN32
    in_handle = GetStdHandle(STD_INPUT_HANDLE);
    if (GetConsoleMode(in_handle, &saved_mode) == 0)
    {
        return false;
    }
    // No line buffering, no echo, Ctrl+C arrives as a key.
    SetConsoleMode(in_handle, ENABLE_WINDOW_INPUT);
#else
    if (isatty(STDIN_FILENO) == 0 || tcgetattr(STDIN_FILENO, &saved_termios) != 0)
    {
        return false;
    }
    struct termios raw = saved_termios;
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= ~(tcflag_t)(IXON | ICRNL);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
#endif
    active = true;
    redraw();
    return true;
}

void le_stop(void)
{
    if (active == false)
    {
        return;
    }
    le_set_fullscreen(false, NULL);
    active = false;
    printf("\r\033[K");
    fflush(stdout);
#ifdef _WIN32
    SetConsoleMode(in_handle, saved_mode);
#else
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
#endif
}

void le_set_prompt(const char *text)
{
    snprintf(prompt, sizeof prompt, "%s", text);
    if (active)
    {
        redraw();
    }
}

bool le_fullscreen(void)
{
    return fullscreen;
}

void le_set_fullscreen(bool on, const char *title)
{
    if (active == false || on == fullscreen)
    {
        return;
    }
    if (on)
    {
        printf("\r\033[K\033[?1049h\033[2J");
        snprintf(fs_title, sizeof fs_title, "%s", title != NULL ? title : "");
        fullscreen = true;
        layout();
    }
    else
    {
        to_output();
        printf("\033[r\033[?1049l");
        fullscreen = false;
    }
    redraw();
}

le_result le_poll(char *out, size_t size)
{
    bool changed = false;
    le_result result = LE_NONE;
    if (fullscreen && (terminal_rows() != fs_rows || terminal_width() != fs_cols))
    {
        to_output();
        printf("\033[2J"); // a resized window: start over at the bottom of the new region
        layout();
        changed = true;
    }
#ifdef _WIN32
    DWORD waiting = 0;
    while (result == LE_NONE && GetNumberOfConsoleInputEvents(in_handle, &waiting) != 0 && waiting > 0)
    {
        INPUT_RECORD rec;
        DWORD got = 0;
        if (ReadConsoleInputW(in_handle, &rec, 1, &got) == 0 || got == 0)
        {
            break;
        }
        if (rec.EventType != KEY_EVENT || rec.Event.KeyEvent.bKeyDown == FALSE)
        {
            continue;
        }
        wchar_t ch = rec.Event.KeyEvent.uChar.UnicodeChar;
        WORD vk = rec.Event.KeyEvent.wVirtualKeyCode;
        uint32_t key = vk == VK_RETURN
            ? '\r'
            : (uint32_t)ch;
        if (key == 0)
        {
            continue; // arrows, function keys, modifiers
        }
        bool handled = false;
        result = control_key(key, out, size, &handled);
        changed = true;
        if (handled || key < 0x20)
        {
            continue;
        }
        if (ch >= 0xd800 && ch <= 0xdbff)
        {
            pending_high = ch;
            continue;
        }
        if (ch >= 0xdc00 && ch <= 0xdfff)
        {
            if (pending_high != 0)
            {
                append_utf8(0x10000 + (((uint32_t)pending_high - 0xd800) << 10) + ((uint32_t)ch - 0xdc00));
            }
            pending_high = 0;
            continue;
        }
        append_utf8(ch);
    }
#else
    unsigned char buf[256];
    ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
    for (ssize_t i = 0; i < n && result == LE_NONE; ++i)
    {
        unsigned char c = buf[i];
        changed = true;
        if (escape_state == 1)
        {
            escape_state = c == '['
                ? 2
                : 0;
            continue;
        }
        if (escape_state == 2)
        {
            if (c >= 0x40 && c <= 0x7e)
            {
                escape_state = 0; // end of an arrow / function key sequence
            }
            continue;
        }
        if (c == 0x1b)
        {
            escape_state = 1;
            continue;
        }
        bool handled = false;
        result = control_key(c, out, size, &handled);
        if (handled || c < 0x20)
        {
            continue;
        }
        if (line_len + 1 < LINE_BYTES)
        {
            line[line_len++] = (char)c; // UTF-8 bytes go in as they come
            line[line_len] = '\0';
        }
    }
    if (n > 0 && escape_state == 1 && buf[n - 1] == 0x1b)
    {
        escape_state = 0; // nothing came after ESC: the Esc key itself
        bool handled = false;
        control_key(0x1b, out, size, &handled);
    }
#endif
    if (changed && result == LE_NONE)
    {
        redraw();
    }
    return result;
}

void le_hide(void)
{
    if (active && fullscreen)
    {
        to_output();
    }
    else if (active)
    {
        printf("\r\033[K");
    }
}

void le_show(void)
{
    if (active)
    {
        redraw();
    }
}
