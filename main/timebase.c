/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Vlad Patoka (VE3VPT) */
/**
 * @file timebase.c
 * @brief SNTP, optional DS323x holdover, 1 Hz rate measurement. See timebase.h.
 *
 * Rate loop: at each reference edge the ADC position is interpolated
 *   pos = n + (t_edge - t_drdy) * fs_nominal
 * where n and t_drdy are the latest DRDY count and time. Two edges one
 * reference second apart give the true sample count per second. The
 * interpolation error is a fraction of one sample period; smoothed over
 * many seconds it settles to well under 1 ppm with GPS PPS.
 *
 * SNTP runs in immediate mode: each hourly sync may step the system clock
 * by a few ms. Timestamps follow the step; the sample stream does not.
 */

#include "timebase.h"
#include "acq.h"
#include "ds323x.h"
#include "fault.h"
#include "io_map.h"

#include <math.h>
#include <string.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "TIME";

#define REF_LOST_MS        3000
#define RATE_EMA_ALPHA     0.1
#define RTC_POLL_S         60

static TaskHandle_t s_task;
static volatile int64_t s_edge_t;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static tb_status_t s_st = { .time_src = "none", .ntp_age_s = -1, .ref_age_ms = -1, .rtc_model = "none" };
static volatile bool s_rtc_write_req;
static volatile int64_t s_ntp_last_us;
static bool s_sntp_started;
static bool s_use_rtc;

static void IRAM_ATTR ref_isr(void *arg)
{
    (void)arg;
    s_edge_t = esp_timer_get_time();
    BaseType_t woken = pdFALSE;
    if (s_task) vTaskNotifyGiveFromISR(s_task, &woken);
    portYIELD_FROM_ISR(woken);
}

const char *timebase_ref_name(tb_ref_t r)
{
    switch (r) {
        case TB_REF_GPS: return "gps";
        case TB_REF_RTC: return "rtc";
        default:         return "none";
    }
}

bool timebase_time_valid(void)
{
    return s_st.time_valid;
}

int64_t timebase_esp_to_utc_us(int64_t t_esp)
{
    if (!s_st.time_valid) return 0;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t now_esp = esp_timer_get_time();
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec + (t_esp - now_esp);
}

void timebase_get_status(tb_status_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
    int64_t now = esp_timer_get_time();
    int64_t ntp = s_ntp_last_us;
    out->ntp_age_s = ntp ? (now - ntp) / 1000000 : -1;
    int64_t e = s_edge_t;
    out->ref_age_ms = (out->ref_seen && e) ? (now - e) / 1000 : -1;
}

/* ── SNTP ───────────────────────────────────────────────────────────────── */

static void sntp_synced(struct timeval *tv)
{
    (void)tv;
    s_ntp_last_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    s_st.ntp_syncs++;
    s_st.time_valid = true;
    s_st.time_src = "ntp";
    portEXIT_CRITICAL(&s_mux);
    fault_clear(FAULT_NO_TIME);
    s_rtc_write_req = true;
    if (s_task) xTaskNotifyGive(s_task);
    ESP_LOGI(TAG, "SNTP sync #%lu", (unsigned long)s_st.ntp_syncs);
}

void timebase_start_sntp(void)
{
    if (s_sntp_started) return;
    s_sntp_started = true;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(4,
        ESP_SNTP_SERVER_LIST(CONFIG_SEISMO_NTP_SERVER_1, CONFIG_SEISMO_NTP_SERVER_2,
                             CONFIG_SEISMO_NTP_SERVER_3, CONFIG_SEISMO_NTP_SERVER_4));
    cfg.sync_cb = sntp_synced;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) ESP_LOGE(TAG, "SNTP init: %s", esp_err_to_name(err));
    else ESP_LOGI(TAG, "SNTP started");
}

/* ── RTC ────────────────────────────────────────────────────────────────── */

