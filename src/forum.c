#include "api.h"
#include "cli.h"
#include "forum.h"
#include "markup.h"
#include "platform.h"
#include "session.h"
#include "term.h"

#include <cJSON.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define THREAD_COLUMNS "id,board_id,author_id,title,body,pinned,locked,expires_at,deleted_at,created_at,last_post_at"

static const char *USAGE =
    "euros forum                              boards\n"
    "euros forum <board>                      threads of a board (its number or name)\n"
    "euros forum read <thread>                a thread with its replies (thread: the short id)\n"
    "euros forum post <board> [--title T] [--ttl 1|7|30]\n"
    "                                         start a thread; it lives forever unless --ttl days\n"
    "euros forum reply <thread>               reply\n"
    "euros forum delete <thread> [n] [--yes]  delete the thread, or its reply n (yours; founder: any)\n"
    "euros forum pin|unpin|lock|unlock <thread>     founder\n"
    "euros forum new-board <name> [description]     founder\n"
    "\n"
    "Text comes from the keyboard (end with a line with a single dot) or a pipe.\n"
    "Markup: **bold** · > quote · links are clickable\n";

typedef struct
{
    euros_session session;
    cJSON *profiles;
    bool color;
} forum;

static int usage(void)
{
    fputs(USAGE, stderr);
    return 2;
}

void forum_burns_in(long long expires_s, long long now_s, char *out, size_t size)
{
    double left = (double)(expires_s - now_s);
    long hours = lround(left / 3600.0);
    if (hours < 1)
    {
        hours = 1;
    }
    if (hours < 48)
    {
        snprintf(out, size, "burns in %ldh", hours);
    }
    else
    {
        snprintf(out, size, "burns in %ldd", lround(left / (double)CLI_DAY_S));
    }
}

static void short_id(const char *uuid, char out[FORUM_SHORT_ID + 1])
{
    snprintf(out, FORUM_SHORT_ID + 1, "%s", uuid);
}

static void dim(const char *text)
{
    printf("%s%s%s", cli_sty("\033[2m"), text, cli_sty("\033[0m"));
}

static bool seen_path(char *out, size_t size)
{
    char dir[1024];
    if (platform_config_dir(dir, sizeof dir) == false)
    {
        return false;
    }
#ifdef _WIN32
    return (size_t)snprintf(out, size, "%s\\forum-seen.json", dir) < size;
#else
    return (size_t)snprintf(out, size, "%s/forum-seen.json", dir) < size;
#endif
}

// { "<thread id>": "<last_post_at seen>" }
static cJSON *seen_load(void)
{
    char path[1100];
    char *text = seen_path(path, sizeof path)
        ? platform_read_file(path, NULL)
        : NULL;
    cJSON *seen = cJSON_Parse(text);
    free(text);
    if (cJSON_IsObject(seen) == false)
    {
        cJSON_Delete(seen);
        seen = cJSON_CreateObject();
    }
    return seen;
}

static void seen_save(const cJSON *seen)
{
    char path[1100];
    char *text = cJSON_PrintUnformatted(seen);
    if (text != NULL && seen_path(path, sizeof path))
    {
        platform_write_private(path, text, strlen(text));
    }
    cJSON_free(text);
}

static void seen_mark(cJSON *seen, const char *thread_id, const char *last_post_at)
{
    if (thread_id == NULL || last_post_at == NULL)
    {
        return;
    }
    cJSON *value = cJSON_CreateString(last_post_at);
    if (cJSON_HasObjectItem(seen, thread_id))
    {
        cJSON_ReplaceItemInObjectCaseSensitive(seen, thread_id, value);
    }
    else
    {
        cJSON_AddItemToObject(seen, thread_id, value);
    }
}

static void seen_prune(cJSON *seen, const cJSON *threads)
{
    cJSON *entry = seen->child;
    while (entry != NULL)
    {
        cJSON *next = entry->next;
        bool alive = false;
        const cJSON *th;
        cJSON_ArrayForEach(th, threads)
        {
            const char *id = cli_str(th, "id");
            if (id != NULL && strcmp(id, entry->string) == 0)
            {
                alive = true;
                break;
            }
        }
        if (alive == false)
        {
            cJSON_Delete(cJSON_DetachItemViaPointer(seen, entry));
        }
        entry = next;
    }
}

