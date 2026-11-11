/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Vlad Patoka (VE3VPT) */
/**
 * @file web_server.c
 * @brief NVS configuration and the HTTP server.
 *
 * Normal mode serves the tuning page and its JSON API. Portal mode (open AP,
 * no WiFi credentials yet) serves only the setup page, the WiFi endpoints
 * and the OS connectivity probes, and redirects everything else to /portal.
 *
 * Handlers never sleep. Anything that must happen "after the reply has gone
 * out" (reboot after saving WiFi, after OTA) goes through an esp_timer.
 *
 * Infrastructure (portal, OTA writer, Origin guard, Basic Auth, config by
 * name, export/import) follows esp32nixie, which follows webcontrol.
 */

#include "web_server.h"
#include "ads131m0x.h"
#include "event_log.h"
#include "fault.h"
#include "io_map.h"
#include "timebase.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "WEB";

seismo_config_t g_config;

static httpd_handle_t s_server;
static esp_timer_handle_t s_restart_timer;
static volatile bool s_ota_active;

static StaticSemaphore_t s_cfg_mutex_buf;
static SemaphoreHandle_t s_cfg_mutex;

void config_lock(void)   { if (s_cfg_mutex) xSemaphoreTake(s_cfg_mutex, portMAX_DELAY); }
void config_unlock(void) { if (s_cfg_mutex) xSemaphoreGive(s_cfg_mutex); }

static const char *const s_clk_names[] = { "ext", "ledc", "ltc6903" };

static const char *capture_name(unsigned m)
{
    return m == ACQ_CAPTURE_MCPWM ? "mcpwm" : "isr";
}

/* ═════════════════════════════════════════════════════════════════════════
 * Configuration
 * ═════════════════════════════════════════════════════════════════════════ */

void config_set_defaults(void)
{
    memset(&g_config, 0, sizeof(g_config));
    g_config.version          = SEISMO_CONFIG_VERSION;
    strcpy(g_config.hostname, "vseismo");
    g_config.http_port        = 80;
    strcpy(g_config.username, "admin");
    strcpy(g_config.password, "admin");
    strcpy(g_config.station, "VSEIS");
    strcpy(g_config.network, "XX");          /* FDSN code for temporary / unregistered */
    g_config.clk_src          = ACQ_CLK_EXT;
    g_config.clk_hz           = 8192000;
    g_config.osr              = 4096;        /* 1000 SPS at 8.192 MHz */
    g_config.decim            = 10;          /* 100 SPS out */
    g_config.taps             = 0;           /* automatic: 201 */
    g_config.ref              = TB_REF_NONE;
    g_config.ref_rising       = 1;
    g_config.rtc              = 0;
    g_config.capture          = ACQ_CAPTURE_ISR;   /* mcpwm once proven on the bench */
    static const char *const names[ACQ_NCH] = { "Z", "H" };
    for (int c = 0; c < ACQ_NCH; c++) {
        seismo_ch_cfg_t *ch = &g_config.ch[c];
        strcpy(ch->name, names[c]);
        ch->pga      = 1;
        ch->phase    = 0;
        ch->gcal     = 1.0f;
        ch->ocal     = 0;
        ch->divider  = 1.0f / 10.1f;   /* 9.1k over 1.0k */
        ch->amp_gain = 0;              /* set it: the EQAMP trim pot decides */
        ch->sens     = 0;              /* set it from the geophone datasheet */
    }
}

static void config_sanitize(void)
{
    g_config.wifi_ssid[sizeof(g_config.wifi_ssid) - 1] = '\0';
    g_config.wifi_password[sizeof(g_config.wifi_password) - 1] = '\0';
    g_config.hostname[sizeof(g_config.hostname) - 1] = '\0';
    g_config.username[sizeof(g_config.username) - 1] = '\0';
    g_config.password[sizeof(g_config.password) - 1] = '\0';
    g_config.station[sizeof(g_config.station) - 1] = '\0';
    g_config.network[sizeof(g_config.network) - 1] = '\0';
    if (g_config.hostname[0] == '\0') strcpy(g_config.hostname, "vseismo");
    if (g_config.http_port == 0) g_config.http_port = 80;
    if (g_config.clk_src > ACQ_CLK_LTC6903) g_config.clk_src = ACQ_CLK_EXT;
    if (g_config.clk_hz < 1000000 || g_config.clk_hz > 8400000) g_config.clk_hz = 8192000;
    if (ads_osr_code(g_config.osr) < 0 || g_config.osr < ACQ_OSR_MIN) g_config.osr = 4096;
    if (g_config.decim == 0) g_config.decim = 1;
    if (g_config.decim > ACQ_DECIM_MAX) {
        ESP_LOGW(TAG, "Stored decim %u is past the FIR limit of %d: using 10", g_config.decim, ACQ_DECIM_MAX);
        g_config.decim = 10;
    }
    if (g_config.ref > TB_REF_RTC) g_config.ref = TB_REF_NONE;
    if (g_config.capture > ACQ_CAPTURE_MCPWM) g_config.capture = ACQ_CAPTURE_ISR;
    for (int c = 0; c < ACQ_NCH; c++) {
        seismo_ch_cfg_t *ch = &g_config.ch[c];
        ch->name[sizeof(ch->name) - 1] = '\0';
        if (ch->pga == 0 || (ch->pga & (ch->pga - 1)) || ch->pga > 128) ch->pga = 1;
        if (!(ch->gcal > 0.5f && ch->gcal < 1.5f)) ch->gcal = 1.0f;
        if (!(ch->divider > 0.0f && ch->divider <= 1.0f)) ch->divider = 1.0f / 10.1f;
        if (!(ch->amp_gain >= 0.0f)) ch->amp_gain = 0;
        if (!(ch->sens >= 0.0f)) ch->sens = 0;
    }
}

esp_err_t config_load(void)
{
    if (!s_cfg_mutex) s_cfg_mutex = xSemaphoreCreateMutexStatic(&s_cfg_mutex_buf);
    config_set_defaults();

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No stored config, using defaults");
        return ESP_OK;
    }
    size_t size = 0;
    err = nvs_get_blob(nvs, NVS_KEY_CONFIG, NULL, &size);
    if (err == ESP_OK && size >= sizeof(uint32_t)) {
        static seismo_config_t blob;
        size_t rd = size > sizeof(blob) ? sizeof(blob) : size;
        memset(&blob, 0, sizeof(blob));
        err = size <= sizeof(blob) ? nvs_get_blob(nvs, NVS_KEY_CONFIG, &blob, &rd)
                                   : ESP_ERR_NVS_INVALID_LENGTH;
        if (err == ESP_OK && blob.version == SEISMO_CONFIG_VERSION && size == sizeof(blob)) {
            g_config = blob;
        } else if (err == ESP_OK && blob.version < SEISMO_CONFIG_VERSION) {
            memcpy(&g_config, &blob, rd);
            g_config.version = SEISMO_CONFIG_VERSION;
            /* Fields added since: an older blob may carry struct padding there. */
            if (blob.version < 2) g_config.capture = ACQ_CAPTURE_ISR;
            ESP_LOGW(TAG, "Config migrated from v%lu", (unsigned long)blob.version);
        } else {
            ESP_LOGW(TAG, "Config blob unusable (v%lu, %u bytes), using defaults",
                     (unsigned long)blob.version, (unsigned)size);
            config_set_defaults();
        }
    }
    nvs_close(nvs);
    config_sanitize();
    ESP_LOGI(TAG, "Config loaded: host=%s clk=%s osr=%u decim=%u ref=%s wifi=%s",
             g_config.hostname, s_clk_names[g_config.clk_src], g_config.osr, g_config.decim,
             timebase_ref_name((tb_ref_t)g_config.ref),
             g_config.wifi_configured ? g_config.wifi_ssid : "(none)");
    return ESP_OK;
}

esp_err_t config_save(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(nvs, NVS_KEY_CONFIG, &g_config, sizeof(g_config));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) ESP_LOGE(TAG, "Config save: %s", esp_err_to_name(err));
    return err;
}

void config_to_acq(const seismo_config_t *c, acq_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->clk_src = (acq_clk_src_t)c->clk_src;
    s->clk_hz = c->clk_hz;
    s->osr = c->osr;
    s->decim = c->decim;
    s->taps = c->taps;
    for (int i = 0; i < ACQ_NCH; i++) {
        s->pga[i] = c->ch[i].pga;
        s->phase[i] = c->ch[i].phase;
        s->gcal[i] = c->ch[i].gcal;
        s->ocal[i] = c->ch[i].ocal;
    }
}

static void restart_cb(void *arg)
{
    (void)arg;
    fault_safe_outputs();
    esp_restart();
}

