/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Vlad Patoka (VE3VPT) */
/**
 * @file event_log.h
 * @brief RAM ring of recent console log lines, readable from the web UI.
 *
 * event_log_init() installs the one esp_log_set_vprintf() hook. Every log line
 * is copied into the ring (ANSI colors stripped) and then forwarded untouched
 * to the original console handler. Start it first in app_main() so the reset
 * reason, config load and the whole WiFi bring-up land in the ring.
 */

#ifndef EVENT_LOG_H
#define EVENT_LOG_H

#include <stddef.h>
#include <stdint.h>

/** @brief Install the capture hook. Idempotent. */
void event_log_init(void);

/**
 * @brief Write the ring as a JSON array of strings, oldest first.
 * @return Bytes written (excluding NUL), or 0 on bad arguments.
 */
int event_log_get_recent_json(char *out, size_t outlen);

/** @brief Number of log lines seen since boot (including ones that rolled out). */
uint32_t event_log_total_lines(void);

#endif /* EVENT_LOG_H */