static bool is_new(const cJSON *seen, const cJSON *thread, const char *me)
{
    const char *id = cli_str(thread, "id");
    const char *author = cli_str(thread, "author_id");
    const char *last = cli_str(thread, "last_post_at");
    if (id == NULL || author == NULL || last == NULL || strcmp(author, me) == 0)
    {
        return false;
    }
    const char *at = cli_str(seen, id);
    return at == NULL || strcmp(at, last) < 0;
}

static cJSON *select_rows(forum *f, const char *query)
{
    api_error err = { 0 };
    cJSON *rows = api_select(&f->session, query, &err);
    if (cJSON_IsArray(rows) == false)
    {
        cJSON_Delete(rows);
        api_fail(&err);
        return NULL;
    }
    return rows;
}

static cJSON *load_boards(forum *f)
{
    return select_rows(f, "boards?select=id,name,description&order=position,created_at");
}

static cJSON *load_thread_index(forum *f)
{
    return select_rows(f, "threads?select=id,board_id,author_id,last_post_at");
}

int forum_unread(euros_session *s)
{
    cJSON *threads = api_select(s, "threads?select=id,author_id,last_post_at", NULL);
    if (cJSON_IsArray(threads) == false)
    {
        cJSON_Delete(threads);
        return -1;
    }
    cJSON *seen = seen_load();
    int n = 0;
    const cJSON *th;
    cJSON_ArrayForEach(th, threads)
    {
        if (is_new(seen, th, s->user_id))
        {
            ++n;
        }
    }
    cJSON_Delete(seen);
    cJSON_Delete(threads);
    return n;
}

static const cJSON *find_board(const cJSON *boards, const char *arg)
{
    int index = 0;
    if (cli_parse_number(arg, &index))
    {
        const cJSON *board = cJSON_GetArrayItem(boards, index - 1);
        if (board != NULL)
        {
            return board;
        }
    }
    const cJSON *board;
    cJSON_ArrayForEach(board, boards)
    {
        const char *name = cli_str(board, "name");
        if (name != NULL && cli_same_text_ci(name, arg))
        {
            return board;
        }
    }
    fprintf(stderr, "No such board: %s (see `euros forum`)\n", arg);
    return NULL;
}

// Short id (or a longer prefix, or the full id) → the thread's id. False if none or ambiguous.
static bool find_thread(forum *f, const char *arg, char out[40])
{
    char prefix[40];
    size_t n = 0;
    for (const char *p = arg; *p != '\0' && n + 1 < sizeof prefix; ++p)
    {
        prefix[n++] = (char)tolower((unsigned char)*p);
    }
    prefix[n] = '\0';
    if (n < 4)
    {
        fprintf(stderr, "Thread ids are like a1b2c3 (see `euros forum <board>`)\n");
        return false;
    }
    cJSON *threads = select_rows(f, "threads?select=id");
    if (threads == NULL)
    {
        return false;
    }
    int matches = 0;
    const cJSON *th;
    cJSON_ArrayForEach(th, threads)
    {
        const char *id = cli_str(th, "id");
        if (id != NULL && strncmp(id, prefix, n) == 0)
        {
            snprintf(out, 40, "%s", id);
            ++matches;
        }
    }
    cJSON_Delete(threads);
    if (matches == 0)
    {
        fprintf(stderr, "No thread %s: it has burned, or the id is wrong\n", arg);
    }
    else if (matches > 1)
    {
        fprintf(stderr, "%s matches several threads: give more characters\n", arg);
    }
    return matches == 1;
}

static cJSON *load_thread(forum *f, const char *id)
{
    char query[256];
    snprintf(query, sizeof query, "threads?select=" THREAD_COLUMNS "&id=eq.%s", id);
    cJSON *rows = select_rows(f, query);
    bool loaded = rows != NULL;
    cJSON *thread = cJSON_DetachItemFromArray(rows, 0);
    cJSON_Delete(rows);
    if (thread == NULL && loaded)
    {
        fprintf(stderr, "This thread has burned.\n");
    }
    return thread;
}

