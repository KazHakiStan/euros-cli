#include "platform.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <direct.h>
#  include <io.h>
#  include <windows.h>
#  include <shellapi.h>
#else
#  include <fcntl.h>
#  include <sys/file.h>
#  include <sys/stat.h>
#  include <termios.h>
#  include <unistd.h>
#endif

void platform_init(void)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    // Let ANSI escape sequences through (colors, cursor control).
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(out, &mode) != 0)
    {
        SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
#endif
}

static bool make_dir(const char *path)
{
#ifdef _WIN32
    return _mkdir(path) == 0 || errno == EEXIST;
#else
    return mkdir(path, 0700) == 0 || errno == EEXIST;
#endif
}

static bool is_set(const char *value)
{
    return value != NULL && *value != '\0';
}

bool platform_config_dir(char *out, size_t size)
{
#ifdef _WIN32
    const char *base = getenv("APPDATA");
    if (is_set(base) == false)
    {
        return false;
    }
    if ((size_t)snprintf(out, size, "%s\\euros", base) >= size)
    {
        return false;
    }
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    int n;
    if (is_set(xdg))
    {
        n = snprintf(out, size, "%s/euros", xdg);
    }
    else if (is_set(home))
    {
        char parent[1024];
        snprintf(parent, sizeof parent, "%s/.config", home);
        make_dir(parent);
        n = snprintf(out, size, "%s/.config/euros", home);
    }
    else
    {
        return false;
    }
    if (n < 0 || (size_t)n >= size)
    {
        return false;
    }
#endif
    return make_dir(out);
}

static void strip_newline(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'))
    {
        s[--n] = '\0';
    }
}

bool platform_read_secret(const char *prompt, char *out, size_t size)
{
    fputs(prompt, stderr);
    fflush(stderr);
    bool ok;
#ifdef _WIN32
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    bool console = GetConsoleMode(in, &mode) != 0;
    if (console)
    {
        SetConsoleMode(in, mode & ~ENABLE_ECHO_INPUT);
    }
    ok = fgets(out, (int)size, stdin) != NULL;
    if (console)
    {
        SetConsoleMode(in, mode);
    }
#else
    struct termios old, quiet;
    bool tty = isatty(STDIN_FILENO) != 0 && tcgetattr(STDIN_FILENO, &old) == 0;
    if (tty)
    {
        quiet = old;
        quiet.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
    }
    ok = fgets(out, (int)size, stdin) != NULL;
    if (tty)
    {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
    }
#endif
    fputc('\n', stderr);
    if (ok)
    {
        strip_newline(out);
    }
    return ok;
}

bool platform_write_private(const char *path, const char *data, size_t len)
{
    char tmp[1100];
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) >= sizeof tmp)
    {
        return false;
    }
#ifdef _WIN32
    // %APPDATA% is already per-user (ACL); just write and swap.
    FILE *f = fopen(tmp, "wb");
    if (f == NULL)
    {
        return false;
    }
    bool ok = fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    if (ok == false)
    {
        remove(tmp);
        return false;
    }
    return MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
    {
        return false;
    }
    bool ok = write(fd, data, len) == (ssize_t)len;
    ok = (close(fd) == 0) && ok;
    if (ok == false)
    {
        unlink(tmp);
        return false;
    }
    return rename(tmp, path) == 0;
#endif
}

intptr_t platform_lock(const char *path)
{
    for (int attempt = 0; attempt < 50; ++attempt)
    {
#ifdef _WIN32
        // No sharing: a second opener fails until the first closes the handle.
        HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE)
        {
            return (intptr_t)h;
        }
#else
        int fd = open(path, O_RDWR | O_CREAT, 0600);
        if (fd < 0)
        {
            return -1;
        }
        if (flock(fd, LOCK_EX | LOCK_NB) == 0)
        {
            return fd;
        }
        close(fd);
#endif
        platform_sleep_ms(20);
    }
    return -1;
}

void platform_unlock(intptr_t lock)
{
    if (lock == -1)
    {
        return;
    }
#ifdef _WIN32
    CloseHandle((HANDLE)lock);
#else
    flock((int)lock, LOCK_UN);
    close((int)lock);
#endif
}

static char *read_stream(FILE *f, size_t *len_out);

char *platform_read_file(const char *path, size_t *len_out)
{
    return read_stream(fopen(path, "rb"), len_out);
}

char *platform_read_user_file(const char *utf8_path, size_t *len_out)
{
#ifdef _WIN32
    wchar_t wide[4096];
    if (MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, wide, (int)(sizeof wide / sizeof *wide)) == 0)
    {
        return NULL;
    }
    return read_stream(_wfopen(wide, L"rb"), len_out);
#else
    return read_stream(fopen(utf8_path, "rb"), len_out);
#endif
}

void platform_utf8_args(int *argc, char ***argv)
{
#ifdef _WIN32
    int n = 0;
    LPWSTR *wide = CommandLineToArgvW(GetCommandLineW(), &n);
    if (wide == NULL)
    {
        return;
    }
    char **utf8 = calloc((size_t)n + 1, sizeof *utf8);
    bool ok = utf8 != NULL;
    for (int i = 0; ok && i < n; ++i)
    {
        int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, NULL, 0, NULL, NULL);
        utf8[i] = size > 0
            ? malloc((size_t)size)
            : NULL;
        ok = utf8[i] != NULL && WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, utf8[i], size, NULL, NULL) != 0;
    }
    LocalFree(wide);
    if (ok)
    {
        *argc = n;
        *argv = utf8;
    }
#else
    (void)argc;
    (void)argv;
#endif
}

static char *read_stream(FILE *f, size_t *len_out)
{
    if (f == NULL)
    {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)n + 1);
    if (buf == NULL)
    {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (len_out != NULL)
    {
        *len_out = got;
    }
    return buf;
}

bool platform_remove(const char *path)
{
    return remove(path) == 0 || errno == ENOENT;
}

bool platform_read_line(char *out, size_t size)
{
#ifdef _WIN32
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(in, &mode) != 0)
    {
        // fgets() on a UTF-8 console turns non-ASCII into zeros; read UTF-16 and convert instead.
        // A long line arrives in several chunks: keep reading until its newline.
        size_t used = 0;
        out[0] = '\0';
        for (;;)
        {
            wchar_t wide[1024];
            DWORD got = 0;
            if (ReadConsoleW(in, wide, (DWORD)(sizeof wide / sizeof *wide) - 1, &got, NULL) == 0 || got == 0)
            {
                return used > 0;
            }
            wide[got] = L'\0';
            int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out + used, (int)(size - used), NULL, NULL);
            if (n == 0)
            {
                out[used] = '\0'; // out is full: keep what fits
                break;
            }
            used += (size_t)n - 1;
            if (wide[got - 1] == L'\n' || used + 1 >= size)
            {
                break;
            }
        }
        strip_newline(out);
        return true;
    }
#endif
    if (fgets(out, (int)size, stdin) == NULL)
    {
        return false;
    }
    strip_newline(out);
    return true;
}

long long platform_now_ms(void)
{
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER t;
    t.LowPart = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    // FILETIME counts 100 ns ticks since 1601-01-01.
    return (long long)(t.QuadPart / 10000ULL) - 11644473600000LL;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

_Noreturn void platform_exit_now(int code)
{
    fflush(stdout);
    fflush(stderr);
    _Exit(code);
}

void platform_sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}
