/*
 * P4-LMS - Push updates: Lyrion tells the display when something changes
 *
 * Part of main.c.  This file is #included by main.c (in order) and is not
 * compiled on its own, so it shares main.c's includes and static names.
 *
 * Lyrion's command-line interface (TCP port 9090) has a "listen" mode that
 * sends one line for every change on the server - new song, play / pause,
 * volume, seek, queue edits - for every player.  A line for the selected
 * player triggers an immediate status refresh; a line for another player
 * refreshes the room dots.  The normal status request and screen update code
 * is reused unchanged; only *when* it runs changes.
 *
 * While this connection is up the regular polling slows to every
 * LYRION_POLL_PUSH_MS as a safety net.  If the server's CLI can't be reached
 * (disabled, or password protected) the display simply keeps polling every
 * LYRION_POLL_MS, exactly as before.
 */
#ifdef ESP_PLATFORM
#include "lwip/netdb.h"
#else
#include <netdb.h>
#endif
#include <errno.h>

#define LMS_CLI_PORT          "9090"
/* lms_events_connected and LYRION_POLL_PUSH_MS live in 03_shared_state.c,
 * because the status task (09) uses them too. */

/* "aa%3Abb%3Acc%3Add%3Aee%3Aff" -> "aa:bb:cc:dd:ee:ff" */
static void lms_url_decode(const char *in, size_t in_len, char *out, size_t out_size)
{
    size_t o = 0;
    for (size_t i = 0; i < in_len && o + 1 < out_size; i++) {
        if (in[i] == '%' && i + 2 < in_len) {
            char hex[3] = { in[i + 1], in[i + 2], 0 };
            out[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
}

/* One line from the server: "<player id> <command> <args...>". */
static void lms_event_line(const char *line, size_t len)
{
    const char *sp = memchr(line, ' ', len);
    if (sp == NULL) return;

    char id[24];
    lms_url_decode(line, (size_t)(sp - line), id, sizeof(id));
    if (strchr(id, ':') == NULL) return;          /* not about a player (e.g. our own "listen 1") */

    const int cur = current_player_index;
    if (cur >= 0 && cur < lyrion_player_count && strcasecmp(id, lyrion_players[cur].id) == 0) {
        xEventGroupSetBits(lyrion_refresh_event, LYRION_REFRESH_BIT);
        return;
    }
    for (int i = 0; i < lyrion_player_count; i++) {
        if (strcasecmp(id, lyrion_players[i].id) == 0) {
            xEventGroupSetBits(lyrion_refresh_event, LYRION_HOUSE_BIT);
            return;
        }
    }
}

/* Host part of "http://host:port" */
static void lms_server_host(char *out, size_t out_size)
{
    const char *h = lyrion_server_base;
    if (strncmp(h, "http://", 7) == 0) h += 7;
    size_t n = 0;
    while (h[n] && h[n] != ':' && h[n] != '/' && n + 1 < out_size) {
        out[n] = h[n];
        n++;
    }
    out[n] = '\0';
}

static void lms_events_task(void *arg)
{
    (void)arg;
    int backoff_s = 5;
    static char buf[1024];

    while (1) {
        xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

        char host[64];
        lms_server_host(host, sizeof(host));

        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo *res = NULL;
        int sock = -1;
        if (getaddrinfo(host, LMS_CLI_PORT, &hints, &res) == 0 && res != NULL) {
            sock = socket(res->ai_family, res->ai_socktype, 0);
            if (sock >= 0 && connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
                close(sock);
                sock = -1;
            }
        }
        if (res != NULL) freeaddrinfo(res);

        if (sock < 0) {
            ESP_LOGW(TAG, "Push updates: can't reach %s:%s - polling every %d s instead",
                     host, LMS_CLI_PORT, LYRION_POLL_MS / 1000);
            vTaskDelay(pdMS_TO_TICKS(backoff_s * 1000));
            if (backoff_s < 60) backoff_s *= 2;
            continue;
        }

        struct timeval tv = { .tv_sec = 20, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        static const char listen_cmd[] = "listen 1\n";
        send(sock, listen_cmd, sizeof(listen_cmd) - 1, 0);

        size_t len = 0;
        int idle = 0;
        while (1) {
            const int n = recv(sock, buf + len, sizeof(buf) - 1 - len, 0);
            if (n > 0) {
                if (!lms_events_connected) {
                    lms_events_connected = true;
                    backoff_s = 5;
                    ESP_LOGI(TAG, "Push updates: listening to %s:%s", host, LMS_CLI_PORT);
                    /* Catch up on anything missed while not listening. */
                    xEventGroupSetBits(lyrion_refresh_event, LYRION_REFRESH_BIT | LYRION_HOUSE_BIT);
                }
                idle = 0;
                len += (size_t)n;
                size_t start = 0;
                for (size_t i = 0; i < len; i++) {
                    if (buf[i] == '\n') {
                        lms_event_line(buf + start, i - start);
                        start = i + 1;
                    }
                }
                memmove(buf, buf + start, len - start);
                len -= start;
                if (len >= sizeof(buf) - 1) len = 0;       /* absurdly long line: drop it */
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                /* Quiet for 20 s: ask for the version to check the link is
                 * alive.  Three quiet periods in a row (a minute) means it isn't. */
                static const char ping[] = "version ?\n";
                if (++idle >= 3 || send(sock, ping, sizeof(ping) - 1, 0) < 0) break;
            } else {
                break;                                        /* closed or failed */
            }
        }

        close(sock);
        if (lms_events_connected) {
            ESP_LOGW(TAG, "Push updates: connection lost - polling every %d s until it is back",
                     LYRION_POLL_MS / 1000);
        }
        lms_events_connected = false;
        vTaskDelay(pdMS_TO_TICKS(backoff_s * 1000));
        if (backoff_s < 60) backoff_s *= 2;
    }
}