static const char *board_name(const cJSON *boards, const char *board_id)
{
    const cJSON *board;
    cJSON_ArrayForEach(board, boards)
    {
        const char *id = cli_str(board, "id");
        if (id != NULL && board_id != NULL && strcmp(id, board_id) == 0)
        {
            return cli_str(board, "name");
        }
    }
    return "?";
}

static bool flag(const cJSON *obj, const char *key)
{
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(obj, key));
}

static bool deleted(const cJSON *obj)
{
    return cli_str(obj, "deleted_at") != NULL;
}

static bool is_founder(const forum *f)
{
    return strcmp(f->session.rank, "founder") == 0;
}

// " · burns in 3d" when the thread has a lifetime.
static void put_burn(const cJSON *thread, long long now)
{
    const char *expires = cli_str(thread, "expires_at");
    if (expires == NULL)
    {
        return;
    }
    char text[32];
    forum_burns_in(cli_parse_time(expires), now, text, sizeof text);
    printf("%s · %s%s", cli_sty("\033[2m"), cli_sty("\033[0;33m"), text);
    printf("%s", cli_sty("\033[0m"));
}

static void put_rendered(const forum *f, const char *text, const char *indent)
{
    char *out = markup_render(text, f->color, indent);
    if (out != NULL)
    {
        fputs(out, stdout);
        free(out);
    }
}

static int list_boards(forum *f)
{
    cJSON *boards = load_boards(f);
    cJSON *threads = boards != NULL
        ? load_thread_index(f)
        : NULL;
    if (threads == NULL)
    {
        cJSON_Delete(boards);
        return 1;
    }
    cJSON *seen = seen_load();
    seen_prune(seen, threads);
    seen_save(seen);

    if (cJSON_GetArraySize(boards) == 0)
    {
        printf("No boards yet.\n");
    }
    int n = 0;
    const cJSON *board;
    cJSON_ArrayForEach(board, boards)
    {
        const char *id = cli_str(board, "id");
        int count = 0;
        bool fresh = false;
        const cJSON *th;
        cJSON_ArrayForEach(th, threads)
        {
            const char *owner = cli_str(th, "board_id");
            if (id != NULL && owner != NULL && strcmp(id, owner) == 0)
            {
                ++count;
                fresh = fresh || is_new(seen, th, f->session.user_id);
            }
        }
        printf("%2d  %s", ++n, cli_sty("\033[1m"));
        term_put_clean(cli_str(board, "name"), " ");
        printf("%s", cli_sty("\033[0m"));
        const char *plural = count == 1
            ? ""
            : "s";
        char meta[48];
        snprintf(meta, sizeof meta, " · %d thread%s", count, plural);
        dim(meta);
        if (fresh)
        {
            printf(" %s•%s", cli_sty("\033[1;32m"), cli_sty("\033[0m"));
        }
        putchar('\n');
        const char *desc = cli_str(board, "description");
        if (desc != NULL && desc[0] != '\0')
        {
            printf("    %s", cli_sty("\033[2m"));
            term_put_clean(desc, " ");
            printf("%s\n", cli_sty("\033[0m"));
        }
    }
    if (n > 0)
    {
        fprintf(stderr, "Open one with `euros forum <number or name>`.\n");
    }
    cJSON_Delete(seen);
    cJSON_Delete(threads);
    cJSON_Delete(boards);
    return 0;
}

static int replies_of(const cJSON *thread)
{
    const cJSON *posts = cJSON_GetObjectItemCaseSensitive(thread, "posts");
    const cJSON *count = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(posts, 0), "count");
    return cJSON_IsNumber(count)
        ? (int)count->valuedouble
        : 0;
}

