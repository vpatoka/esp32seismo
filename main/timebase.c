/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Vlad Patoka (VE3VPT) */
/**
 * @file timebase.c
 * @brief SNTP, optional DS323x holdover, 1 Hz rate measurement. See timebase.h.
 *
 * Rate loop: at each reference edge the ADC position is interpolated
 *   pos = n + (t_edge - t_drdy) * fs_nominal
 * where n and t_drdy are the latest DRDY count and time. pos is a phase
 * measurement against reference second x. The rate is the slope of a
 * least-squares line through (x, pos) over the last RATE_WIN edges, not
 * an average of second-to-second differences: with white timing jitter
 * sigma_pos per point, differencing gives sigma_f ~ alpha * sigma_pos
 * (1.4 ppm at 10 us jitter, alpha 0.1), the fit gives
 *   sigma_f = sigma_pos * sqrt(12 / N^3)
 * which is about 0.01 ppm at N = 300 from the same edges. The same fit of
 * the edge times themselves gives the ESP crystal error.
 *
 * Edges at the wrong spacing (not a whole number of seconds) are ignored.
 * Edges at the right spacing but far off the fit are outliers; a run of
 * them means the rate itself moved, and the fit restarts. A restart of
 * acquisition (epoch change) or a reference gap over RATE_MAX_SKIP seconds
 * restarts it too.
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
#include <stdlib.h>
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
#define RTC_POLL_S         60

#define RATE_WIN           300     /* reference edges in the fit window */
#define RATE_MIN_PTS       10      /* edges before the rate is reported */
#define RATE_MAX_SKIP      30      /* missed edges bridged without a restart */
#define RATE_SPACING_US    20000   /* edge spacing tolerance against whole seconds */
#define RATE_OUTLIER_SMP   0.5     /* off the fit by this many samples ... */
#define RATE_OUTLIER_US    200.0   /* ... or this much time, whichever is larger */
#define RATE_RESET_AFTER   5       /* consecutive outliers: the rate moved, restart */

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

/* Fit state. Points are stored as residuals against the nominal rate so
 * the sums stay small: y = pos - pos0 - fs * x (samples), e = t - t0 - 1e6 x
 * (us). Slopes of y and e against x are then the rate error in samples per
 * second and the ESP timer error in us per second (= ppm). */
static struct {
    double   x[RATE_WIN], y[RATE_WIN], e[RATE_WIN];
    int      head, n;
    bool     active;
    uint32_t epoch;
    double   fs;            /* nominal rate the residuals are taken against */
    double   pos0;
    int64_t  t0;
    double   x_last;        /* reference second of the last accepted or outlier edge */
    int64_t  t_last;
    double   a, b;          /* y = a + b x from the last solve */
    bool     have_line;
    int      bad_run;
} s_fit;

static void fit_add(double x, double y, double e)
{
    s_fit.x[s_fit.head] = x;
    s_fit.y[s_fit.head] = y;
    s_fit.e[s_fit.head] = e;
    s_fit.head = (s_fit.head + 1) % RATE_WIN;
    if (s_fit.n < RATE_WIN) s_fit.n++;
}

static void fit_start(double pos, int64_t t, double fs, uint32_t epoch)
{
    memset(&s_fit, 0, sizeof(s_fit));
    s_fit.active = true;
    s_fit.epoch = epoch;
    s_fit.fs = fs;
    s_fit.pos0 = pos;
    s_fit.t0 = t;
    s_fit.t_last = t;
    fit_add(0.0, 0.0, 0.0);
}

/* Least-squares slopes of y and e against x over the window. Two passes
 * (means, then centred sums): cheap at 1 Hz and free of cancellation. */