void seismo_schedule_restart(uint32_t delay_ms)
{
    if (!s_restart_timer) {
        const esp_timer_create_args_t a = { .callback = restart_cb, .name = "restart" };
        if (esp_timer_create(&a, &s_restart_timer) != ESP_OK) restart_cb(NULL);
    }
    esp_timer_stop(s_restart_timer);
    esp_timer_start_once(s_restart_timer, (uint64_t)delay_ms * 1000);
}

void config_factory_reset(void)
{
    ESP_LOGW(TAG, "Factory reset: erasing NVS");
    fault_safe_outputs();
    nvs_flash_erase();
    esp_restart();
}

void config_wifi_reset(void)
{
    ESP_LOGW(TAG, "WiFi reset: clearing credentials");
    config_lock();
    memset(g_config.wifi_ssid, 0, sizeof(g_config.wifi_ssid));
    memset(g_config.wifi_password, 0, sizeof(g_config.wifi_password));
    g_config.wifi_configured = 0;
    config_save();
    config_unlock();
    fault_safe_outputs();
    esp_restart();
}

bool webserver_running(void)    { return s_server != NULL; }
bool webserver_ota_active(void) { return s_ota_active; }

/* ═════════════════════════════════════════════════════════════════════════
 * Helpers
 * ═════════════════════════════════════════════════════════════════════════ */

static void set_json_headers(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
}

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    set_json_headers(req);
    return httpd_resp_sendstr(req, json);
}

static esp_err_t send_json_err(httpd_req_t *req, const char *msg)
{
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", msg);
    return send_json(req, buf);
}

static void json_escape(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (; *src && o + 7 < cap; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') { dst[o++] = '\\'; dst[o++] = (char)c; }
        else if (c < 0x20) { o += (size_t)snprintf(dst + o, cap - o, "\\u%04x", c); }
        else dst[o++] = (char)c;
    }
    dst[o] = '\0';
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *s)
{
    char *d = s;
    while (*s) {
        if (*s == '%' && hexval(s[1]) >= 0 && hexval(s[2]) >= 0) {
            *d++ = (char)(hexval(s[1]) * 16 + hexval(s[2]));
            s += 3;
        } else if (*s == '+') {
            *d++ = ' ';
            s++;
        } else {
            *d++ = *s++;
        }
    }
    *d = '\0';
}

static int read_body(httpd_req_t *req, char *buf, size_t cap)
{
    int total = (int)req->content_len;
    if (total < 0 || (size_t)total >= cap) return -1;
    int got = 0, retries = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, (size_t)(total - got));
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++retries < 5) continue;
        if (r <= 0) return -1;
        got += r;
    }
    buf[got] = '\0';
    return got;
}

static bool form_get(const char *body, const char *key, char *out, size_t cap)
{
    if (httpd_query_key_value(body, key, out, cap) != ESP_OK) return false;
    url_decode(out);
    return true;
}

static bool web_auth_ok(httpd_req_t *req)
{
    if (!g_config.weblogin_enabled) return true;
    char hdr[128];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK &&
        strncasecmp(hdr, "Basic ", 6) == 0) {
        unsigned char dec[96];
        size_t n = 0;
        if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &n,
                                  (const unsigned char *)hdr + 6, strlen(hdr + 6)) == 0) {
            dec[n] = '\0';
            char expect[48];
            int el = snprintf(expect, sizeof(expect), "%s:%s", g_config.username, g_config.password);
            if (el > 0 && (size_t)el == n && strcmp((char *)dec, expect) == 0) return true;
        }
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"VSEISMO\"");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "401 Unauthorized");
    return false;
}

static void origin_norm(char *s)
{
    char *p = strstr(s, "://");
    if (p) memmove(s, p + 3, strlen(p + 3) + 1);
    size_t n = strlen(s);
    if (n > 0 && s[n - 1] == '/') s[--n] = '\0';
    if (n > 3 && strcmp(s + n - 3, ":80") == 0) s[n - 3] = '\0';
    for (char *c = s; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
}

/* Cross-site request guard for every POST (see esp32nixie for the reasoning).
 * No Origin (curl, scripts) passes; a foreign or "null" Origin is refused. */
static bool post_origin_ok(httpd_req_t *req)
{
    if (httpd_req_get_hdr_value_len(req, "Origin") == 0) return true;
    char origin[96] = "", host[96] = "";
    bool ok = httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) == ESP_OK &&
              httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK;
    if (ok) {
        origin_norm(origin);
        origin_norm(host);
        ok = origin[0] && strcmp(origin, "null") != 0 && strcmp(origin, host) == 0;
    }
    if (ok) return true;
    ESP_LOGW(TAG, "POST %s refused: cross-site Origin", req->uri);
    httpd_resp_set_status(req, "403 Forbidden");
    set_json_headers(req);
    httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"cross-site request refused\"}");
    return false;
}

/* Fixed-point text for doubles that may be NaN. */
static const char *num(char *buf, size_t cap, double v, int decimals)
{
    if (isnan(v) || isinf(v)) snprintf(buf, cap, "null");
    else snprintf(buf, cap, "%.*f", decimals, v);
    return buf;
}

/* ═════════════════════════════════════════════════════════════════════════
 * Settings by name
 * ═════════════════════════════════════════════════════════════════════════ */