static void print_thread_row(forum *f, const cJSON *th, const cJSON *seen, long long now)
{
    char sid[FORUM_SHORT_ID + 1];
    short_id(cli_str(th, "id"), sid);
    printf("  %s%s%s  ", cli_sty("\033[36m"), sid, cli_sty("\033[0m"));
    if (flag(th, "pinned"))
    {
        printf("▲ ");
    }
    if (flag(th, "locked"))
    {
        printf("■ ");
    }
    if (deleted(th))
    {
        dim("deleted");
    }
    else
    {
        printf("%s", cli_sty("\033[1m"));
        term_put_clean(cli_str(th, "title"), " ");
        printf("%s", cli_sty("\033[0m"));
    }
    if (is_new(seen, th, f->session.user_id))
    {
        printf(" %s•%s", cli_sty("\033[1;32m"), cli_sty("\033[0m"));
    }
    printf("\n          ");
    cli_put_nick(cli_nick(f->profiles, cli_str(th, "author_id")));
    char when[32];
    cli_local_time(cli_str(th, "last_post_at"), when, sizeof when);
    int replies = replies_of(th);
    const char *ending = replies == 1
        ? "y"
        : "ies";
    char meta[96];
    snprintf(meta, sizeof meta, " · %d repl%s · %s", replies, ending, when);
    dim(meta);
    put_burn(th, now);
    putchar('\n');
}

static int list_threads(forum *f, const char *board_arg)
{
    cJSON *boards = load_boards(f);
    const cJSON *board = boards != NULL
        ? find_board(boards, board_arg)
        : NULL;
    if (board == NULL)
    {
        cJSON_Delete(boards);
        return 1;
    }
    char query[512];
    snprintf(query, sizeof query,
             "threads?select=id,author_id,title,pinned,locked,expires_at,deleted_at,last_post_at,posts(count)"
             "&board_id=eq.%s&order=pinned.desc,last_post_at.desc",
             cli_str(board, "id"));
    cJSON *threads = select_rows(f, query);
    if (threads == NULL)
    {
        cJSON_Delete(boards);
        return 1;
    }
    printf("%sForum ›%s ", cli_sty("\033[2m"), cli_sty("\033[0m"));
    term_put_clean(cli_str(board, "name"), " ");
    printf("\n\n");
    if (cJSON_GetArraySize(threads) == 0)
    {
        printf("No threads here yet.\n");
    }
    cJSON *seen = seen_load();
    long long now = (long long)time(NULL);
    const cJSON *th;
    cJSON_ArrayForEach(th, threads)
    {
        print_thread_row(f, th, seen, now);
    }
    putchar('\n');
    fprintf(stderr, "Read: `euros forum read <id>` · new thread: `euros forum post %s`\n", board_arg);
    cJSON_Delete(seen);
    cJSON_Delete(threads);
    cJSON_Delete(boards);
    return 0;
}

static void print_thread_head(forum *f, const cJSON *th, const cJSON *boards, long long now)
{
    char sid[FORUM_SHORT_ID + 1];
    short_id(cli_str(th, "id"), sid);
    printf("%sForum › ", cli_sty("\033[2m"));
    term_put_clean(board_name(boards, cli_str(th, "board_id")), " ");
    printf(" ›%s %s%s%s\n\n", cli_sty("\033[0m"), cli_sty("\033[36m"), sid, cli_sty("\033[0m"));

    if (deleted(th))
    {
        dim("deleted");
    }
    else
    {
        printf("%s", cli_sty("\033[1m"));
        term_put_clean(cli_str(th, "title"), " ");
        printf("%s", cli_sty("\033[0m"));
    }
    printf("\n");
    cli_put_nick(cli_nick(f->profiles, cli_str(th, "author_id")));
    char when[32];
    cli_local_time(cli_str(th, "created_at"), when, sizeof when);
    char meta[64];
    snprintf(meta, sizeof meta, " · %s", when);
    dim(meta);
    put_burn(th, now);
    if (flag(th, "pinned"))
    {
        dim(" · pinned");
    }
    if (flag(th, "locked"))
    {
        dim(" · locked");
    }
    printf("\n\n");
    if (deleted(th) == false)
    {
        put_rendered(f, cli_str(th, "body"), "  ");
        putchar('\n');
    }
}

