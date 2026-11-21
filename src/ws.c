#include "http.h"
#include "platform.h"
#include "ws.h"

#include <curl/curl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#  include <sys/select.h>
#endif

struct ws_conn
{
    CURL *curl;
    char *buf; // message being reassembled from fragments
    size_t len;
};

ws_conn *ws_connect(const char *url, char *err, size_t err_size)
{
    ws_conn *ws = calloc(1, sizeof *ws);
    if (ws == NULL)
    {
        snprintf(err, err_size, "out of memory");
        return NULL;
    }
    ws->curl = curl_easy_init();
    if (ws->curl == NULL)
    {
        snprintf(err, err_size, "curl init failed");
        free(ws);
        return NULL;
    }
    char errbuf[CURL_ERROR_SIZE] = "";
    curl_easy_setopt(ws->curl, CURLOPT_URL, url);
    curl_easy_setopt(ws->curl, CURLOPT_CONNECT_ONLY, 2L); // WebSocket upgrade, then hand the socket to us
    curl_easy_setopt(ws->curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(ws->curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(ws->curl, CURLOPT_USERAGENT, http_user_agent());
    http_setup_tls(ws->curl);

    CURLcode rc = curl_easy_perform(ws->curl);
    if (rc != CURLE_OK)
    {
        const char *reason = errbuf[0] != '\0'
            ? errbuf
            : curl_easy_strerror(rc);
        snprintf(err, err_size, "%s", reason);
        curl_easy_cleanup(ws->curl);
        free(ws);
        return NULL;
    }
    curl_easy_setopt(ws->curl, CURLOPT_ERRORBUFFER, NULL);
    return ws;
}

bool ws_send_text(ws_conn *ws, const char *text)
{
    size_t len = strlen(text);
    size_t off = 0;
    int idle = 0;
    while (off < len)
    {
        size_t sent = 0;
        CURLcode rc = curl_ws_send(ws->curl, text + off, len - off, &sent, 0, CURLWS_TEXT);
        if (rc == CURLE_AGAIN)
        {
            if (++idle > 500)
            {
                return false; // ~5 s without progress
            }
            platform_sleep_ms(10);
            continue;
        }
        if (rc != CURLE_OK)
        {
            return false;
        }
        off += sent;
        idle = 0;
    }
    return true;
}

int ws_next_message(ws_conn *ws, char **out)
{
    char chunk[16384];
    for (;;)
    {
        size_t got = 0;
        const struct curl_ws_frame *meta = NULL;
        CURLcode rc = curl_ws_recv(ws->curl, chunk, sizeof chunk, &got, &meta);
        if (rc == CURLE_AGAIN)
        {
            return 0;
        }
        if (rc != CURLE_OK || meta == NULL)
        {
            return -1;
        }
        if ((meta->flags & CURLWS_CLOSE) != 0)
        {
            return -1;
        }
        if ((meta->flags & (CURLWS_TEXT | CURLWS_CONT)) == 0)
        {
            continue; // binary or pong: not used
        }

        char *grown = realloc(ws->buf, ws->len + got + 1);
        if (grown == NULL)
        {
            return -1;
        }
        ws->buf = grown;
        memcpy(ws->buf + ws->len, chunk, got);
        ws->len += got;
        ws->buf[ws->len] = '\0';

        bool frame_done = meta->bytesleft == 0;
        bool message_done = frame_done && (meta->flags & CURLWS_CONT) == 0;
        if (message_done)
        {
            *out = ws->buf;
            ws->buf = NULL;
            ws->len = 0;
            return 1;
        }
    }
}

void ws_wait(ws_conn *ws, int timeout_ms)
{
    curl_socket_t sock = CURL_SOCKET_BAD;
    if (curl_easy_getinfo(ws->curl, CURLINFO_ACTIVESOCKET, &sock) != CURLE_OK || sock == CURL_SOCKET_BAD)
    {
        platform_sleep_ms(timeout_ms);
        return;
    }
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(sock, &readable);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    select((int)sock + 1, &readable, NULL, NULL, &tv);
}

void ws_close(ws_conn *ws)
{
    if (ws == NULL)
    {
        return;
    }
    size_t sent = 0;
    curl_ws_send(ws->curl, "", 0, &sent, 0, CURLWS_CLOSE);
    curl_easy_cleanup(ws->curl);
    free(ws->buf);
    free(ws);
}
