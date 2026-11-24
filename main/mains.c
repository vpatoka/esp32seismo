/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Vlad Patoka (VE3VPT) */
/**
 * @file mains.c
 * @brief Mains frequency from the zero-cross edge. See mains.h.
 *
 * ISR side: glitch gate, storm breaker, edge time into a ring. Nothing else.
 * Task side, once a second: assign cycle numbers to the new edges (a gap of
 * k periods counts k cycles), least-squares fit of time against cycle number
 * for the period, correct it for the ESP crystal, keep a 60 s history, and
 * track grid time against UTC from an anchor.
 *
 * The ring is single-writer (the ISR) and single-reader (the task). The task
 * copies without masking interrupts, so it never delays a DRDY interrupt, and
 * re-checks the head afterwards for entries overwritten during the copy.
 */

#include "mains.h"

#include <math.h>
#include <string.h>

#include "acq.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fault.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/gpio_ll.h"
#include "io_map.h"
#include "soc/gpio_struct.h"
#include "timebase.h"

static const char *TAG = "MAINS";

#define RING_LEN        128               /* 2 s of edges; the task drains it every second */
#define RING_MASK       (RING_LEN - 1)
#define FIT_MIN_PTS     20                /* a third of a second of edges */
#define FREQ_MIN_HZ     45.0
#define FREQ_MAX_HZ     75.0
#define FAULT_CLEAR_S   60                /* clean running after a re-enable before the fault clears */
#define DIP_LOG_GAP_US  10000000          /* at most one dip log line per 10 s */

/* ── ISR side ─────────────────────────────────────────────────────────── */

static volatile int64_t  s_ring[RING_LEN];
static volatile uint32_t s_head;          /* edges written since boot */
static volatile int64_t  s_last_t;        /* last accepted edge, for the glitch gate */
static volatile uint32_t s_glitches;
static volatile int64_t  s_bucket_t;      /* start of the storm-count window */
static volatile uint32_t s_bucket_n;
static volatile bool     s_tripped;       /* input switched off by the breaker */

/* An edge at esp_timer time t, from the capture callback or the GPIO ISR. */
static void IRAM_ATTR mains_edge(int64_t t)
{
    if (s_tripped) return;
    /* Every edge counts toward the storm limit, glitches included. */
    if (t - s_bucket_t >= MAINS_STORM_BUCKET_US || t < s_bucket_t) {
        s_bucket_t = t;
        s_bucket_n = 0;
    }
    if (++s_bucket_n > MAINS_STORM_EDGES) {
        /* Off at the pad: no more edges reach the capture unit or the GPIO
         * interrupt. Register write only, safe here and during flash writes. */
        gpio_ll_input_disable(&GPIO, MAINS_ZC_GPIO);
        s_tripped = true;
        return;
    }
    const int64_t last = s_last_t;
    if (last && t - last < MAINS_GATE_US) {
        s_glitches = s_glitches + 1;
        return;
    }
    s_last_t = t;
    s_ring[s_head & RING_MASK] = t;
    s_head = s_head + 1;
}

static void IRAM_ATTR mains_isr(void *arg)
{
    (void)arg;
    mains_edge(esp_timer_get_time());
}

/* ── Task side ────────────────────────────────────────────────────────── */

static portMUX_TYPE   s_mux = portMUX_INITIALIZER_UNLOCKED;
static mains_status_t s_st;               /* published copy, under s_mux */

static uint32_t s_tail;                   /* next ring index to read */
static int64_t  s_prev_t;                 /* last processed edge, 0 = no continuity */
static double   s_period_us = 1e6 / MAINS_NOMINAL_HZ;
static uint64_t s_cycles;
static uint32_t s_missed, s_dips, s_last_dip_ms;
static int64_t  s_last_dip_log;
static bool     s_present;

static bool     s_anchor_valid;
static int64_t  s_anchor_utc_us, s_anchor_esp_us;
static uint64_t s_anchor_cycles;

static uint32_t s_trips;
static bool     s_trip_handled, s_latched, s_fault_up;
static int64_t  s_trip_t, s_clean_since;

static double   s_hist[MAINS_HIST_S];     /* per-second frequency, NAN when none */
static uint32_t s_hist_i;

static int64_t  s_edges[RING_LEN];        /* task-only copy buffer, off the stack */

static void break_continuity(void)
{
    s_prev_t = 0;
    s_anchor_valid = false;
}