static void print_post(forum *f, const cJSON *post, int n)
{
    printf("%s#%d%s ", cli_sty("\033[2m"), n, cli_sty("\033[0m"));
    cli_put_nick(cli_nick(f->profiles, cli_str(post, "author_id")));
    char when[32];
    cli_local_time(cli_str(post, "created_at"), when, sizeof when);
    char meta[64];
    snprintf(meta, sizeof meta, " · %s", when);
    dim(meta);
    putchar('\n');
    if (deleted(post))
    {
        printf("  ");
        dim("deleted");
        putchar('\n');
    }
    else
    {
        put_rendered(f, cli_str(post, "body"), "  ");
    }
    putchar('\n');
}

static cJSON *load_posts(forum *f, const char *thread_id)
{
    char query[256];
    snprintf(query, sizeof query, "posts?select=id,author_id,body,deleted_at,created_at&thread_id=eq.%s&order=id",
             thread_id);
    return select_rows(f, query);
}

static int read_thread(forum *f, const char *arg)
{
    char id[40];
    if (find_thread(f, arg, id) == false)
    {
        return 1;
    }
    cJSON *th = load_thread(f, id);
    cJSON *posts = th != NULL
        ? load_posts(f, id)
        : NULL;
    cJSON *boards = posts != NULL
        ? load_boards(f)
        : NULL;
    if (boards == NULL)
    {
        cJSON_Delete(posts);
        cJSON_Delete(th);
        return 1;
    }
    long long now = (long long)time(NULL);
    print_thread_head(f, th, boards, now);
    int n = 0;
    const cJSON *post;
    cJSON_ArrayForEach(post, posts)
    {
        print_post(f, post, ++n);
    }

    char sid[FORUM_SHORT_ID + 1];
    short_id(id, sid);
    if (deleted(th))
    {
        dim("The thread was deleted. No new replies.\n");
    }
    else if (flag(th, "locked") && is_founder(f) == false)
    {
        dim("The thread is locked.\n");
    }
    else
    {
        fprintf(stderr, "Reply: `euros forum reply %s`\n", sid);
    }

    cJSON *seen = seen_load();
    seen_mark(seen, id, cli_str(th, "last_post_at"));
    seen_save(seen);
    cJSON_Delete(seen);
    cJSON_Delete(boards);
    cJSON_Delete(posts);
    cJSON_Delete(th);
    return 0;
}

static void trim_copy(const char *s, char *out, size_t size)
{
    while (*s != '\0' && isspace((unsigned char)*s) != 0)
    {
        ++s;
    }
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1]) != 0)
    {
        --n;
    }
    if (n >= size)
    {
        n = size - 1;
    }
    memcpy(out, s, n);
    out[n] = '\0';
}

static bool valid_body(const char *body)
{
    if (body == NULL || cli_has_text(body) == false || cli_codepoints(body) > FORUM_MAX_BODY)
    {
        fprintf(stderr, "Not posted: the text is empty or longer than %d characters\n", FORUM_MAX_BODY);
        return false;
    }
    return true;
}

static bool read_title(const char *given, char *out, size_t size)
{
    char raw[1024] = "";
    if (given != NULL)
    {
        snprintf(raw, sizeof raw, "%s", given);
    }
    else if (term_stdin_is_tty())
    {
        fputs("title: ", stderr);
        fflush(stderr);
        platform_read_line(raw, sizeof raw);
    }
    else
    {
        fprintf(stderr, "Give the title with --title when the text comes from a pipe\n");
        return false;
    }
    trim_copy(raw, out, size);
    size_t len = cli_codepoints(out);
    if (len == 0 || len > FORUM_MAX_TITLE)
    {
        fprintf(stderr, "The title must be 1 to %d characters\n", FORUM_MAX_TITLE);
        return false;
    }
    return true;
}

