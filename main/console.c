/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Vlad Patoka (VE3VPT) */
/**
 * @file console.c
 * @brief Command console on the USB Serial/JTAG port. See console.h.
 *
 * Commands only read snapshots (acq_get_status, timebase_get_status) or go
 * through the shared config setters; nothing here touches SPI or I2C.
 * Log output shares the port, so lines can interleave with the prompt.
 */

#include "console.h"
#include "acq.h"
#include "fault.h"
#include "timebase.h"
#include "web_server.h"

#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "esp_app_desc.h"
#include "esp_console.h"
#include "linenoise/linenoise.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "sdkconfig.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"

static const char *TAG = "CONSOLE";

static int cmd_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    const esp_app_desc_t *app = esp_app_get_description();
    acq_status_t a;
    acq_get_status(&a);
    tb_status_t t;
    timebase_get_status(&t);

    printf("firmware   %s (%s %s), IDF %s, api %d\n",
           app->version, app->date, app->time, app->idf_ver, SEISMO_API_VERSION);
    seismo_hw_t hw;
    seismo_hw_get(&hw);
    printf("uptime     %lld s, reset %s\n", (long long)(esp_timer_get_time() / 1000000), hw.reset_reason);
    printf("chip       ESP32-C5 v%u.%u, %u core, %lu MHz, flash %s, MAC " MACSTR "\n",
           hw.chip_rev / 100, hw.chip_rev % 100, hw.cores, (unsigned long)hw.cpu_mhz,
           CONFIG_ESPTOOLPY_FLASHSIZE, MAC2STR(hw.mac));
    printf("internal   %lu KB free of %lu KB, lowest %lu KB, largest block %lu KB\n",
           (unsigned long)(hw.int_free / 1024), (unsigned long)(hw.int_total / 1024),
           (unsigned long)(hw.int_min / 1024), (unsigned long)(hw.int_largest / 1024));
    if (hw.psram_total) {
        printf("psram      %lu KB free of %lu KB, lowest %lu KB, largest block %lu KB\n",
               (unsigned long)(hw.psram_free / 1024), (unsigned long)(hw.psram_total / 1024),
               (unsigned long)(hw.psram_min / 1024), (unsigned long)(hw.psram_largest / 1024));
    } else {
        printf("psram      none\n");
    }
    printf("stack free acq %lu, timebase %lu, httpd %lu, tcpip %lu, wifi %lu bytes\n",
           (unsigned long)hw.stack_acq, (unsigned long)hw.stack_timebase, (unsigned long)hw.stack_httpd,
           (unsigned long)hw.stack_tcpip, (unsigned long)hw.stack_wifi);
    printf("station    %s.%s\n", g_config.network, g_config.station);

    printf("adc        %s, ID 0x%04X, clock %s, %lu Hz on CLKIN\n",
           a.adc_ok ? "ok" : "NOT FOUND", a.adc_id, a.clock_ok ? "running" : "missing",
           (unsigned long)a.clk_hz_actual);
    printf("rate       %.4f SPS in, %.4f SPS out, delay %.1f ms, %llu in, seq %lu\n",
           a.fs_in, a.fs_out, a.group_delay_s * 1000.0, (unsigned long long)a.n_in, (unsigned long)a.seq);
    printf("errors     CRC %lu, overruns %lu, restarts %lu\n",
           (unsigned long)a.crc_errors, (unsigned long)a.overruns, (unsigned long)a.restarts);
    for (int c = 0; c < ACQ_NCH; c++) {
        const seismo_ch_cfg_t *cc = &g_config.ch[c];
        double uv = a.ch[c].rms * 1.2e6 / cc->pga / 8388608.0;
        printf("ch%d %-6s  PGA %3u  mean %10.1f  rms %9.2f cnt (%.3f uV)  min %ld  max %ld  clips %lu%s%s\n",
               c, cc->name, cc->pga, a.ch[c].mean, a.ch[c].rms, uv,
               (long)a.ch[c].min, (long)a.ch[c].max, (unsigned long)a.ch[c].clips,
               a.mux[c] ? "  mux=" : "", a.mux[c] ? acq_mux_name(a.mux[c]) : "");
    }

    if (t.time_valid) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        struct tm u;
        gmtime_r(&tv.tv_sec, &u);
        printf("utc        %04d-%02d-%02d %02d:%02d:%02d.%03ld (%s)\n",
               u.tm_year + 1900, u.tm_mon + 1, u.tm_mday, u.tm_hour, u.tm_min, u.tm_sec,
               (long)(tv.tv_usec / 1000), t.time_src);
    } else {
        printf("utc        no wall-clock time yet\n");
    }
    {
        static const char *const what[] = {
            [TB_TQ_NONE] = "none, samples carry no UTC",
            [TB_TQ_RTC]  = "rtc, RTC seed without NTP",
            [TB_TQ_NTP]  = "ntp, system clock slewed at 500 ppm max",
            [TB_TQ_PPS]  = "pps, sample count against GPS PPS",
        };
        printf("timestamps %s", what[t.quality <= TB_TQ_PPS ? t.quality : TB_TQ_NONE]);
        if (fabs(t.slew_ms) >= 0.05) printf(", slewing %+.1f ms", t.slew_ms);
        if (t.pps_naming_errors) printf(", PPS second renamed %lux", (unsigned long)t.pps_naming_errors);
        printf("\n");
    }
    printf("ntp        %lu syncs, last %lld s ago, %lu rejected\n", (unsigned long)t.ntp_syncs,
           (long long)t.ntp_age_s, (unsigned long)t.ntp_rejected);
    if (t.ref != TB_REF_NONE) {
        printf("1 Hz ref   %s, %s, %lu edges", timebase_ref_name(t.ref), t.ref_seen ? "seen" : "not seen",
               (unsigned long)t.ref_edges);
        if (t.rate_valid) printf(", ADC %.4f SPS (%+.3f +- %.4f ppm), ESP %+.3f ppm",
                                 t.fs_measured, t.adc_ppm, t.rate_sigma_ppm, t.esp_ppm);
        printf("\n");
        printf("rate fit   %lu of 300 edges%s, %lu rejected since boot\n", (unsigned long)t.rate_points,
               t.rate_valid ? "" : " (measuring, 10 needed)", (unsigned long)t.rate_rejects);
    } else {
        printf("1 Hz ref   none (rate is nominal)\n");
    }
    if (t.rtc_enabled) {
        printf("rtc        %s, %s, OSF %s, %.2f C\n", t.rtc_model, t.rtc_online ? "online" : "offline",
               t.rtc_osf ? "set" : "clear", (double)t.rtc_temp_c);
    }

    if (g_wifi_mode == SEISMO_WIFI_AP_PROVISIONING) {
        printf("wifi       setup AP, portal http://192.168.4.1/portal\n");
    } else {
        if (hw.wifi_up) {
            printf("wifi       %s, RSSI %d dBm, channel %u (%s GHz), %s, AP " MACSTR "\n",
                   g_config.wifi_ssid, hw.rssi, hw.channel, hw.channel > 14 ? "5" : "2.4", hw.phy,
                   MAC2STR(hw.bssid));
        } else {
            printf("wifi       %s, not connected\n", g_config.wifi_ssid);
        }
        printf("radio      TX power max %.1f dBm, %lu disconnects since boot",
               (double)hw.tx_power_dbm, (unsigned long)hw.wifi_disconnects);
        if (hw.wifi_disconnects) printf(" (last reason %u)", hw.wifi_last_reason);
        printf("\n");
        printf("ip         %s\n", g_ip_address[0] ? g_ip_address : "(none)");
        printf("web        http://%s:%u/\n", g_ip_address[0] ? g_ip_address : g_config.hostname,
               g_config.http_port);
    }
    printf("faults     0x%04lx%s\n", (unsigned long)fault_get_mask(), fault_active() ? " (class A latched)" : "");
    return 0;
}

