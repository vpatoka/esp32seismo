/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Vlad Patoka (VE3VPT) */
/**
 * @file web_server.h
 * @brief Persistent configuration and the HTTP server (normal and portal mode).
 */

#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "acq.h"

#define NVS_NAMESPACE          "seismo"
#define NVS_KEY_CONFIG         "config"
#define SEISMO_CONFIG_VERSION  2       /* 2: capture */

/* HTTP API contract version, reported as "api" in /api/status, /api/config,
 * /api/acq and /api/fw/info. Additive changes only. See doc/API.md. */
#define SEISMO_API_VERSION     1

typedef struct {
    char     name[8];        /* component label: Z, N, E, H ... */
    uint8_t  pga;            /* ADC PGA gain 1..128 */
    int16_t  phase;          /* ADC phase trim, modulator clocks, -512..511 */
    float    gcal;           /* ADC gain calibration, 0.5..1.5 */
    int32_t  ocal;           /* ADC offset calibration, counts */
    float    divider;        /* ADC volts per EQAMP output volt (0.0990 for 9.1k/1.0k) */
    float    amp_gain;       /* EQAMP gain set on the trim pot; 0 = not known */
    float    sens;           /* geophone V/(m/s) incl. damping shunt; 0 = not known */
} seismo_ch_cfg_t;

/* Stored as one NVS blob. Append new fields at the end and bump the version;
 * config_load() keeps the common prefix of an older blob. */
typedef struct {
    uint32_t version;
    uint8_t  wifi_configured;
    char     wifi_ssid[33];
    char     wifi_password[65];
    char     hostname[32];
    uint16_t http_port;
    uint8_t  weblogin_enabled;
    char     username[16];
    char     password[24];

    char     station[6];     /* station code, for the record headers later */
    char     network[3];     /* network code */
    uint8_t  clk_src;        /* acq_clk_src_t */
    uint32_t clk_hz;         /* nominal CLKIN */
    uint16_t osr;
    uint8_t  decim;
    uint8_t  taps;           /* 0 = automatic */
    uint8_t  ref;            /* tb_ref_t */
    uint8_t  ref_rising;
    uint8_t  rtc;            /* DS323x fitted */
    seismo_ch_cfg_t ch[ACQ_NCH];
    /* v2 */
    uint8_t  capture;        /* acq_capture_t: edge timing, isr or mcpwm */
} seismo_config_t;

extern seismo_config_t g_config;

typedef enum {
    SEISMO_WIFI_NONE = 0,
    SEISMO_WIFI_STA,
    SEISMO_WIFI_AP_PROVISIONING,
} seismo_wifi_mode_t;

/* Owned by vseismo.c */
extern volatile seismo_wifi_mode_t g_wifi_mode;
extern volatile bool g_wifi_connected;
extern char g_ip_address[16];

/* Hardware and runtime health, for the banner, the console and
 * /api/status. Internal RAM and PSRAM are reported apart: WiFi, lwIP,
 * stacks and the sample path live in internal RAM, and that is the pool
 * that runs out. Stack figures are the least free space seen, in bytes
 * (0 = task not running). */
typedef struct {
    uint32_t int_free, int_min, int_largest, int_total;
    uint32_t psram_free, psram_min, psram_largest, psram_total;
    uint32_t cpu_mhz;
    uint16_t chip_rev;              /* major * 100 + minor */
    uint8_t  cores;
    uint8_t  mac[6];
    const char *reset_reason;
    bool     wifi_up;
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  bssid[6];
    const char *phy;                /* "11ax", "11ac", "11n", "11g", "11b", "" */
    float    tx_power_dbm;          /* configured maximum */
    uint32_t wifi_disconnects;      /* since boot */
    uint8_t  wifi_last_reason;      /* wifi_err_reason_t of the last one, 0 = none */
    uint32_t stack_acq, stack_timebase, stack_httpd, stack_tcpip, stack_wifi;
} seismo_hw_t;

void seismo_hw_get(seismo_hw_t *out);

void      config_set_defaults(void);
esp_err_t config_load(void);
esp_err_t config_save(void);
void      config_factory_reset(void);   /* does not return */
void      config_wifi_reset(void);      /* does not return */

/** @brief Acquisition settings derived from the config. */
void      config_to_acq(const seismo_config_t *c, acq_settings_t *s);

/* ── Settings by name ─────────────────────────────────────────────────────
 * One code path for the HTTP API, config import and the USB console.
 * General keys: hostname, port, login, user, pass, station, network,
 *   clk_src, clk_hz, osr, decim, taps, ref, ref_edge, rtc
 * Per channel (N = 0 or 1): chN_name, chN_pga, chN_phase, chN_gcal,
 *   chN_ocal, chN_div, chN_amp, chN_sens
 * WiFi (import and console only): wifi_ssid, wifi_pass
 *
 * *acq_changed is set when acquisition must restart, *reboot_needed when
 * the change only takes effect after a reboot. */
esp_err_t config_set_kv(seismo_config_t *c, const char *key, const char *val,
                        bool allow_wifi, bool *reboot_needed, bool *acq_changed,
                        const char **err);

/** @brief Make c the live config and write NVS. Restarts acquisition if asked. */
esp_err_t config_commit(const seismo_config_t *c, bool acq_changed);

void      config_lock(void);
void      config_unlock(void);

/** @brief Config as "key=value" lines, passwords left out. Returns length or -1. */
int       config_export_text(char *out, size_t cap);

/** @brief Set a runtime log level by name (none..debug). Lasts until reboot. */
esp_err_t seismo_log_level_set(const char *tag, const char *level, const char **err);

esp_err_t webserver_start(void);
esp_err_t webserver_start_portal_mode(void);
bool      webserver_running(void);
bool      webserver_ota_active(void);

/** @brief Restart after delay_ms from an esp_timer, so callers never sleep. */
void      seismo_schedule_restart(uint32_t delay_ms);

#endif /* WEB_SERVER_H */