static bool hostname_ok(const char *h)
{
    size_t n = strlen(h);
    if (n == 0 || n > 31 || h[0] == '-' || h[n - 1] == '-') return false;
    for (size_t i = 0; i < n; i++) {
        char c = h[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

static bool code_ok(const char *v, size_t maxlen)
{
    size_t n = strlen(v);
    if (n == 0 || n > maxlen) return false;
    for (size_t i = 0; i < n; i++) {
        char c = v[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
    }
    return true;
}

static bool parse_bool(const char *v, uint8_t *out)
{
    if (strcmp(v, "1") == 0 || strcasecmp(v, "on") == 0 || strcasecmp(v, "true") == 0) { *out = 1; return true; }
    if (strcmp(v, "0") == 0 || strcasecmp(v, "off") == 0 || strcasecmp(v, "false") == 0) { *out = 0; return true; }
    return false;
}

static bool parse_long(const char *v, long lo, long hi, long *out)
{
    char *end;
    long x = strtol(v, &end, 10);
    if (!v[0] || *end || x < lo || x > hi) return false;
    *out = x;
    return true;
}

static bool parse_float(const char *v, float lo, float hi, float *out)
{
    char *end;
    float x = strtof(v, &end);
    if (!v[0] || *end || !(x >= lo && x <= hi)) return false;
    *out = x;
    return true;
}

static esp_err_t set_channel_kv(seismo_ch_cfg_t *ch, const char *sub, const char *v,
                                bool *acq, const char **err)
{
    long l;
    float f;
    if (strcmp(sub, "name") == 0) {
        if (!v[0] || strlen(v) >= sizeof(ch->name)) { *err = "chN_name: 1-7 characters"; return ESP_ERR_INVALID_ARG; }
        strcpy(ch->name, v);
    } else if (strcmp(sub, "pga") == 0) {
        if (!parse_long(v, 1, 128, &l) || (l & (l - 1))) { *err = "chN_pga: 1, 2, 4 .. 128"; return ESP_ERR_INVALID_ARG; }
        if (ch->pga != l) *acq = true;
        ch->pga = (uint8_t)l;
    } else if (strcmp(sub, "phase") == 0) {
        if (!parse_long(v, -512, 511, &l)) { *err = "chN_phase: -512..511 modulator clocks"; return ESP_ERR_INVALID_ARG; }
        if (ch->phase != l) *acq = true;
        ch->phase = (int16_t)l;
    } else if (strcmp(sub, "gcal") == 0) {
        if (!parse_float(v, 0.5f, 1.5f, &f)) { *err = "chN_gcal: 0.5..1.5"; return ESP_ERR_INVALID_ARG; }
        if (ch->gcal != f) *acq = true;
        ch->gcal = f;
    } else if (strcmp(sub, "ocal") == 0) {
        if (!parse_long(v, -8388608, 8388607, &l)) { *err = "chN_ocal: counts, +-8388607"; return ESP_ERR_INVALID_ARG; }
        if (ch->ocal != l) *acq = true;
        ch->ocal = (int32_t)l;
    } else if (strcmp(sub, "div") == 0) {
        if (!parse_float(v, 0.001f, 1.0f, &f)) { *err = "chN_div: 0.001..1 (ADC volts per amp volt)"; return ESP_ERR_INVALID_ARG; }
        ch->divider = f;
    } else if (strcmp(sub, "amp") == 0) {
        if (!parse_float(v, 0.0f, 100000.0f, &f)) { *err = "chN_amp: 0 (unknown) .. 100000"; return ESP_ERR_INVALID_ARG; }
        ch->amp_gain = f;
    } else if (strcmp(sub, "sens") == 0) {
        if (!parse_float(v, 0.0f, 10000.0f, &f)) { *err = "chN_sens: 0 (unknown) .. 10000 V/(m/s)"; return ESP_ERR_INVALID_ARG; }
        ch->sens = f;
    } else {
        *err = "unknown key";
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t config_set_kv(seismo_config_t *c, const char *key, const char *v,
                        bool allow_wifi, bool *reboot, bool *acq, const char **err)
{
    static const char *dummy;
    static bool dummy_b;
    if (!err) err = &dummy;
    if (!reboot) reboot = &dummy_b;
    if (!acq) acq = &dummy_b;
    *err = NULL;
    long l;
    uint8_t b;

    if (strncmp(key, "ch", 2) == 0 && key[2] >= '0' && key[2] < '0' + ACQ_NCH && key[3] == '_') {
        return set_channel_kv(&c->ch[key[2] - '0'], key + 4, v, acq, err);
    }

    if (strcmp(key, "hostname") == 0) {
        if (!hostname_ok(v)) { *err = "hostname: letters, digits, dash, 1-31"; goto bad; }
        if (strcmp(v, c->hostname) != 0) *reboot = true;
        strcpy(c->hostname, v);
    } else if (strcmp(key, "port") == 0) {
        if (!parse_long(v, 1, 65535, &l)) { *err = "port: 1..65535"; goto bad; }
        if (l != c->http_port) *reboot = true;
        c->http_port = (uint16_t)l;
    } else if (strcmp(key, "login") == 0) {
        if (!parse_bool(v, &c->weblogin_enabled)) { *err = "login: 0 or 1"; goto bad; }
    } else if (strcmp(key, "user") == 0) {
        if (!v[0]) return ESP_OK;
        if (strlen(v) >= sizeof(c->username)) { *err = "user: up to 15 characters"; goto bad; }
        strcpy(c->username, v);
    } else if (strcmp(key, "pass") == 0) {
        if (!v[0]) return ESP_OK;
        if (strlen(v) >= sizeof(c->password)) { *err = "pass: up to 23 characters"; goto bad; }
        strcpy(c->password, v);
    } else if (strcmp(key, "station") == 0) {
        if (!code_ok(v, 5)) { *err = "station: 1-5 of A-Z, 0-9"; goto bad; }
        strcpy(c->station, v);
    } else if (strcmp(key, "network") == 0) {
        if (!code_ok(v, 2)) { *err = "network: 1-2 of A-Z, 0-9"; goto bad; }
        strcpy(c->network, v);
    } else if (strcmp(key, "clk_src") == 0) {
        int s = -1;
        for (int i = 0; i < 3; i++) if (strcasecmp(v, s_clk_names[i]) == 0) s = i;
        if (s < 0) { *err = "clk_src: ext, ledc or ltc6903"; goto bad; }
        if (c->clk_src != s) *acq = true;
        c->clk_src = (uint8_t)s;
    } else if (strcmp(key, "clk_hz") == 0) {
        if (!parse_long(v, 1000000, 8400000, &l)) { *err = "clk_hz: 1000000..8400000"; goto bad; }
        if (c->clk_hz != (uint32_t)l) *acq = true;
        c->clk_hz = (uint32_t)l;
    } else if (strcmp(key, "osr") == 0) {
        if (!parse_long(v, ACQ_OSR_MIN, 16256, &l) || ads_osr_code((uint16_t)l) < 0) {
            *err = "osr: 512, 1024, 2048, 4096, 8192 or 16256"; goto bad;
        }
        if (c->osr != l) *acq = true;
        c->osr = (uint16_t)l;
    } else if (strcmp(key, "decim") == 0) {
        if (!parse_long(v, 1, ACQ_DECIM_MAX, &l)) { *err = "decim: 1..18"; goto bad; }
        if (c->decim != l) *acq = true;
        c->decim = (uint8_t)l;
    } else if (strcmp(key, "taps") == 0) {
        if (!parse_long(v, 0, 255, &l) || (l && (l < 3 || !(l & 1)))) { *err = "taps: 0 (auto) or odd 3..255"; goto bad; }
        if (c->taps != l) *acq = true;
        c->taps = (uint8_t)l;
    } else if (strcmp(key, "ref") == 0) {
        int r = strcasecmp(v, "none") == 0 ? TB_REF_NONE : strcasecmp(v, "gps") == 0 ? TB_REF_GPS
              : strcasecmp(v, "rtc") == 0 ? TB_REF_RTC : -1;
        if (r < 0) { *err = "ref: none, gps or rtc"; goto bad; }
        if (c->ref != r) *reboot = true;
        c->ref = (uint8_t)r;
    } else if (strcmp(key, "ref_edge") == 0) {
        uint8_t rising = strcasecmp(v, "rise") == 0 ? 1 : strcasecmp(v, "fall") == 0 ? 0 : 2;
        if (rising > 1) { *err = "ref_edge: rise or fall"; goto bad; }
        if (c->ref_rising != rising) *reboot = true;
        c->ref_rising = rising;
    } else if (strcmp(key, "rtc") == 0) {
        if (!parse_bool(v, &b)) { *err = "rtc: 0 or 1"; goto bad; }
        if (c->rtc != b) *reboot = true;
        c->rtc = b;
    } else if (strcmp(key, "capture") == 0) {
        int m = strcasecmp(v, "isr") == 0 ? ACQ_CAPTURE_ISR : strcasecmp(v, "mcpwm") == 0 ? ACQ_CAPTURE_MCPWM : -1;
        if (m < 0) { *err = "capture: isr or mcpwm"; goto bad; }
        if (c->capture != m) *reboot = true;
        c->capture = (uint8_t)m;
    } else if (allow_wifi && strcmp(key, "wifi_ssid") == 0) {
        if (!v[0] || strlen(v) > 32) { *err = "wifi_ssid: 1-32 bytes"; goto bad; }
        if (strcmp(v, c->wifi_ssid) != 0) *reboot = true;
        strcpy(c->wifi_ssid, v);
        c->wifi_configured = 1;
    } else if (allow_wifi && strcmp(key, "wifi_pass") == 0) {
        size_t n = strlen(v);
        if (n > 63 || (n && n < 8)) { *err = "wifi_pass: empty (open) or 8-63 characters"; goto bad; }
        if (strcmp(v, c->wifi_password) != 0) *reboot = true;
        strcpy(c->wifi_password, v);
    } else {
        *err = "unknown key";
        goto bad;
    }
    return ESP_OK;
bad:
    return ESP_ERR_INVALID_ARG;
}

esp_err_t config_commit(const seismo_config_t *c, bool acq_changed)
{
    g_config = *c;
    esp_err_t err = config_save();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Config updated: clk=%s %lu Hz osr=%u decim=%u taps=%u pga=%u/%u%s",
                 s_clk_names[g_config.clk_src], (unsigned long)g_config.clk_hz, g_config.osr,
                 g_config.decim, g_config.taps, g_config.ch[0].pga, g_config.ch[1].pga,
                 acq_changed ? ", restarting acquisition" : "");
    }
    if (acq_changed) {
        acq_settings_t s;
        config_to_acq(&g_config, &s);
        acq_apply(&s);
    }
    return err;
}

int config_export_text(char *out, size_t cap)
{
    const esp_app_desc_t *app = esp_app_get_description();
    int n = snprintf(out, cap,
        "# vseismo settings, api %d, firmware %s\n"
        "# Passwords are not exported. To restore them, add pass= and\n"
        "# wifi_pass= lines before importing.\n"
        "hostname=%s\nport=%u\nlogin=%u\nuser=%s\n"
        "station=%s\nnetwork=%s\n"
        "clk_src=%s\nclk_hz=%lu\nosr=%u\ndecim=%u\ntaps=%u\n"
        "ref=%s\nref_edge=%s\nrtc=%u\ncapture=%s\n",
        SEISMO_API_VERSION, app->version,
        g_config.hostname, g_config.http_port, g_config.weblogin_enabled, g_config.username,
        g_config.station, g_config.network,
        s_clk_names[g_config.clk_src], (unsigned long)g_config.clk_hz, g_config.osr,
        g_config.decim, g_config.taps,
        timebase_ref_name((tb_ref_t)g_config.ref), g_config.ref_rising ? "rise" : "fall", g_config.rtc,
        capture_name(g_config.capture));
    for (int c = 0; c < ACQ_NCH && n > 0 && (size_t)n < cap; c++) {
        const seismo_ch_cfg_t *ch = &g_config.ch[c];
        n += snprintf(out + n, cap - (size_t)n,
            "ch%d_name=%s\nch%d_pga=%u\nch%d_phase=%d\nch%d_gcal=%.6f\nch%d_ocal=%ld\n"
            "ch%d_div=%.6f\nch%d_amp=%g\nch%d_sens=%g\n",
            c, ch->name, c, ch->pga, c, ch->phase, c, (double)ch->gcal, c, (long)ch->ocal,
            c, (double)ch->divider, c, (double)ch->amp_gain, c, (double)ch->sens);
    }
    if (n > 0 && (size_t)n < cap && g_config.wifi_configured && g_config.wifi_ssid[0]) {
        n += snprintf(out + n, cap - (size_t)n, "wifi_ssid=%s\n", g_config.wifi_ssid);
    }
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

/* ═════════════════════════════════════════════════════════════════════════
 * Normal mode: UI and API
 * ═════════════════════════════════════════════════════════════════════════ */

extern const uint8_t index_html_start[]  asm("_binary_index_html_start");
extern const uint8_t index_html_end[]    asm("_binary_index_html_end");
extern const uint8_t portal_html_start[] asm("_binary_portal_html_start");
extern const uint8_t portal_html_end[]   asm("_binary_portal_html_end");

static esp_err_t root_handler(httpd_req_t *req)
{
    if (!web_auth_ok(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start);
}

static const char *ota_state_name(void)
{
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK) return "factory";
    switch (st) {
        case ESP_OTA_IMG_VALID:          return "valid";
        case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
        case ESP_OTA_IMG_NEW:            return "new";
        default:                         return "other";
    }
}

static esp_err_t api_status_handler(httpd_req_t *req)
{
    if (!web_auth_ok(req)) return ESP_OK;
    tb_status_t tb;
    timebase_get_status(&tb);
    const esp_app_desc_t *app = esp_app_get_description();

    char ssid[70] = "";
    int rssi = 0, ch = 0;
    wifi_ap_record_t ap;
    if (g_wifi_mode == SEISMO_WIFI_STA && g_wifi_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        json_escape(ssid, sizeof(ssid), (const char *)ap.ssid);
        rssi = ap.rssi;
        ch = ap.primary;
    }
    char ip6[48] = "";
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_ip6_addr_t a6;
    if (sta && g_wifi_mode == SEISMO_WIFI_STA && esp_netif_get_ip6_global(sta, &a6) == ESP_OK) {
        snprintf(ip6, sizeof(ip6), IPV6STR, IPV62STR(a6));
    }
    int64_t utc_us = timebase_esp_to_utc_us(esp_timer_get_time());
    char fsm[24], appm[24], espm[24], sigm[24], temp[16];
    num(fsm, sizeof(fsm), tb.rate_valid ? tb.fs_measured : NAN, 4);
    num(appm, sizeof(appm), tb.rate_valid ? tb.adc_ppm : NAN, 3);
    num(espm, sizeof(espm), tb.rate_valid ? tb.esp_ppm : NAN, 3);
    num(sigm, sizeof(sigm), tb.rate_valid ? tb.rate_sigma_ppm : NAN, 4);
    num(temp, sizeof(temp), tb.rtc_online ? tb.rtc_temp_c : NAN, 2);

    char buf[1400];
    int n = snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"api\":%d,"
        "\"fw\":\"%s\",\"idf\":\"%s\",\"uptime\":%lld,\"heap\":%lu,\"heap_min\":%lu,"
        "\"station\":\"%s\",\"network\":\"%s\",\"utc_ms\":%lld,"
        "\"time\":{\"valid\":%s,\"src\":\"%s\",\"ntp_syncs\":%lu,\"ntp_age\":%lld,\"ntp_rejected\":%lu,"
        "\"quality\":\"%s\",\"slew_ms\":%.3f,\"pps_naming_errors\":%lu},"
        "\"ref\":{\"type\":\"%s\",\"edge\":\"%s\",\"seen\":%s,\"edges\":%lu,\"age_ms\":%lld,"
        "\"rate_valid\":%s,\"fs\":%s,\"adc_ppm\":%s,\"esp_ppm\":%s,"
        "\"rate_sigma_ppm\":%s,\"rate_points\":%lu,\"rate_rejects\":%lu},"
        "\"rtc\":{\"enabled\":%s,\"online\":%s,\"model\":\"%s\",\"osf\":%s,\"temp\":%s},"
        "\"wifi\":{\"mode\":\"%s\",\"up\":%s,\"ssid\":\"%s\",\"rssi\":%d,\"ch\":%d,\"band\":\"%s\","
        "\"ip\":\"%s\",\"ip6\":\"%s\"},"
        "\"faults\":%lu,\"class_a\":%s,\"ota\":\"%s\",\"ota_busy\":%s",
        SEISMO_API_VERSION, app->version, app->idf_ver,
        (long long)(esp_timer_get_time() / 1000000),
        (unsigned long)esp_get_free_heap_size(), (unsigned long)esp_get_minimum_free_heap_size(),
        g_config.station, g_config.network, (long long)(utc_us / 1000),
        tb.time_valid ? "true" : "false", tb.time_src, (unsigned long)tb.ntp_syncs, (long long)tb.ntp_age_s,
        (unsigned long)tb.ntp_rejected,
        timebase_tq_name(tb.quality), tb.slew_ms, (unsigned long)tb.pps_naming_errors,
        timebase_ref_name(tb.ref), tb.ref_rising ? "rise" : "fall", tb.ref_seen ? "true" : "false",
        (unsigned long)tb.ref_edges, (long long)tb.ref_age_ms,
        tb.rate_valid ? "true" : "false", fsm, appm, espm,
        sigm, (unsigned long)tb.rate_points, (unsigned long)tb.rate_rejects,
        tb.rtc_enabled ? "true" : "false", tb.rtc_online ? "true" : "false", tb.rtc_model,
        tb.rtc_osf ? "true" : "false", temp,
        g_wifi_mode == SEISMO_WIFI_AP_PROVISIONING ? "ap" : "sta",
        g_wifi_connected ? "true" : "false", ssid, rssi, ch, ch > 14 ? "5" : ch ? "2.4" : "",
        g_ip_address, ip6,
        (unsigned long)fault_get_mask(), fault_active() ? "true" : "false",
        ota_state_name(), s_ota_active ? "true" : "false");
    if (n < 0 || n >= (int)sizeof(buf)) return send_json_err(req, "status overflow");

    seismo_hw_t hw;
    seismo_hw_get(&hw);
    char hwb[720];
    int m = snprintf(hwb, sizeof(hwb),
        ",\"hw\":{\"cpu_mhz\":%lu,\"cores\":%u,\"chip_rev\":\"v%u.%u\",\"flash\":\"%s\","
        "\"mac\":\"" MACSTR "\",\"reset\":\"%s\","
        "\"mem\":{\"int_free\":%lu,\"int_min\":%lu,\"int_largest\":%lu,\"int_total\":%lu,"
        "\"psram_free\":%lu,\"psram_min\":%lu,\"psram_largest\":%lu,\"psram_total\":%lu},"
        "\"radio\":{\"up\":%s,\"rssi\":%d,\"ch\":%u,\"bssid\":\"" MACSTR "\",\"phy\":\"%s\","
        "\"tx_power_dbm\":%.2f,\"disconnects\":%lu,\"last_reason\":%u},"
        "\"stack_free\":{\"acq\":%lu,\"timebase\":%lu,\"httpd\":%lu,\"tcpip\":%lu,\"wifi\":%lu}}}",
        (unsigned long)hw.cpu_mhz, hw.cores, hw.chip_rev / 100, hw.chip_rev % 100, CONFIG_ESPTOOLPY_FLASHSIZE,
        MAC2STR(hw.mac), hw.reset_reason,
        (unsigned long)hw.int_free, (unsigned long)hw.int_min, (unsigned long)hw.int_largest,
        (unsigned long)hw.int_total, (unsigned long)hw.psram_free, (unsigned long)hw.psram_min,
        (unsigned long)hw.psram_largest, (unsigned long)hw.psram_total,
        hw.wifi_up ? "true" : "false", hw.rssi, hw.channel, MAC2STR(hw.bssid), hw.phy,
        (double)hw.tx_power_dbm, (unsigned long)hw.wifi_disconnects, hw.wifi_last_reason,
        (unsigned long)hw.stack_acq, (unsigned long)hw.stack_timebase, (unsigned long)hw.stack_httpd,
        (unsigned long)hw.stack_tcpip, (unsigned long)hw.stack_wifi);
    if (m < 0 || m >= (int)sizeof(hwb)) return send_json_err(req, "status overflow");

    set_json_headers(req);
    httpd_resp_send_chunk(req, buf, n);
    httpd_resp_send_chunk(req, hwb, m);
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ── Acquisition ──────────────────────────────────────────────────────────
 * Scale chain for one count:
 *   ADC pin     1.2 V / PGA / 2^23
 *   EQAMP out   ADC pin / divider
 *   ground      EQAMP out / (amp gain x geophone V/(m/s))   (in the passband)
 * Quantities whose inputs are not set (0) come back as null. */

static esp_err_t api_acq_handler(httpd_req_t *req)
{
    if (!web_auth_ok(req)) return ESP_OK;
    acq_status_t st;
    acq_get_status(&st);

    char *buf = malloc(2048);
    if (!buf) return send_json_err(req, "no memory");
    int n = snprintf(buf, 2048,
        "{\"ok\":true,\"api\":%d,\"running\":%s,\"adc_ok\":%s,\"clock_ok\":%s,"
        "\"adc_id\":\"0x%04X\",\"adc_status\":\"0x%04X\",\"clk_src\":\"%s\",\"clk_hz\":%lu,"
        "\"osr\":%u,\"decim\":%u,\"fs_in\":%.4f,\"fs_out\":%.4f,\"delay_ms\":%.2f,"
        "\"n_in\":%llu,\"seq\":%lu,\"crc_errors\":%lu,\"overruns\":%lu,\"stale_frames\":%lu,"
        "\"status_events\":%lu,\"restarts\":%lu,\"ring_len\":%lu,\"capture\":\"%s\","
        "\"cap_bad\":%lu,\"cap_bad_us\":%ld,\"drdy_rate\":%.2f,\"drdy_rate_bad\":%lu,\"ch\":[",
        SEISMO_API_VERSION, st.running ? "true" : "false", st.adc_ok ? "true" : "false",
        st.clock_ok ? "true" : "false", st.adc_id, st.adc_status, s_clk_names[g_config.clk_src],
        (unsigned long)st.clk_hz_actual, g_config.osr, g_config.decim, st.fs_in, st.fs_out,
        st.group_delay_s * 1000.0, (unsigned long long)st.n_in, (unsigned long)st.seq,
        (unsigned long)st.crc_errors, (unsigned long)st.overruns, (unsigned long)st.stale_frames,
        (unsigned long)st.status_events, (unsigned long)st.restarts, (unsigned long)acq_ring_len(),
        capture_name(acq_capture_mode()),
        (unsigned long)st.cap_bad, (long)st.cap_bad_us, st.drdy_rate, (unsigned long)st.drdy_rate_bad);

    for (int c = 0; c < ACQ_NCH && n < 1900; c++) {
        const seismo_ch_cfg_t *cc = &g_config.ch[c];
        const acq_ch_stats_t *s = &st.ch[c];
        double lsb_uv = 1.2e6 / cc->pga / 8388608.0;
        double rms_uv = s->rms * lsb_uv;
        double amp_mv = cc->divider > 0 ? rms_uv / cc->divider / 1000.0 : NAN;
        double vel = (cc->amp_gain > 0 && cc->sens > 0 && cc->divider > 0)
                   ? rms_uv / cc->divider / cc->amp_gain / cc->sens : NAN;   /* um/s */
        double dbfs = s->rms > 0 ? 20.0 * log10(s->rms * 1.41421356 / 8388607.0) : NAN;
        char a[24], b[24], d[24], e[24], name[20];
        json_escape(name, sizeof(name), cc->name);
        n += snprintf(buf + n, 2048 - (size_t)n,
            "%s{\"name\":\"%s\",\"pga\":%u,\"mean\":%.1f,\"rms\":%.2f,\"min\":%ld,\"max\":%ld,"
            "\"clips\":%lu,\"lsb_uv\":%.6f,\"rms_uv\":%s,\"amp_mv\":%s,\"vel_ums\":%s,\"dbfs\":%s,"
            "\"mux\":\"%s\"}",
            c ? "," : "", name, cc->pga, s->mean, s->rms, (long)s->min, (long)s->max,
            (unsigned long)s->clips, lsb_uv, num(a, sizeof(a), rms_uv, 3),
            num(b, sizeof(b), amp_mv, 4), num(d, sizeof(d), vel, 4), num(e, sizeof(e), dbfs, 1),
            acq_mux_name(st.mux[c]));
    }
    if (n < 2040) n += snprintf(buf + n, 2048 - (size_t)n, "]}");
    esp_err_t r = (n >= 2048) ? send_json_err(req, "acq overflow")
                              : (set_json_headers(req), httpd_resp_send(req, buf, n));
    free(buf);
    return r;
}

/* GET /api/wave?since=SEQ&max=N[&order=newest|oldest]  ->  samples from SEQ
 * on, chunked JSON. With more than N waiting, order=newest (default) sends
 * the newest N and skips the rest (live display); order=oldest sends the
 * oldest N still held and "pending" says how many follow (recorder catching
 * up). The page polls with the "next" value it got back. t0 is the UTC time of
 * the first sample in ms (0 = no wall-clock time yet). A seq jump or an
 * epoch change means a gap; the page draws a break there. flags[] holds
 * ACQ_F_* per sample: the FIR window of that sample held a lost, corrupt,
 * unsettled or clipped input. */
#define WAVE_MAX 1000
#define WAVE_BLK 64            /* samples per ring copy, on the httpd stack */

/* One JSON array of the window: channel col, or the flags when col ==
 * ACQ_NCH. Read from the ring block by block; nothing is allocated. A
 * sample the writer reached during the reply (only possible on a stalled
 * connection) goes out as the previous value with the gap flag set. The
 * flags array is sent last, so it sees every loss an earlier array saw. */
static void wave_column(httpd_req_t *req, int col, uint32_t first, int cnt)
{
    acq_sample_t blk[WAVE_BLK];
    char chunk[512];
    int o = col < ACQ_NCH ? snprintf(chunk, sizeof(chunk), ",\"ch%d\":[", col)
                          : snprintf(chunk, sizeof(chunk), ",\"flags\":[");
    int32_t held = 0;
    for (int off = 0; off < cnt; off += WAVE_BLK) {
        int k = cnt - off < WAVE_BLK ? cnt - off : WAVE_BLK;
        int lost = acq_read_block(first + (uint32_t)off, blk, k);
        for (int i = 0; i < k; i++) {
            if (o > (int)sizeof(chunk) - 16) {
                httpd_resp_send_chunk(req, chunk, o);
                o = 0;
            }
            const char *sep = off + i ? "," : "";
            if (col < ACQ_NCH) {
                if (i >= lost) held = blk[i].v[col];
                o += snprintf(chunk + o, sizeof(chunk) - (size_t)o, "%s%ld", sep, (long)held);
            } else {
                unsigned f = i < lost ? ACQ_F_GAP : blk[i].flags;
                o += snprintf(chunk + o, sizeof(chunk) - (size_t)o, "%s%u", sep, f);
            }
        }
    }
    o += snprintf(chunk + o, sizeof(chunk) - (size_t)o, "]");
    httpd_resp_send_chunk(req, chunk, o);
}

static esp_err_t api_wave_handler(httpd_req_t *req)
{
    if (!web_auth_ok(req)) return ESP_OK;
    char q[96] = "", v[16];
    uint32_t since = 0;
    int max = 400;
    bool oldest = false;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK) since = (uint32_t)strtoul(v, NULL, 10);
        if (httpd_query_key_value(q, "max", v, sizeof(v)) == ESP_OK) max = atoi(v);
        if (httpd_query_key_value(q, "order", v, sizeof(v)) == ESP_OK) {
            if (strcmp(v, "oldest") == 0) oldest = true;
            else if (strcmp(v, "newest") != 0) return send_json_err(req, "order: newest or oldest");
        }
    }
    if (max < 1) max = 1;
    if (max > WAVE_MAX) max = WAVE_MAX;

    /* Output seq starts at 0 on every boot. A random id drawn on the first
     * request (WiFi is up by then, so esp_random() is truly random) lets a
     * recorder tell a reboot from a seq it simply missed. Only the httpd
     * task gets here. */
    static uint32_t s_boot;
    while (!s_boot) s_boot = esp_random();

    uint32_t first = 0, next = 0, pending = 0;
    int cnt = acq_read_window(since, max, oldest, &first, &next, &pending);

    /* One unbroken run only, so t0 + i / fs holds for every sample sent.
     * Oldest first stops at the first break and the rest is pending; newest
     * keeps the run that ends the window, and the page sees the seq jump. */
    uint32_t end = first + (uint32_t)cnt;
    acq_run_t run;
    acq_window_run(&first, &cnt, !oldest, &run);
    if (oldest) {
        next = first + (uint32_t)cnt;
        pending += end - next;
    }

    set_json_headers(req);
    char line[256];
    snprintf(line, sizeof(line),
             "{\"ok\":true,\"next\":%lu,\"seq0\":%lu,\"n\":%d,\"pending\":%lu,\"fs\":%.4f,\"t0\":%lld,\"tq\":\"%s\","
             "\"t0_us\":%lld,\"epoch\":%u,\"boot\":%lu",
             (unsigned long)next, (unsigned long)(cnt ? first : next), cnt, (unsigned long)pending,
             run.fs, (long long)(cnt ? run.t0_us / 1000 : 0),
             timebase_tq_name(cnt ? (tb_tq_t)run.tq : TB_TQ_NONE),
             (long long)(cnt ? run.t0_us : 0), (unsigned)run.epoch, (unsigned long)s_boot);
    httpd_resp_send_chunk(req, line, HTTPD_RESP_USE_STRLEN);

    for (int c = 0; c <= ACQ_NCH; c++) wave_column(req, c, first, cnt);
    httpd_resp_send_chunk(req, "}", 1);
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* POST /api/acq/cmd  cmd=sync | restart | mux&ch=N&mode=normal|short|testp|testn | zero&ch=N
 * mux is a bench aid and is not saved; restart and the next reboot go back
 * to normal inputs. zero folds the channel's current mean into OCAL (the
 * chip subtracts OCAL from each result) and saves it: short the input or
 * select mux=short first. */
static esp_err_t api_acq_cmd_handler(httpd_req_t *req)
{
    if (!post_origin_ok(req) || !web_auth_ok(req)) return ESP_OK;
    char body[128], cmd[16] = "", chs[8] = "", mode[12] = "";
    if (read_body(req, body, sizeof(body)) < 0) return send_json_err(req, "bad body");
    form_get(body, "cmd", cmd, sizeof(cmd));
    form_get(body, "ch", chs, sizeof(chs));
    form_get(body, "mode", mode, sizeof(mode));
    int ch = chs[0] ? atoi(chs) : -1;

    if (strcmp(cmd, "sync") == 0) {
        acq_request_sync();
    } else if (strcmp(cmd, "restart") == 0) {
        acq_settings_t s;
        config_lock();
        config_to_acq(&g_config, &s);
        config_unlock();
        acq_apply(&s);
    } else if (strcmp(cmd, "mux") == 0) {
        static const char *const modes[] = { "normal", "short", "testp", "testn" };
        int m = -1;
        for (int i = 0; i < 4; i++) if (strcmp(mode, modes[i]) == 0) m = i;
        if (ch < 0 || ch >= ACQ_NCH || m < 0) return send_json_err(req, "mux needs ch=0|1 and mode=normal|short|testp|testn");
        acq_request_mux((uint8_t)ch, (uint8_t)m);
    } else if (strcmp(cmd, "zero") == 0) {
        if (ch < 0 || ch >= ACQ_NCH) return send_json_err(req, "zero needs ch=0|1");
        acq_status_t st;
        acq_get_status(&st);
        if (!st.running) return send_json_err(req, "acquisition not running");
        config_lock();
        seismo_config_t c = g_config;
        long o = (long)c.ch[ch].ocal + lround(st.ch[ch].mean);
        if (o > 8388607) o = 8388607;
        if (o < -8388608) o = -8388608;
        c.ch[ch].ocal = (int32_t)o;
        esp_err_t err = config_commit(&c, true);
        config_unlock();
        if (err != ESP_OK) return send_json_err(req, "NVS write failed");
        char resp[64];
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"ocal\":%ld}", o);
        return send_json(req, resp);
    } else {
        return send_json_err(req, "cmd: sync, restart, mux or zero");
    }
    return send_json(req, "{\"ok\":true}");
}

/* ── Config ─────────────────────────────────────────────────────────────── */

static esp_err_t api_config_get_handler(httpd_req_t *req)
{
    if (!web_auth_ok(req)) return ESP_OK;
    char *buf = malloc(1536);
    if (!buf) return send_json_err(req, "no memory");
    char host[70], user[40];
    json_escape(host, sizeof(host), g_config.hostname);
    json_escape(user, sizeof(user), g_config.username);
    int n = snprintf(buf, 1536,
        "{\"ok\":true,\"api\":%d,\"hostname\":\"%s\",\"port\":%u,\"login\":%u,\"user\":\"%s\","
        "\"station\":\"%s\",\"network\":\"%s\",\"clk_src\":\"%s\",\"clk_hz\":%lu,\"osr\":%u,"
        "\"decim\":%u,\"taps\":%u,\"ref\":\"%s\",\"ref_edge\":\"%s\",\"rtc\":%u,\"capture\":\"%s\",\"ch\":[",
        SEISMO_API_VERSION, host, g_config.http_port, g_config.weblogin_enabled, user,
        g_config.station, g_config.network, s_clk_names[g_config.clk_src],
        (unsigned long)g_config.clk_hz, g_config.osr, g_config.decim, g_config.taps,
        timebase_ref_name((tb_ref_t)g_config.ref), g_config.ref_rising ? "rise" : "fall", g_config.rtc,
        capture_name(g_config.capture));
    for (int c = 0; c < ACQ_NCH; c++) {
        const seismo_ch_cfg_t *ch = &g_config.ch[c];
        char name[20];
        json_escape(name, sizeof(name), ch->name);
        n += snprintf(buf + n, 1536 - (size_t)n,
            "%s{\"name\":\"%s\",\"pga\":%u,\"phase\":%d,\"gcal\":%.6f,\"ocal\":%ld,"
            "\"div\":%.6f,\"amp\":%g,\"sens\":%g}",
            c ? "," : "", name, ch->pga, ch->phase, (double)ch->gcal, (long)ch->ocal,
            (double)ch->divider, (double)ch->amp_gain, (double)ch->sens);
    }
    n += snprintf(buf + n, 1536 - (size_t)n, "]}");
    esp_err_t r = (n >= 1536) ? send_json_err(req, "config overflow")
                              : (set_json_headers(req), httpd_resp_send(req, buf, n));
    free(buf);
    return r;
}

/* Every form field whose name is a known key is applied; the rest are
 * ignored. All or nothing: one bad value and nothing changes. */
static esp_err_t api_config_post_handler(httpd_req_t *req)
{
    if (!post_origin_ok(req) || !web_auth_ok(req)) return ESP_OK;

    static const char *const keys[] = {
        "hostname", "port", "login", "user", "pass", "station", "network",
        "clk_src", "clk_hz", "osr", "decim", "taps", "ref", "ref_edge", "rtc", "capture",
        "ch0_name", "ch0_pga", "ch0_phase", "ch0_gcal", "ch0_ocal", "ch0_div", "ch0_amp", "ch0_sens",
        "ch1_name", "ch1_pga", "ch1_phase", "ch1_gcal", "ch1_ocal", "ch1_div", "ch1_amp", "ch1_sens",
    };
    char *body = malloc(1024);
    if (!body) return send_json_err(req, "no memory");
    if (read_body(req, body, 1024) < 0) { free(body); return send_json_err(req, "bad body"); }

    char v[72], err_buf[120];
    bool reboot = false, acq = false;
    const char *msg = NULL;

    config_lock();
    seismo_config_t c = g_config;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (!form_get(body, keys[i], v, sizeof(v))) continue;
        if (config_set_kv(&c, keys[i], v, false, &reboot, &acq, &msg) != ESP_OK) {
            config_unlock();
            free(body);
            snprintf(err_buf, sizeof(err_buf), "%s", msg);
            return send_json_err(req, err_buf);
        }
    }
    free(body);
    esp_err_t err = config_commit(&c, acq);
    config_unlock();
    if (err != ESP_OK) return send_json_err(req, "NVS write failed");
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"reboot_needed\":%s,\"acq_restarted\":%s}",
             reboot ? "true" : "false", acq ? "true" : "false");
    return send_json(req, resp);
}