static int cmd_config(int argc, char **argv)
{
    (void)argc; (void)argv;
    static char buf[2048];
    config_lock();
    int n = config_export_text(buf, sizeof(buf));
    config_unlock();
    if (n < 0) {
        printf("export overflow\n");
        return 1;
    }
    fputs(buf, stdout);
    return 0;
}

static int cmd_set(int argc, char **argv)
{
    if (argc != 3) {
        printf("usage: set <key> <value>   (quote values with spaces)\n"
               "keys:  hostname port login user pass station network clk_src clk_hz osr decim\n"
               "       taps ref ref_edge rtc wifi_ssid wifi_pass\n"
               "       chN_name chN_pga chN_phase chN_gcal chN_ocal chN_div chN_amp chN_sens (N = 0, 1)\n");
        return 1;
    }
    bool reboot = false, acq = false;
    const char *msg = NULL;
    config_lock();
    seismo_config_t c = g_config;
    esp_err_t err = config_set_kv(&c, argv[1], argv[2], true, &reboot, &acq, &msg);
    if (err == ESP_OK) err = config_commit(&c, acq);
    config_unlock();
    if (msg) {
        printf("error: %s\n", msg);
        return 1;
    }
    if (err != ESP_OK) {
        printf("error: NVS write failed\n");
        return 1;
    }
    printf("ok%s%s\n", acq ? ", acquisition restarted" : "", reboot ? ", takes effect after reboot" : "");
    return 0;
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        printf("usage: wifi <ssid> [password]   (no password: open network; reboots)\n");
        return 1;
    }
    const char *msg = NULL;
    config_lock();
    seismo_config_t c = g_config;
    esp_err_t err = config_set_kv(&c, "wifi_ssid", argv[1], true, NULL, NULL, &msg);
    if (err == ESP_OK) err = config_set_kv(&c, "wifi_pass", argc == 3 ? argv[2] : "", true, NULL, NULL, &msg);
    if (err == ESP_OK) err = config_commit(&c, false);
    config_unlock();
    if (err != ESP_OK) {
        printf("error: %s\n", msg ? msg : "NVS write failed");
        return 1;
    }
    printf("saved, rebooting\n");
    ESP_LOGW(TAG, "WiFi credentials set from console for \"%s\"", argv[1]);
    seismo_schedule_restart(500);
    return 0;
}

