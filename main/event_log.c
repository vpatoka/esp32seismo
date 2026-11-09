/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Vlad Patoka (VE3VPT) */
/**
 * @file event_log.c
 * @brief RAM ring of recent console log lines. See event_log.h.
 *
 * The hook runs in whatever task logged. It formats into a stack buffer, then
 * holds the spinlock only for one memcpy. JSON formatting for the web UI works
 * on a snapshot taken under the lock, never while holding it.
 */

#include "event_log.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define LOG_RING_LINES    40
#define LOG_RING_LINELEN  120

static char s_ring[LOG_RING_LINES][LOG_RING_LINELEN];
static int  s_head;                 /* next slot to write */
static int  s_count;                /* valid lines, <= LOG_RING_LINES */
static uint32_t s_total;
static bool s_installed;
static portMUX_TYPE   s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_orig_vprintf;

/* Snapshot buffer for the JSON dump. Static to keep it off the httpd stack;
 * guarded by s_snap_mux because two browser tabs can poll at once. */
static char s_snap[LOG_RING_LINES][LOG_RING_LINELEN];
static portMUX_TYPE s_snap_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_snap_busy;

static int log_hook(const char *fmt, va_list args)
{
    char line[LOG_RING_LINELEN];
    va_list copy;
    va_copy(copy, args);
    vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);

    /* Keep printable ASCII only, drop "\x1b[...m" color sequences. */
    char clean[LOG_RING_LINELEN];
    int ci = 0;
    for (int i = 0; line[i] && ci < (int)sizeof(clean) - 1; i++) {
        unsigned char c = (unsigned char)line[i];
        if (c == 0x1b) {
            while (line[i] && line[i] != 'm') i++;
            if (!line[i]) break;
            continue;
        }
        if (c >= 0x20 && c < 0x7f) clean[ci++] = (char)c;
    }
    clean[ci] = '\0';

    if (ci > 0) {
        portENTER_CRITICAL(&s_mux);
        memcpy(s_ring[s_head], clean, (size_t)ci + 1);
        s_head = (s_head + 1) % LOG_RING_LINES;
        if (s_count < LOG_RING_LINES) s_count++;
        s_total++;
        portEXIT_CRITICAL(&s_mux);
    }

    int ret = ci;
    if (s_orig_vprintf) {
        va_list a;
        va_copy(a, args);
        ret = s_orig_vprintf(fmt, a);
        va_end(a);
    }
    return ret;
}

void event_log_init(void)
{
    if (s_installed) return;
    s_installed = true;
    s_head = 0;
    s_count = 0;
    s_orig_vprintf = esp_log_set_vprintf(log_hook);
}

uint32_t event_log_total_lines(void)
{
    return s_total;
}

int event_log_get_recent_json(char *out, size_t outlen)
{
    if (!out || outlen < 3) {
        if (out && outlen) out[0] = '\0';
        return 0;
    }

    /* One reader at a time owns the snapshot; a second caller gets "[]"
     * rather than blocking an httpd worker. */
    portENTER_CRITICAL(&s_snap_mux);
    bool busy = s_snap_busy;
    s_snap_busy = true;
    portEXIT_CRITICAL(&s_snap_mux);
    if (busy) {
        out[0] = '['; out[1] = ']'; out[2] = '\0';
        return 2;
    }

    int count, head;
    portENTER_CRITICAL(&s_mux);
    count = s_count;
    head  = s_head;
    memcpy(s_snap, s_ring, sizeof(s_snap));
    portEXIT_CRITICAL(&s_mux);

    size_t pos = 0;
    out[pos++] = '[';
    int start = (count < LOG_RING_LINES) ? 0 : head;
    for (int n = 0; n < count; n++) {
        const char *s = s_snap[(start + n) % LOG_RING_LINES];
        /* Worst case per line: comma, two quotes, every char escaped. */
        if (pos + 4 + 2 * strlen(s) + 2 >= outlen) break;
        if (n) out[pos++] = ',';
        out[pos++] = '"';
        for (int i = 0; s[i]; i++) {
            if (s[i] == '"' || s[i] == '\\') out[pos++] = '\\';
            out[pos++] = s[i];
        }
        out[pos++] = '"';
    }
    out[pos++] = ']';
    out[pos] = '\0';

    s_snap_busy = false;
    return (int)pos;
}