#define CFG_TEXT_MAX 2048

static esp_err_t api_config_export_handler(httpd_req_t *req)
{
    if (!web_auth_ok(req)) return ESP_OK;
    char *buf = malloc(CFG_TEXT_MAX);
    if (!buf) return send_json_err(req, "no memory");
    config_lock();
    int n = config_export_text(buf, CFG_TEXT_MAX);
    config_unlock();
    if (n < 0) { free(buf); return send_json_err(req, "export overflow"); }
    httpd_resp_set_type(req, "text/plain; charset=UTF-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"vseismo.cfg\"");
    esp_err_t r = httpd_resp_send(req, buf, n);
    free(buf);
    return r;
}

static esp_err_t api_config_import_handler(httpd_req_t *req)
{
    if (!post_origin_ok(req) || !web_auth_ok(req)) return ESP_OK;
    char *body = malloc(CFG_TEXT_MAX);
    if (!body) return send_json_err(req, "no memory");
    if (read_body(req, body, CFG_TEXT_MAX) < 0) {
        free(body);
        return send_json_err(req, "body missing or over 2 KB");
    }
    bool reboot = false, acq = false;
    int applied = 0, lineno = 0;
    const char *msg = NULL;
    char errbuf[120];

    config_lock();
    seismo_config_t c = g_config;
    char *next = body;
    while (next && *next) {
        char *line = next;
        next = strchr(line, '\n');
        if (next) *next++ = '\0';
        lineno++;
        size_t n = strlen(line);
        if (n && line[n - 1] == '\r') line[--n] = '\0';
        while (*line == ' ' || *line == '\t') line++;
        if (!*line || *line == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) { msg = "expected key=value"; break; }
        *eq = '\0';
        char *k = line, *ke = eq;
        while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = '\0';
        if (config_set_kv(&c, k, eq + 1, true, &reboot, &acq, &msg) != ESP_OK) break;
        applied++;
        msg = NULL;
    }
    free(body);
    if (msg) {
        config_unlock();
        snprintf(errbuf, sizeof(errbuf), "line %d: %s, nothing applied", lineno, msg);
        return send_json_err(req, errbuf);
    }
    esp_err_t err = config_commit(&c, acq);
    config_unlock();
    if (err != ESP_OK) return send_json_err(req, "NVS write failed");
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"applied\":%d,\"reboot_needed\":%s}",
             applied, reboot ? "true" : "false");
    return send_json(req, resp);
}