static void rtc_seed(void)
{
    time_t utc;
    bool osf = true;
    if (ds323x_read_time(&utc, &osf) != ESP_OK) return;
    s_st.rtc_osf = osf;
    if (osf) {
        fault_raise(FAULT_RTC_LOST_POWER);
        ESP_LOGW(TAG, "RTC oscillator stopped at some point: not seeding the clock");
        return;
    }
    if (s_st.time_valid) return;
    struct timeval tv = { .tv_sec = utc, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    portENTER_CRITICAL(&s_mux);
    s_st.time_valid = true;
    s_st.time_src = "rtc";
    portEXIT_CRITICAL(&s_mux);
    fault_clear(FAULT_NO_TIME);
    ESP_LOGI(TAG, "System clock seeded from the RTC (holdover until SNTP)");
}

/* Write the RTC at a second boundary so its sub-second phase matches UTC. */
static void rtc_write_from_system(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint32_t wait_ms = (uint32_t)((1000000 - tv.tv_usec) / 1000);
    vTaskDelay(pdMS_TO_TICKS(wait_ms ? wait_ms : 1));
    gettimeofday(&tv, NULL);
    time_t sec = tv.tv_sec + (tv.tv_usec > 500000 ? 1 : 0);
    if (ds323x_write_time(sec) == ESP_OK) {
        s_st.rtc_osf = false;
        fault_clear(FAULT_RTC_LOST_POWER);
        ESP_LOGI(TAG, "RTC set from SNTP");
    }
}

static void rtc_poll(void)
{
    if (!ds323x_online()) {
        if (ds323x_recover() == ESP_OK && ds323x_configure() == ESP_OK) {
            fault_clear(FAULT_RTC_MISSING);
            ESP_LOGI(TAG, "RTC back online");
        }
    }
    int16_t q;
    if (ds323x_online() && ds323x_read_temp_q(&q) == ESP_OK) s_st.rtc_temp_c = q / 4.0f;
    s_st.rtc_online = ds323x_online();
    s_st.rtc_model = ds323x_model_name();
}

/* ── Rate loop ──────────────────────────────────────────────────────────── */

static void on_edge(int64_t t_edge)
{
    static int64_t prev_t;
    static double prev_pos;
    static uint32_t prev_epoch;
    static bool have_prev;

    uint64_t n;
    int64_t t_d;
    uint32_t epoch;
    acq_get_position(&n, &t_d, &epoch);
    double fs = acq_fs_in();

    portENTER_CRITICAL(&s_mux);
    s_st.ref_edges++;
    s_st.ref_seen = true;
    portEXIT_CRITICAL(&s_mux);
    fault_clear(FAULT_REF_LOST);

    if (fs <= 0 || t_d == 0) { have_prev = false; return; }
    double pos = (double)n + (double)(t_edge - t_d) * fs / 1e6;

    if (have_prev && epoch == prev_epoch) {
        int64_t dt = t_edge - prev_t;
        int64_t k = (dt + 500000) / 1000000;           /* reference seconds elapsed */
        if (k >= 1 && k <= 5 && llabs(dt - k * 1000000) < 20000) {
            double rate = (pos - prev_pos) / (double)k;
            double esp_ppm = ((double)dt / (double)k - 1e6);   /* us per s = ppm */
            portENTER_CRITICAL(&s_mux);
            if (!s_st.rate_valid) {
                s_st.fs_measured = rate;
                s_st.esp_ppm = esp_ppm;
                s_st.rate_valid = true;
            } else {
                s_st.fs_measured += RATE_EMA_ALPHA * (rate - s_st.fs_measured);
                s_st.esp_ppm += RATE_EMA_ALPHA * (esp_ppm - s_st.esp_ppm);
            }
            s_st.adc_ppm = (s_st.fs_measured / fs - 1.0) * 1e6;
            portEXIT_CRITICAL(&s_mux);
        } else if (k >= 1) {
            ESP_LOGW(TAG, "Reference edge spacing %lld us: ignored", (long long)dt);
        }
    } else {
        portENTER_CRITICAL(&s_mux);
        s_st.rate_valid = false;
        portEXIT_CRITICAL(&s_mux);
    }
    prev_t = t_edge;
    prev_pos = pos;
    prev_epoch = epoch;
    have_prev = true;
}

static void timebase_task(void *arg)
{
    (void)arg;
    int64_t last_rtc_poll = 0;
    int64_t last_edge_seen = 0;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        int64_t now = esp_timer_get_time();

        int64_t e = s_edge_t;
        if (s_st.ref != TB_REF_NONE && e && e != last_edge_seen) {
            last_edge_seen = e;
            on_edge(e);
        }
        if (s_st.ref_seen && now - last_edge_seen > (int64_t)REF_LOST_MS * 1000) {
            fault_raise(FAULT_REF_LOST);
            s_st.rate_valid = false;
        }

        if (s_use_rtc) {
            if (s_rtc_write_req && ds323x_online()) {
                s_rtc_write_req = false;
                rtc_write_from_system();
            }
            if (now - last_rtc_poll > (int64_t)RTC_POLL_S * 1000000) {
                last_rtc_poll = now;
                rtc_poll();
            }
        }
    }
}

esp_err_t timebase_init(tb_ref_t ref, bool rising, bool use_rtc, i2c_master_bus_handle_t bus)
{
    s_st.ref = ref;
    s_st.ref_rising = rising;
    s_st.rtc_enabled = use_rtc && bus;
    s_use_rtc = s_st.rtc_enabled;
    fault_raise(FAULT_NO_TIME);

    if (s_use_rtc) {
        if (ds323x_init(bus) == ESP_OK && ds323x_configure() == ESP_OK) {
            s_st.rtc_online = true;
            s_st.rtc_model = ds323x_model_name();
            rtc_seed();
        } else {
            fault_raise(FAULT_RTC_MISSING);
        }
    }

    if (ref != TB_REF_NONE) {
        const gpio_config_t io = {
            .pin_bit_mask = 1ULL << REF_1HZ_GPIO,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,   /* SQW is open drain; GPS PPS ignores it */
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = rising ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE,
        };
        gpio_config(&io);
        gpio_isr_handler_add(REF_1HZ_GPIO, ref_isr, NULL);
        ESP_LOGI(TAG, "1 Hz reference: %s, %s edge on GPIO%d", timebase_ref_name(ref),
                 rising ? "rising" : "falling", REF_1HZ_GPIO);
    } else {
        ESP_LOGI(TAG, "No 1 Hz reference: sample rate is the nominal one");
    }

    if (xTaskCreate(timebase_task, "timebase", 3584, NULL, PRIO_TIMEBASE, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