static int post_thread(forum *f, int argc, char **argv)
{
    const char *board_arg = NULL;
    const char *title_arg = NULL;
    int ttl = 0;
    for (int i = 0; i < argc; ++i)
    {
        if (strcmp(argv[i], "--title") == 0 && i + 1 < argc)
        {
            title_arg = argv[++i];
        }
        else if (strcmp(argv[i], "--ttl") == 0 && i + 1 < argc)
        {
            if (cli_parse_number(argv[++i], &ttl) == false || (ttl != 1 && ttl != 7 && ttl != 30))
            {
                return usage();
            }
        }
        else if (board_arg == NULL && argv[i][0] != '-')
        {
            board_arg = argv[i];
        }
        else
        {
            return usage();
        }
    }
    if (board_arg == NULL)
    {
        return usage();
    }
    cJSON *boards = load_boards(f);
    const cJSON *board = boards != NULL
        ? find_board(boards, board_arg)
        : NULL;
    char title[1024];
    if (board == NULL || read_title(title_arg, title, sizeof title) == false)
    {
        cJSON_Delete(boards);
        return 1;
    }
    char intro[128];
    if (ttl > 0)
    {
        const char *plural = ttl == 1
            ? ""
            : "s";
        snprintf(intro, sizeof intro, "The thread burns in %d day%s with its replies.", ttl, plural);
    }
    else
    {
        snprintf(intro, sizeof intro, "The thread lives forever. Markup: **bold** · > quote · links.");
    }
    bool too_long = false;
    char *body = cli_read_text(intro, &too_long);
    int rc = 1;
    if (valid_body(body))
    {
        cJSON *row = cJSON_CreateObject();
        cJSON_AddStringToObject(row, "board_id", cli_str(board, "id"));
        cJSON_AddStringToObject(row, "title", title);
        cJSON_AddStringToObject(row, "body", body);
        if (ttl > 0)
        {
            char expires[32];
            time_t when = time(NULL) + (time_t)(ttl * CLI_DAY_S);
            strftime(expires, sizeof expires, "%Y-%m-%dT%H:%M:%SZ", gmtime(&when));
            cJSON_AddStringToObject(row, "expires_at", expires);
        }
        else
        {
            cJSON_AddNullToObject(row, "expires_at");
        }
        char *json = cJSON_PrintUnformatted(row);
        cJSON_Delete(row);
        api_error err = { 0 };
        cJSON *created = json != NULL
            ? api_insert_returning(&f->session, "threads", json, "id", &err)
            : NULL;
        cJSON_free(json);
        const char *id = cli_str(created, "id");
        if (id != NULL)
        {
            char sid[FORUM_SHORT_ID + 1];
            short_id(id, sid);
            printf("Posted: %s · `euros forum read %s`\n", sid, sid);
            rc = 0;
        }
        else
        {
            rc = api_fail(&err);
        }
        cJSON_Delete(created);
    }
    cli_free_text(body);
    cJSON_Delete(boards);
    return rc;
}

static int reply(forum *f, const char *arg)
{
    char id[40];
    if (find_thread(f, arg, id) == false)
    {
        return 1;
    }
    cJSON *th = load_thread(f, id);
    if (th == NULL)
    {
        return 1;
    }
    int rc = 1;
    if (deleted(th))
    {
        fprintf(stderr, "The thread was deleted. No new replies.\n");
    }
    else if (flag(th, "locked") && is_founder(f) == false)
    {
        fprintf(stderr, "The thread is locked.\n");
    }
    else
    {
        char intro[200];
        snprintf(intro, sizeof intro, "Reply to this thread. Markup: **bold** · > quote · links.");
        bool too_long = false;
        char *body = cli_read_text(intro, &too_long);
        if (valid_body(body))
        {
            cJSON *row = cJSON_CreateObject();
            cJSON_AddStringToObject(row, "thread_id", id);
            cJSON_AddStringToObject(row, "body", body);
            char *json = cJSON_PrintUnformatted(row);
            cJSON_Delete(row);
            api_error err = { 0 };
            bool ok = json != NULL && api_insert(&f->session, "posts", json, &err);
            cJSON_free(json);
            rc = ok
                ? 0
                : api_fail(&err);
            if (ok)
            {
                printf("Replied.\n");
            }
        }
        cli_free_text(body);
    }
    cJSON_Delete(th);
    return rc;
}