static esp_err_t api_logs_handler(httpd_req_t *req)
{
    if (!web_auth_ok(req)) return ESP_OK;
    char *buf = malloc(10240);
    if (!buf) return send_json_err(req, "no memory");
    int n = event_log_get_recent_json(buf, 10240);
    set_json_headers(req);
    esp_err_t r = httpd_resp_send(req, buf, n);
    free(buf);
    return r;
}

/* ── Runtime log levels (RAM only, reset on reboot) ─────────────────────── */

static const char *const s_log_tags[] = {
    "SEISMO", "ACQ", "ADS", "TIME", "DS323X", "WEB", "DNS", "FAULT", "CONSOLE"
};
static const char *const s_level_names[] = { "none", "error", "warn", "info", "debug" };

static esp_err_t api_loglevel_get_handler(httpd_req_t *req)
{
    if (!web_auth_ok(req)) return ESP_OK;
    esp_log_level_t d = esp_log_level_get("*");
    if (d > ESP_LOG_DEBUG) d = ESP_LOG_DEBUG;
    char buf[400];
    int n = snprintf(buf, sizeof(buf), "{\"ok\":true,\"default\":\"%s\",\"tags\":{", s_level_names[d]);
    for (size_t i = 0; i < sizeof(s_log_tags) / sizeof(s_log_tags[0]) && n < (int)sizeof(buf); i++) {
        esp_log_level_t l = esp_log_level_get(s_log_tags[i]);
        if (l > ESP_LOG_DEBUG) l = ESP_LOG_DEBUG;
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s\"%s\":\"%s\"", i ? "," : "",
                      s_log_tags[i], s_level_names[l]);
    }
    if (n >= (int)sizeof(buf) - 3) return send_json_err(req, "loglevel overflow");
    strcat(buf, "}}");
    return send_json(req, buf);
}

