/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Vlad Patoka (VE3VPT) */
/**
 * @file console.h
 * @brief Command console on the USB Serial/JTAG port.
 *
 * The path that needs no WiFi, no browser and no HTTP client: plug in a USB
 * cable and open any serial terminal. Same settings and rules as the HTTP
 * API (config_set_kv() in web_server.c). Type "help" for the command list.
 */

#ifndef CONSOLE_H
#define CONSOLE_H

#include "esp_err.h"

/** @brief Start the REPL task. Not fatal on failure: acquisition runs without it. */
esp_err_t console_start(void);

#endif /* CONSOLE_H */