static bool confirm(bool yes)
{
    if (yes)
    {
        return true;
    }
    if (term_stdin_is_tty() == false)
    {
        fprintf(stderr, "Add --yes to delete without a terminal\n");
        return false;
    }
    fputs("Delete? The text will be wiped, a \"deleted\" mark stays. [y/N] ", stdout);
    fflush(stdout);
    char answer[16];
    if (platform_read_line(answer, sizeof answer) == false)
    {
        return false;
    }
    return strcmp(answer, "y") == 0 || strcmp(answer, "Y") == 0 || strcmp(answer, "yes") == 0;
}

static bool run_rpc(forum *f, const char *fn, cJSON *args)
{
    char *json = cJSON_PrintUnformatted(args);
    cJSON_Delete(args);
    api_error err = { 0 };
    cJSON *res = json != NULL
        ? api_rpc(&f->session, fn, json, &err)
        : NULL;
    cJSON_free(json);
    bool ok = res != NULL;
    cJSON_Delete(res);
    if (ok == false)
    {
        api_fail(&err);
    }
    return ok;
}

static bool may_delete(const forum *f, const cJSON *obj)
{
    const char *author = cli_str(obj, "author_id");
    bool mine = author != NULL && strcmp(author, f->session.user_id) == 0;
    if (mine || is_founder(f))
    {
        return true;
    }
    fprintf(stderr, "Only the author or the founder can delete it\n");
    return false;
}

static int delete_cmd(forum *f, int argc, char **argv)
{
    const char *thread_arg = NULL;
    int reply_n = 0;
    bool yes = false;
    for (int i = 0; i < argc; ++i)
    {
        if (strcmp(argv[i], "--yes") == 0 || strcmp(argv[i], "-y") == 0)
        {
            yes = true;
        }
        else if (thread_arg == NULL)
        {
            thread_arg = argv[i];
        }
        else if (reply_n == 0 && cli_parse_number(argv[i], &reply_n))
        {
            continue;
        }
        else
        {
            return usage();
        }
    }
    char id[40];
    if (thread_arg == NULL)
    {
        return usage();
    }
    if (find_thread(f, thread_arg, id) == false)
    {
        return 1;
    }
    cJSON *th = load_thread(f, id);
    if (th == NULL)
    {
        return 1;
    }
    int rc = 1;
    if (reply_n == 0)
    {
        if (deleted(th))
        {
            fprintf(stderr, "Already deleted\n");
        }
        else if (may_delete(f, th) && confirm(yes))
        {
            cJSON *args = cJSON_CreateObject();
            cJSON_AddStringToObject(args, "p_thread", id);
            if (run_rpc(f, "delete_thread", args))
            {
                printf("Deleted.\n");
                rc = 0;
            }
        }
    }
    else
    {
        cJSON *posts = load_posts(f, id);
        const cJSON *post = cJSON_GetArrayItem(posts, reply_n - 1);
        const cJSON *post_id = cJSON_GetObjectItemCaseSensitive(post, "id");
        if (posts != NULL && cJSON_IsNumber(post_id) == false)
        {
            fprintf(stderr, "No reply #%d in this thread\n", reply_n);
        }
        else if (post != NULL && deleted(post))
        {
            fprintf(stderr, "Already deleted\n");
        }
        else if (post != NULL && may_delete(f, post) && confirm(yes))
        {
            cJSON *args = cJSON_CreateObject();
            cJSON_AddNumberToObject(args, "p_post", post_id->valuedouble);
            if (run_rpc(f, "delete_post", args))
            {
                printf("Deleted.\n");
                rc = 0;
            }
        }
        cJSON_Delete(posts);
    }
    cJSON_Delete(th);
    return rc;
}