static bool log_tag_ok(const char *t)
{
    size_t n = strlen(t);
    if (n == 0 || n > 31) return false;
    if (strcmp(t, "*") == 0) return true;
    for (size_t i = 0; i < n; i++) {
        char c = t[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

esp_err_t seismo_log_level_set(const char *tag, const char *lvl, const char **err)
{
    static const char *dummy;
    if (!err) err = &dummy;
    if (!tag || !log_tag_ok(tag)) {
        *err = "tag: letters, digits, _ or -, 1-31, or *";
        return ESP_ERR_INVALID_ARG;
    }
    int level = -1;
    for (int i = 0; lvl && i < (int)(sizeof(s_level_names) / sizeof(s_level_names[0])); i++) {
        if (strcasecmp(lvl, s_level_names[i]) == 0) level = i;
    }
    if (level < 0) {
        *err = "level: none, error, warn, info or debug";
        return ESP_ERR_INVALID_ARG;
    }
    esp_log_level_set(tag, (esp_log_level_t)level);
    ESP_LOGW(TAG, "Log level for %s set to %s until reboot", tag, s_level_names[level]);
    return ESP_OK;
}

static esp_err_t api_loglevel_post_handler(httpd_req_t *req)
{
    if (!post_origin_ok(req) || !web_auth_ok(req)) return ESP_OK;
    char body[96];
    if (read_body(req, body, sizeof(body)) < 0) return send_json_err(req, "bad body");
    char tag[40] = "", lvl[12] = "";
    form_get(body, "tag", tag, sizeof(tag));
    if (!form_get(body, "level", lvl, sizeof(lvl))) return send_json_err(req, "level missing");
    const char *msg = NULL;
    if (seismo_log_level_set(tag, lvl, &msg) != ESP_OK) return send_json_err(req, msg);
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t api_reboot_handler(httpd_req_t *req)
{
    if (!post_origin_ok(req) || !web_auth_ok(req)) return ESP_OK;
    ESP_LOGW(TAG, "Reboot requested from web UI");
    send_json(req, "{\"ok\":true}");
    seismo_schedule_restart(800);
    return ESP_OK;
}

/* ── Firmware update ──────────────────────────────────────────────────────
 * Raw .bin in the request body, streamed into the inactive slot.
 * Acquisition pauses before the first flash write (cache-off windows would
 * otherwise show up as overruns). The header is checked from the first
 * chunk (chip, app descriptor magic, project name), so a wrong file is
 * refused before anything is erased. */

#define FW_BUF_SZ      4096
#define FW_MIN_SZ      0x20000
#define FW_DESC_OFFSET (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))

static esp_err_t fw_fail(httpd_req_t *req, esp_ota_handle_t ota, const char *msg)
{
    if (ota) esp_ota_abort(ota);
    s_ota_active = false;
    ESP_LOGE(TAG, "Firmware update failed: %s", msg);
    acq_settings_t s;
    config_to_acq(&g_config, &s);
    acq_apply(&s);                         /* resume acquisition */
    return send_json_err(req, msg);
}

static esp_err_t api_fw_info_handler(httpd_req_t *req)
{
    if (!web_auth_ok(req)) return ESP_OK;
    const esp_app_desc_t *d = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *nxt = esp_ota_get_next_update_partition(NULL);
    char buf[400];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"api\":%d,\"version\":\"%s\",\"project\":\"%s\",\"date\":\"%s\",\"time\":\"%s\","
             "\"idf\":\"%s\",\"running\":\"%s\",\"target\":\"%s\",\"slot\":%lu,\"state\":\"%s\"}",
             SEISMO_API_VERSION, d->version, d->project_name, d->date, d->time, d->idf_ver,
             run ? run->label : "?", nxt ? nxt->label : "none",
             (unsigned long)(nxt ? nxt->size : 0), ota_state_name());
    return send_json(req, buf);
}

static esp_err_t api_fw_upload_handler(httpd_req_t *req)
{
    if (!post_origin_ok(req) || !web_auth_ok(req)) return ESP_OK;
    if (s_ota_active) return send_json_err(req, "Update already in progress");

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    int total = (int)req->content_len;
    if (!target) return send_json_err(req, "No OTA partition: flash the dual-slot table over USB once");
    if (total < FW_MIN_SZ) return send_json_err(req, "File too small to be firmware");
    if ((size_t)total > target->size) return send_json_err(req, "Firmware larger than the slot");

    s_ota_active = true;
    fault_safe_outputs();
    ESP_LOGW(TAG, "Firmware upload: %d bytes -> %s", total, target->label);

    static uint8_t buf[FW_BUF_SZ];
    esp_ota_handle_t ota = 0;
    int received = 0, retries = 0;
    bool checked = false;
    const esp_app_desc_t *cur = esp_app_get_description();

    while (received < total) {
        int want = total - received;
        if (want > FW_BUF_SZ) want = FW_BUF_SZ;
        int r = httpd_req_recv(req, (char *)buf, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++retries < 10) continue;
        if (r <= 0) return fw_fail(req, ota, "Upload interrupted");
        retries = 0;

        if (!checked) {
            if (r < (int)(FW_DESC_OFFSET + sizeof(esp_app_desc_t))) return fw_fail(req, ota, "First chunk too short");
            const esp_image_header_t *ih = (const esp_image_header_t *)buf;
            const esp_app_desc_t *nd = (const esp_app_desc_t *)(buf + FW_DESC_OFFSET);
            if (ih->magic != ESP_IMAGE_HEADER_MAGIC) return fw_fail(req, ota, "Not an ESP image");
            if (ih->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) return fw_fail(req, ota, "Image built for another chip");
            if (nd->magic_word != ESP_APP_DESC_MAGIC_WORD) return fw_fail(req, ota, "No app descriptor");
            if (strncmp(nd->project_name, cur->project_name, sizeof(nd->project_name)) != 0) {
                return fw_fail(req, ota, "Project mismatch: not a vseismo image");
            }
            ESP_LOGI(TAG, "Incoming image: %s v%s", nd->project_name, nd->version);
            if (esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
                return fw_fail(req, 0, "esp_ota_begin failed");
            }
            checked = true;
        }
        if (esp_ota_write(ota, buf, (size_t)r) != ESP_OK) return fw_fail(req, ota, "Flash write failed");
        received += r;
    }

    esp_err_t err = esp_ota_end(ota);
    ota = 0;
    if (err != ESP_OK) {
        return fw_fail(req, 0, err == ESP_ERR_OTA_VALIDATE_FAILED ? "Image failed validation" : "esp_ota_end failed");
    }
    if (esp_ota_set_boot_partition(target) != ESP_OK) return fw_fail(req, 0, "Could not set boot partition");

    esp_app_desc_t nd;
    esp_ota_get_partition_description(target, &nd);
    ESP_LOGW(TAG, "Firmware v%s written to %s, rebooting into self-test", nd.version, target->label);
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"version\":\"%s\",\"reboot\":true}", nd.version);
    send_json(req, resp);
    seismo_schedule_restart(800);
    return ESP_OK;
}

/* ═════════════════════════════════════════════════════════════════════════
 * WiFi provisioning endpoints (both modes)
 * ═════════════════════════════════════════════════════════════════════════ */

static esp_err_t portal_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    return httpd_resp_send(req, (const char *)portal_html_start, portal_html_end - portal_html_start);
}

static esp_err_t api_wifi_scan_handler(httpd_req_t *req)
{
    if (g_wifi_mode == SEISMO_WIFI_STA && !web_auth_ok(req)) return ESP_OK;
    wifi_scan_config_t sc = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 80,
        .scan_time.active.max = 200,
    };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) return send_json_err(req, esp_err_to_name(err));

    uint16_t n = 20;
    wifi_ap_record_t *list = calloc(n, sizeof(*list));
    if (!list) {
        esp_wifi_clear_ap_list();
        return send_json_err(req, "no memory");
    }
    esp_wifi_scan_get_ap_records(&n, list);
    char *json = malloc(3072);
    if (!json) { free(list); return send_json_err(req, "no memory"); }
    size_t pos = (size_t)snprintf(json, 3072, "{\"ok\":true,\"networks\":[");
    for (int i = 0; i < n && pos < 3072 - 160; i++) {
        char ssid[70];
        json_escape(ssid, sizeof(ssid), (const char *)list[i].ssid);
        if (!ssid[0]) continue;
        pos += (size_t)snprintf(json + pos, 3072 - pos, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"ch\":%d,\"auth\":%d}",
                                pos > 24 ? "," : "", ssid, list[i].rssi, list[i].primary, list[i].authmode);
    }
    snprintf(json + pos, 3072 - pos, "]}");
    free(list);
    set_json_headers(req);
    err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