static void storm_service(int64_t now)
{
    if (s_tripped && !s_trip_handled) {
        s_trip_handled = true;
        s_trips++;
        s_trip_t = now;
        s_clean_since = 0;
        break_continuity();
        if (!s_fault_up) { fault_raise(FAULT_MAINS_STORM); s_fault_up = true; }
        if (s_trips >= MAINS_STORM_MAX_TRIPS) {
            s_latched = true;
            ESP_LOGE(TAG, "Edge storm on GPIO%d (trip %lu): input off until reboot",
                     MAINS_ZC_GPIO, (unsigned long)s_trips);
        } else {
            ESP_LOGE(TAG, "Edge storm on GPIO%d (trip %lu): input off, retry in %d s",
                     MAINS_ZC_GPIO, (unsigned long)s_trips, MAINS_STORM_RETRY_S);
        }
    }
    if (s_tripped && !s_latched && now - s_trip_t >= (int64_t)MAINS_STORM_RETRY_S * 1000000) {
        /* The input is off, so the ISR is quiet while its state is reset. */
        s_bucket_t = 0;
        s_bucket_n = 0;
        s_last_t = 0;
        s_trip_handled = false;
        s_clean_since = now;
        s_tripped = false;
        gpio_input_enable(MAINS_ZC_GPIO);
        ESP_LOGW(TAG, "GPIO%d input back on after a storm trip", MAINS_ZC_GPIO);
    }
    if (s_fault_up && !s_tripped && !s_latched && s_clean_since &&
        now - s_clean_since >= (int64_t)FAULT_CLEAR_S * 1000000) {
        fault_clear(FAULT_MAINS_STORM);
        s_fault_up = false;
        s_clean_since = 0;
    }
}

/* Copy the edges written since the last call. Returns the count; on a ring
 * overrun the oldest are gone and continuity is broken. */
static int take_edges(void)
{
    uint32_t head = s_head;
    if (head - s_tail > RING_LEN) {
        ESP_LOGW(TAG, "Edge ring overrun: %lu edges lost", (unsigned long)(head - s_tail - RING_LEN));
        s_tail = head - RING_LEN;
        break_continuity();
    }
    int n = 0;
    for (uint32_t i = s_tail; i != head; i++) s_edges[n++] = s_ring[i & RING_MASK];
    /* Entries the ISR overwrote while we copied: drop them. */
    uint32_t after = s_head;
    int bad = (int)(after - s_tail) - RING_LEN;
    if (bad > 0) {
        if (bad > n) bad = n;
        memmove(s_edges, s_edges + bad, (size_t)(n - bad) * sizeof(s_edges[0]));
        n -= bad;
        break_continuity();
    }
    s_tail = head;
    return n;
}

static void dip(int64_t gap_us, int64_t now)
{
    s_dips++;
    s_last_dip_ms = (uint32_t)(gap_us / 1000);
    if (s_last_dip_log == 0 || now - s_last_dip_log >= DIP_LOG_GAP_US) {
        s_last_dip_log = now;
        ESP_LOGW(TAG, "Mains dip: no edge for %lu ms (dips %lu)",
                 (unsigned long)s_last_dip_ms, (unsigned long)s_dips);
    }
    s_anchor_valid = false;
}

static void process(int64_t now)
{
    const int n = take_edges();

    /* Least squares of time (us from the first point) on cycle number. */
    double sx = 0, sy = 0, sxx = 0, sxy = 0, x = 0;
    int np = 0;
    int64_t y0 = 0;
#define ADD(px, py) do { double _x = (px), _y = (py); sx += _x; sy += _y; \
                         sxx += _x * _x; sxy += _x * _y; np++; } while (0)
    if (s_prev_t) { y0 = s_prev_t; ADD(0.0, 0.0); }
    for (int i = 0; i < n; i++) {
        const int64_t t = s_edges[i];
        if (!s_prev_t) {
            s_prev_t = t;
            y0 = t; x = 0;
            sx = sy = sxx = sxy = 0; np = 0;
            ADD(0.0, 0.0);
            continue;
        }
        const int64_t dt = t - s_prev_t;
        if (dt <= 0) continue;                      /* out of order: should not happen */
        if (dt >= MAINS_DIP_US) {
            dip(dt, now);
            s_prev_t = t;
            y0 = t; x = 0;
            sx = sy = sxx = sxy = 0; np = 0;
            ADD(0.0, 0.0);
            continue;
        }
        long k = lround((double)dt / s_period_us);
        if (k < 1) k = 1;
        s_missed += (uint32_t)(k - 1);
        s_cycles += (uint64_t)k;
        x += (double)k;
        s_prev_t = t;
        ADD(x, (double)(t - y0));
    }