static bool fit_solve(double *b_y, double *b_e, double *sigma_b)
{
    int n = s_fit.n;
    if (n < 3) return false;
    double mx = 0, my = 0, me = 0;
    for (int i = 0; i < n; i++) { mx += s_fit.x[i]; my += s_fit.y[i]; me += s_fit.e[i]; }
    mx /= n; my /= n; me /= n;
    double sxx = 0, sxy = 0, sxe = 0;
    for (int i = 0; i < n; i++) {
        double dx = s_fit.x[i] - mx;
        sxx += dx * dx;
        sxy += dx * (s_fit.y[i] - my);
        sxe += dx * (s_fit.e[i] - me);
    }
    if (sxx <= 0) return false;
    double b = sxy / sxx, a = my - b * mx, ss = 0;
    for (int i = 0; i < n; i++) {
        double r = s_fit.y[i] - a - b * s_fit.x[i];
        ss += r * r;
    }
    s_fit.a = a;
    s_fit.b = b;
    s_fit.have_line = true;
    *b_y = b;
    *b_e = sxe / sxx;
    *sigma_b = sqrt(ss / (n - 2) / sxx);
    return true;
}

static void rate_invalidate(void)
{
    portENTER_CRITICAL(&s_mux);
    s_st.rate_valid = false;
    s_st.rate_points = s_fit.n;
    portEXIT_CRITICAL(&s_mux);
}

static void on_edge(int64_t t_edge)
{
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

    if (fs <= 0 || t_d == 0) { s_fit.active = false; s_fit.n = 0; rate_invalidate(); return; }
    double pos = (double)n + (double)(t_edge - t_d) * fs / 1e6;

    if (!s_fit.active || epoch != s_fit.epoch || fs != s_fit.fs) {
        fit_start(pos, t_edge, fs, epoch);
        rate_invalidate();
        return;
    }

    int64_t dt = t_edge - s_fit.t_last;
    int64_t k = (dt + 500000) / 1000000;               /* reference seconds since the last edge */
    if (k < 1 || llabs(dt - k * 1000000) >= RATE_SPACING_US) {
        ESP_LOGW(TAG, "Reference edge spacing %lld us: ignored", (long long)dt);
        return;                                        /* a glitch: keep the last good edge */
    }
    if (k > RATE_MAX_SKIP) {
        ESP_LOGW(TAG, "Reference gap of %lld s: rate fit restarted", (long long)k);
        fit_start(pos, t_edge, fs, epoch);
        rate_invalidate();
        return;
    }

    double x = s_fit.x_last + (double)k;
    double y = pos - s_fit.pos0 - fs * x;
    double e = (double)(t_edge - s_fit.t0) - 1e6 * x;
    s_fit.x_last = x;
    s_fit.t_last = t_edge;

    if (s_fit.have_line && s_fit.n >= RATE_MIN_PTS) {
        double r = y - (s_fit.a + s_fit.b * x);
        double lim = fmax(RATE_OUTLIER_SMP, RATE_OUTLIER_US * 1e-6 * fs);
        if (fabs(r) > lim) {
            portENTER_CRITICAL(&s_mux);
            s_st.rate_rejects++;
            portEXIT_CRITICAL(&s_mux);
            if (++s_fit.bad_run >= RATE_RESET_AFTER) {
                ESP_LOGW(TAG, "%d edges in a row off the rate fit: restarted", RATE_RESET_AFTER);
                fit_start(pos, t_edge, fs, epoch);
                rate_invalidate();
            } else {
                ESP_LOGW(TAG, "Reference edge %.3f samples off the rate fit: ignored", r);
            }
            return;
        }
    }
    s_fit.bad_run = 0;
    fit_add(x, y, e);

    double by, be, sb;
    if (!fit_solve(&by, &be, &sb) || s_fit.n < RATE_MIN_PTS) { rate_invalidate(); return; }
    portENTER_CRITICAL(&s_mux);
    s_st.rate_valid = true;
    s_st.fs_measured = fs + by;
    s_st.adc_ppm = by / fs * 1e6;
    s_st.esp_ppm = be;
    s_st.rate_sigma_ppm = sb / fs * 1e6;
    s_st.rate_points = s_fit.n;
    portEXIT_CRITICAL(&s_mux);
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