static esp_err_t api_wifi_status_handler(httpd_req_t *req)
{
    char ssid[70];
    json_escape(ssid, sizeof(ssid), g_config.wifi_ssid);
    char buf[200];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"mode\":\"%s\",\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"configured\":%u}",
             g_wifi_mode == SEISMO_WIFI_AP_PROVISIONING ? "ap" : "sta",
             g_wifi_connected ? "true" : "false", ssid, g_ip_address, g_config.wifi_configured);
    return send_json(req, buf);
}

static esp_err_t api_wifi_connect_handler(httpd_req_t *req)
{
    if (!post_origin_ok(req)) return ESP_OK;
    if (g_wifi_mode == SEISMO_WIFI_STA && !web_auth_ok(req)) return ESP_OK;
    char body[320];
    if (read_body(req, body, sizeof(body)) < 0) return send_json_err(req, "bad body");
    char ssid[96] = "", pass[200] = "";
    form_get(body, "ssid", ssid, sizeof(ssid));
    form_get(body, "password", pass, sizeof(pass));
    if (!ssid[0] || strlen(ssid) > 32) return send_json_err(req, "SSID must be 1-32 bytes");
    if (strlen(pass) > 63 || (pass[0] && strlen(pass) < 8)) {
        return send_json_err(req, "Password must be empty (open) or 8-63 characters");
    }
    config_lock();
    strcpy(g_config.wifi_ssid, ssid);
    strcpy(g_config.wifi_password, pass);
    g_config.wifi_configured = 1;
    esp_err_t err = config_save();
    config_unlock();
    if (err != ESP_OK) return send_json_err(req, "NVS write failed");
    ESP_LOGI(TAG, "WiFi credentials saved for \"%s\", rebooting", ssid);
    send_json(req, "{\"ok\":true,\"message\":\"Saved. Restarting...\"}");
    seismo_schedule_restart(1000);
    return ESP_OK;
}