static int cmd_loglevel(int argc, char **argv)
{
    if (argc != 3) {
        printf("usage: loglevel <tag|*> <none|error|warn|info|debug>   (until reboot)\n"
               "tags:  SEISMO ACQ ADS TIME DS323X WEB DNS FAULT CONSOLE, or any IDF tag\n");
        return 1;
    }
    const char *msg = NULL;
    if (seismo_log_level_set(argv[1], argv[2], &msg) != ESP_OK) {
        printf("error: %s\n", msg);
        return 1;
    }
    printf("ok\n");
    return 0;
}

static int cmd_sync(int argc, char **argv)
{
    (void)argc; (void)argv;
    acq_status_t a;
    acq_get_status(&a);
    if (!a.running) {
        printf("acquisition not running: no SYNC sent\n");
        return 1;
    }
    acq_request_sync();
    printf("SYNC pulse requested\n");
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("rebooting\n");
    ESP_LOGW(TAG, "Reboot from console");
    seismo_schedule_restart(200);
    return 0;
}

static int cmd_wifireset(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("clearing WiFi credentials, rebooting into the setup AP\n");
    config_wifi_reset();
    return 0;
}

static int cmd_factory(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "yes") != 0) {
        printf("erases every setting, WiFi included. Type: factory yes\n");
        return 1;
    }
    printf("factory reset\n");
    config_factory_reset();
    return 0;
}