#undef ADD

    tb_status_t tb;
    timebase_get_status(&tb);

    double f = NAN;
    bool corrected = false;
    const double den = np * sxx - sx * sx;
    if (np >= FIT_MIN_PTS && den > 0) {
        const double p = (np * sxy - sx * sy) / den;     /* esp_timer us per cycle */
        double fe = p > 0 ? 1e6 / p : NAN;
        if (fe >= FREQ_MIN_HZ && fe <= FREQ_MAX_HZ) {
            s_period_us = p;
            /* esp_ppm: esp_timer us per reference second, less 1e6. A fast
             * esp_timer reads periods long, so the true frequency is higher. */
            if (tb.rate_valid) { fe *= 1.0 + tb.esp_ppm * 1e-6; corrected = true; }
            f = fe;
        }
    }
    s_hist[s_hist_i++ % MAINS_HIST_S] = f;

    /* Presence */
    const bool present = !s_tripped && s_prev_t && now - s_prev_t < MAINS_ABSENT_US;
    if (present != s_present) {
        s_present = present;
        if (present) ESP_LOGI(TAG, "Mains edges on GPIO%d", MAINS_ZC_GPIO);
        else         ESP_LOGW(TAG, "No mains edges on GPIO%d", MAINS_ZC_GPIO);
    }
    if (!present) break_continuity();

    /* Grid time against UTC, from an anchor edge. */
    double terr = NAN;
    if (present && s_prev_t) {
        if (!s_anchor_valid && !isnan(f) && timebase_time_valid()) {
            const int64_t u = timebase_esp_to_utc_us(s_prev_t);
            if (u) {
                s_anchor_utc_us = u;
                s_anchor_esp_us = s_prev_t;
                s_anchor_cycles = s_cycles;
                s_anchor_valid = true;
            }
        }
        if (s_anchor_valid) {
            const int64_t u = timebase_esp_to_utc_us(s_prev_t);
            if (u) {
                const double grid_s = (double)(s_cycles - s_anchor_cycles) / MAINS_NOMINAL_HZ;
                terr = (grid_s - (double)(u - s_anchor_utc_us) * 1e-6) * 1000.0;
            }
        }
    }

    /* History */
    double sum = 0, lo = INFINITY, hi = -INFINITY;
    int nv = 0;
    for (int i = 0; i < MAINS_HIST_S; i++) {
        const double v = s_hist[i];
        if (isnan(v)) continue;
        sum += v; nv++;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }

    mains_status_t st = {
        .enabled = true,
        .capture_hw = s_st.capture_hw,
        .present = present,
        .corrected = corrected,
        .freq_hz = f,
        .freq_avg_hz = nv ? sum / nv : NAN,
        .freq_min_hz = nv ? lo : NAN,
        .freq_max_hz = nv ? hi : NAN,
        .time_error_valid = !isnan(terr),
        .time_error_ms = terr,
        .anchor_age_s = s_anchor_valid ? (now - s_anchor_esp_us) / 1000000 : -1,
        .cycles = s_cycles,
        .missed = s_missed,
        .glitches = s_glitches,
        .dips = s_dips,
        .last_dip_ms = s_last_dip_ms,
        .storm_trips = s_trips,
        .storm_latched = s_latched,
    };
    portENTER_CRITICAL(&s_mux);
    s_st = st;
    portEXIT_CRITICAL(&s_mux);
}

static void mains_task(void *arg)
{
    (void)arg;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(1000));
        const int64_t now = esp_timer_get_time();
        storm_service(now);
        process(now);
    }
}

esp_err_t mains_init(void)
{
    for (int i = 0; i < MAINS_HIST_S; i++) s_hist[i] = NAN;
    s_st.freq_hz = s_st.freq_avg_hz = s_st.freq_min_hz = s_st.freq_max_hz = NAN;
    s_st.time_error_ms = NAN;
    s_st.anchor_age_s = -1;

    /* On the acquisition capture timer when it runs (its last free channel),
     * so mains edges share the DRDY counter; the GPIO interrupt otherwise. */
    esp_err_t cerr = acq_capture_add(MAINS_ZC_GPIO, true, mains_edge);
    if (cerr != ESP_OK) {
        if (cerr != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGE(TAG, "Mains capture channel: %s, using the GPIO interrupt", esp_err_to_name(cerr));
        }
        const gpio_config_t io = {
            .pin_bit_mask = 1ULL << MAINS_ZC_GPIO,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,       /* open-collector front ends need it */
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_POSEDGE,
        };
        esp_err_t err = gpio_config(&io);
        if (err == ESP_OK) err = gpio_isr_handler_add(MAINS_ZC_GPIO, mains_isr, NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Mains GPIO%d interrupt: %s", MAINS_ZC_GPIO, esp_err_to_name(err));
            return err;
        }
    }
    s_st.capture_hw = cerr == ESP_OK;
    s_st.enabled = true;

    if (xTaskCreate(mains_task, "mains", 3072, NULL, PRIO_MAINS, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Mains zero-cross on GPIO%d, rising edge, %s", MAINS_ZC_GPIO,
             s_st.capture_hw ? "MCPWM capture" : "GPIO interrupt");
    return ESP_OK;
}

void mains_get_status(mains_status_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
}