static int set_flags(forum *f, const char *what, const char *arg)
{
    if (is_founder(f) == false)
    {
        fprintf(stderr, "Only the founder can %s threads\n", what);
        return 1;
    }
    char id[40];
    if (find_thread(f, arg, id) == false)
    {
        return 1;
    }
    cJSON *th = load_thread(f, id);
    if (th == NULL)
    {
        return 1;
    }
    bool pinned = flag(th, "pinned");
    bool locked = flag(th, "locked");
    if (strcmp(what, "pin") == 0 || strcmp(what, "unpin") == 0)
    {
        pinned = strcmp(what, "pin") == 0;
    }
    else
    {
        locked = strcmp(what, "lock") == 0;
    }
    cJSON_Delete(th);
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "p_thread", id);
    cJSON_AddBoolToObject(args, "p_pinned", pinned);
    cJSON_AddBoolToObject(args, "p_locked", locked);
    if (run_rpc(f, "set_thread_flags", args) == false)
    {
        return 1;
    }
    bool pin_word = strcmp(what, "pin") == 0 || strcmp(what, "unpin") == 0;
    const char *ending = pin_word
        ? "ned"
        : "ed";
    printf("Done: %s%s.\n", what, ending);
    return 0;
}

static int new_board(forum *f, int argc, char **argv)
{
    if (is_founder(f) == false)
    {
        fprintf(stderr, "Only the founder can create boards\n");
        return 1;
    }
    if (argc < 1 || argc > 2)
    {
        return usage();
    }
    char name[256];
    trim_copy(argv[0], name, sizeof name);
    const char *desc = argc == 2
        ? argv[1]
        : "";
    size_t name_len = cli_codepoints(name);
    if (name_len == 0 || name_len > FORUM_MAX_BOARD_NAME || cli_codepoints(desc) > FORUM_MAX_BOARD_DESC)
    {
        fprintf(stderr, "Name: 1 to %d characters, description: up to %d\n", FORUM_MAX_BOARD_NAME,
                FORUM_MAX_BOARD_DESC);
        return 1;
    }
    cJSON *boards = load_boards(f);
    if (boards == NULL)
    {
        return 1;
    }
    cJSON *row = cJSON_CreateObject();
    cJSON_AddStringToObject(row, "name", name);
    cJSON_AddStringToObject(row, "description", desc);
    cJSON_AddNumberToObject(row, "position", cJSON_GetArraySize(boards));
    cJSON_Delete(boards);
    char *json = cJSON_PrintUnformatted(row);
    cJSON_Delete(row);
    api_error err = { 0 };
    bool ok = json != NULL && api_insert(&f->session, "boards", json, &err);
    cJSON_free(json);
    if (ok == false)
    {
        return api_fail(&err);
    }
    printf("Board created.\n");
    return 0;
}

static int dispatch(forum *f, int argc, char **argv)
{
    if (argc == 0)
    {
        return list_boards(f);
    }
    const char *cmd = argv[0];
    bool one_arg = argc == 2;
    if (strcmp(cmd, "read") == 0)
    {
        return one_arg
            ? read_thread(f, argv[1])
            : usage();
    }
    if (strcmp(cmd, "post") == 0)
    {
        return post_thread(f, argc - 1, argv + 1);
    }
    if (strcmp(cmd, "reply") == 0)
    {
        return one_arg
            ? reply(f, argv[1])
            : usage();
    }
    if (strcmp(cmd, "delete") == 0)
    {
        return delete_cmd(f, argc - 1, argv + 1);
    }
    if (strcmp(cmd, "pin") == 0 || strcmp(cmd, "unpin") == 0 || strcmp(cmd, "lock") == 0 || strcmp(cmd, "unlock") == 0)
    {
        return one_arg
            ? set_flags(f, cmd, argv[1])
            : usage();
    }
    if (strcmp(cmd, "new-board") == 0)
    {
        return new_board(f, argc - 1, argv + 1);
    }
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0)
    {
        fputs(USAGE, stdout);
        return 0;
    }
    return argc == 1
        ? list_threads(f, cmd)
        : usage();
}

int forum_run(int argc, char **argv)
{
    forum f = { 0 };
    f.color = term_stdout_is_tty();
    if (cli_open_session(&f.session) == false)
    {
        return 1;
    }
    api_error err = { 0 };
    f.profiles = cli_load_profiles(&f.session, &err);
    int rc = f.profiles != NULL
        ? dispatch(&f, argc, argv)
        : api_fail(&err);
    cJSON_Delete(f.profiles);
    session_wipe(&f.session);
    return rc;
}