static esp_err_t api_wifi_forget_handler(httpd_req_t *req)
{
    if (!post_origin_ok(req)) return ESP_OK;
    config_lock();
    g_config.wifi_ssid[0] = '\0';
    g_config.wifi_password[0] = '\0';
    g_config.wifi_configured = 0;
    esp_err_t err = config_save();
    config_unlock();
    if (err != ESP_OK) return send_json_err(req, "NVS write failed");
    return send_json(req, "{\"ok\":true,\"message\":\"WiFi credentials cleared\"}");
}

/* ── Captive portal redirects ─────────────────────────────────────────────
 * Each OS probes a well-known URL; a 302 to the portal makes it pop the
 * sign-in sheet. Common probe paths are registered, the rest hit the 404
 * handler. */

static esp_err_t redirect_to_portal(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" WIFI_AP_IP "/portal");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t portal_404_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    return redirect_to_portal(req);
}

static const char *const s_probe_paths[] = {
    "/generate_204", "/gen_204",
    "/hotspot-detect.html", "/library/test/success.html",
    "/connecttest.txt", "/ncsi.txt", "/redirect",
    "/canonical.html", "/success.txt",
};

/* ═════════════════════════════════════════════════════════════════════════
 * Server start
 * ═════════════════════════════════════════════════════════════════════════ */

#define URI(u, m, h) { .uri = (u), .method = (m), .handler = (h), .user_ctx = NULL }

static const httpd_uri_t s_common_uris[] = {
    URI("/portal",            HTTP_GET,  portal_handler),
    URI("/api/wifi/scan",     HTTP_GET,  api_wifi_scan_handler),
    URI("/api/wifi/status",   HTTP_GET,  api_wifi_status_handler),
    URI("/api/wifi/connect",  HTTP_POST, api_wifi_connect_handler),
};

static const httpd_uri_t s_normal_uris[] = {
    URI("/",                  HTTP_GET,  root_handler),
    URI("/api/status",        HTTP_GET,  api_status_handler),
    URI("/api/acq",           HTTP_GET,  api_acq_handler),
    URI("/api/wave",          HTTP_GET,  api_wave_handler),
    URI("/api/acq/cmd",       HTTP_POST, api_acq_cmd_handler),
    URI("/api/config",        HTTP_GET,  api_config_get_handler),
    URI("/api/config",        HTTP_POST, api_config_post_handler),
    URI("/api/config/export", HTTP_GET,  api_config_export_handler),
    URI("/api/config/import", HTTP_POST, api_config_import_handler),
    URI("/api/logs",          HTTP_GET,  api_logs_handler),
    URI("/api/loglevel",      HTTP_GET,  api_loglevel_get_handler),
    URI("/api/loglevel",      HTTP_POST, api_loglevel_post_handler),
    URI("/api/reboot",        HTTP_POST, api_reboot_handler),
    URI("/api/fw/info",       HTTP_GET,  api_fw_info_handler),
    URI("/api/fw/upload",     HTTP_POST, api_fw_upload_handler),
};

static esp_err_t start_httpd(uint16_t port, int max_handlers)
{
    if (s_server) return ESP_ERR_INVALID_STATE;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = port;
    cfg.max_uri_handlers = max_handlers;
    cfg.stack_size       = 8192;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;

    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }
    for (size_t i = 0; i < sizeof(s_common_uris) / sizeof(s_common_uris[0]); i++) {
        httpd_register_uri_handler(s_server, &s_common_uris[i]);
    }
    return ESP_OK;
}

esp_err_t webserver_start(void)
{
    esp_err_t err = start_httpd(g_config.http_port, 28);
    if (err != ESP_OK) return err;
    for (size_t i = 0; i < sizeof(s_normal_uris) / sizeof(s_normal_uris[0]); i++) {
        httpd_register_uri_handler(s_server, &s_normal_uris[i]);
    }
    ESP_LOGI(TAG, "HTTP server on port %u", g_config.http_port);
    return ESP_OK;
}

esp_err_t webserver_start_portal_mode(void)
{
    esp_err_t err = start_httpd(80, 20);
    if (err != ESP_OK) return err;
    static const httpd_uri_t root   = URI("/",                HTTP_GET,  redirect_to_portal);
    static const httpd_uri_t forget = URI("/api/wifi/forget", HTTP_POST, api_wifi_forget_handler);
    httpd_register_uri_handler(s_server, &root);
    httpd_register_uri_handler(s_server, &forget);
    static httpd_uri_t probes[sizeof(s_probe_paths) / sizeof(s_probe_paths[0])];
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        probes[i] = (httpd_uri_t)URI(s_probe_paths[i], HTTP_GET, redirect_to_portal);
        httpd_register_uri_handler(s_server, &probes[i]);
    }
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, portal_404_handler);
    ESP_LOGI(TAG, "Portal HTTP server on port 80");
    return ESP_OK;
}