/* When the terminal does not answer the escape-sequence probe within its
 * 500 ms (idf_monitor is often still busy writing at boot), the console
 * runs in dumb mode and the late answer, ESC [ 0 n, stays queued as input.
 * Dumb mode drops the ESC but keeps "[0n", which glues onto the first
 * command. Arrow keys leave "[A" and the like the same way. In dumb mode
 * stdin is read through this filter: whole escape sequences are dropped,
 * everything else passes. Smart terminals keep linenoise's own reader,
 * which needs those sequences for editing. */
static ssize_t console_read(int fd, void *buf, size_t len)
{
    static enum { IN_TEXT, IN_ESC, IN_CSI } s_state = IN_TEXT;
    uint8_t *b = buf;
    for (;;) {
        ssize_t n = read(fd, buf, len);
        if (n <= 0) return n;
        size_t kept = 0;
        for (ssize_t i = 0; i < n; i++) {
            const uint8_t c = b[i];
            switch (s_state) {
                case IN_TEXT:
                    if (c == 0x1B) s_state = IN_ESC;
                    else b[kept++] = c;
                    break;
                case IN_ESC:                   /* ESC [ starts a CSI; ESC x is a two-byte escape */
                    s_state = c == '[' ? IN_CSI : IN_TEXT;
                    break;
                case IN_CSI:
                    if (c >= 0x40 && c <= 0x7E) {
                        s_state = IN_TEXT;     /* final byte ends it */
                    } else if (c < 0x20 || c > 0x3F) {
                        s_state = IN_TEXT;     /* malformed: keep the byte (an Enter must not vanish) */
                        b[kept++] = c;
                    }                          /* 0x20-0x3F: parameters and intermediates */
                    break;
            }
        }
        if (kept) return (ssize_t)kept;        /* all filtered: read again, never hand back 0 */
    }
}

static const esp_console_cmd_t s_cmds[] = {
    { .command = "status",    .help = "ADC, rates, channel stats, time, WiFi, faults",   .func = cmd_status },
    { .command = "config",    .help = "Show settings as key=value (same as /api/config/export)", .func = cmd_config },
    { .command = "set",       .help = "Change one setting", .hint = "<key> <value>",   .func = cmd_set },
    { .command = "wifi",      .help = "Set WiFi credentials and reboot", .hint = "<ssid> [password]", .func = cmd_wifi },
    { .command = "sync",      .help = "Pulse ADC SYNC (restart both channel filters together)", .func = cmd_sync },
    { .command = "loglevel",  .help = "Set a log level until reboot", .hint = "<tag|*> <level>", .func = cmd_loglevel },
    { .command = "reboot",    .help = "Restart the station",                            .func = cmd_reboot },
    { .command = "wifireset", .help = "Forget WiFi, reboot into the setup AP",          .func = cmd_wifireset },
    { .command = "factory",   .help = "Erase all settings", .hint = "yes",              .func = cmd_factory },
};

esp_err_t console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "vseismo> ";
    rc.task_stack_size = 6144;
    rc.max_cmdline_length = 160;

    esp_console_dev_usb_serial_jtag_config_t hw = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&hw, &rc, &repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Console init: %s", esp_err_to_name(err));
        return err;
    }
    /* The probe has run. In dumb mode, filter stray escape sequences. IDF's
     * own reader is a select() that only exists so esp_console_stop_repl()
     * can interrupt it; we never stop the REPL, so a blocking read does. */
    if (linenoiseIsDumbMode()) {
        const int fd = fileno(stdin);
        const int flags = fcntl(fd, F_GETFL);
        if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        linenoiseSetReadFunction(console_read);
    }
    esp_console_register_help_command();
    for (size_t i = 0; i < sizeof(s_cmds) / sizeof(s_cmds[0]); i++) esp_console_cmd_register(&s_cmds[i]);
    err = esp_console_start_repl(repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Console start: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "USB console ready: type \"help\"");
    return ESP_OK;
}
