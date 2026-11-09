/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Vlad Patoka (VE3VPT) */
/**
 * @file acq.c
 * @brief Acquisition task. See acq.h.
 *
 * Timing budget at 1000 SPS: one SPI frame (about 15 us at 8 MHz) plus two
 * FIR dot products every tenth sample (2 x 201 int64 MACs). The task runs
 * at priority 20, above lwIP, below the WiFi driver; a 1 ms deadline per
 * frame leaves a wide margin. A DRDY served late shows up as an overrun
 * (the notification count was above one) instead of passing silently.
 *
 * FIR: windowed sinc (Blackman), linear phase, unity DC gain, cutoff at
 * 0.4 x the output rate (40 Hz at 100 SPS). Coefficients in Q30, samples
 * 24-bit, accumulated in int64: no float on the sample path (the C5 core
 * has no FPU). Minimum-phase is a phase-2 item.
 */

#include "acq.h"
#include "ads131m0x.h"
#include "fault.h"
#include "io_map.h"
#include "timebase.h"

#include <math.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_clk_tree.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ACQ";

#define FIR_MAX           255
#define CLIP_COUNTS       (ADS_FS_COUNTS - 16)
#define RETRY_TICKS       50          /* 100 ms loop ticks: 5 s between ADC probes */

static TaskHandle_t s_task;
static volatile int64_t  s_drdy_t;
static volatile bool     s_paused;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;   /* status, position */
static SemaphoreHandle_t s_ring_lock;

static acq_settings_t s_set;            /* owned by the task once started */
static acq_settings_t s_pending;
static volatile bool  s_pending_flag;
static volatile bool  s_sync_req;
static volatile int   s_mux_ch = -1;
static volatile uint8_t s_mux_val;

static acq_status_t s_st;
static uint64_t s_n_in;
static int64_t  s_last_t;
static uint32_t s_epoch;

static int32_t  s_coef[FIR_MAX];
static int32_t  s_hist[ACQ_NCH][FIR_MAX];
static int      s_taps = 1, s_hpos, s_dcount;

static acq_sample_t s_ring[ACQ_RING_LEN];
static uint32_t s_seq;                  /* next output seq; monotonic across restarts */

static bool s_ledc_on;

/* ═════════════════════════════════════════════════════════════════════════
 * DRDY
 * ═════════════════════════════════════════════════════════════════════════ */

static void IRAM_ATTR drdy_isr(void *arg)
{
    (void)arg;
    s_drdy_t = esp_timer_get_time();
    BaseType_t woken = pdFALSE;
    if (s_task) vTaskNotifyGiveFromISR(s_task, &woken);
    portYIELD_FROM_ISR(woken);
}

static void drdy_init(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << ADC_DRDY_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,       /* the Click has 4.7k too */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io);
    gpio_isr_handler_add(ADC_DRDY_GPIO, drdy_isr, NULL);
    gpio_intr_disable(ADC_DRDY_GPIO);
}

/* ═════════════════════════════════════════════════════════════════════════
 * Clock path
 * ═════════════════════════════════════════════════════════════════════════ */

static uint32_t clock_setup(const acq_settings_t *s)
{
    if (s->clk_src != ACQ_CLK_LEDC && s_ledc_on) {
        ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
        s_ledc_on = false;
    }

    if (s->clk_src == ACQ_CLK_LEDC) {
        uint32_t xtal = 0;
        esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_XTAL, ESP_CLK_TREE_SRC_FREQ_PRECISION_APPROX, &xtal);
        if (!xtal) xtal = 48000000;
        /* Integer divider only: a fractional LEDC divider dithers the period. */
        uint32_t k = (uint32_t)lround((double)xtal / (2.0 * s->clk_hz));
        if (k < 1) k = 1;
        uint32_t f = xtal / (2 * k);
        const ledc_timer_config_t tc = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_1_BIT,
            .timer_num = LEDC_TIMER_0,
            .freq_hz = f,
            .clk_cfg = LEDC_USE_XTAL_CLK,
        };
        const ledc_channel_config_t cc = {
            .gpio_num = ADC_CLKIN_LEDC_GPIO,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_0,
            .timer_sel = LEDC_TIMER_0,
            .duty = 1,                           /* 50 % at 1-bit resolution */
            .hpoint = 0,
        };
        if (ledc_timer_config(&tc) == ESP_OK && ledc_channel_config(&cc) == ESP_OK) {
            s_ledc_on = true;
            ESP_LOGI(TAG, "CLKIN from LEDC: XTAL %lu Hz / %lu = %lu Hz on GPIO%d",
                     (unsigned long)xtal, (unsigned long)(2 * k), (unsigned long)f, ADC_CLKIN_LEDC_GPIO);
        } else {
            ESP_LOGE(TAG, "LEDC clock setup failed");
        }
        return f;
    }

    if (s->clk_src == ACQ_CLK_LTC6903) {
        uint32_t f = s->clk_hz;
        if (ltc6903_set_hz(s->clk_hz, &f) != ESP_OK) ESP_LOGE(TAG, "LTC6903 write failed");
        ESP_LOGW(TAG, "CLKIN from the LTC6903: about 1 %% off and drifting; use a 1 Hz reference");
        return f;
    }

    ESP_LOGI(TAG, "CLKIN from an external oscillator, %lu Hz nominal", (unsigned long)s->clk_hz);
    return s->clk_hz;
}

/* ═════════════════════════════════════════════════════════════════════════
 * FIR
 * ═════════════════════════════════════════════════════════════════════════ */

static int auto_taps(int decim)
{
    if (decim <= 1) return 1;
    int t = 20 * decim + 1;
    return t > FIR_MAX ? FIR_MAX : t;
}

static void fir_design(int taps, int decim)
{
    memset(s_hist, 0, sizeof(s_hist));
    s_hpos = 0;
    s_dcount = 0;
    if (decim <= 1) {
        s_taps = 1;
        s_coef[0] = 1 << 30;
        return;
    }
    if (taps <= 0) taps = auto_taps(decim);
    if (taps > FIR_MAX) taps = FIR_MAX;
    if (!(taps & 1)) taps--;
    if (taps < 3) taps = 3;
    s_taps = taps;

    static double h[FIR_MAX];
    const double fc = 0.4 / decim;        /* cycles per input sample */
    const int m = taps - 1;
    double sum = 0;
    for (int n = 0; n < taps; n++) {
        double x = n - m / 2.0;
        double sinc = (x == 0.0) ? 2.0 * fc : sin(2.0 * M_PI * fc * x) / (M_PI * x);
        double w = 0.42 - 0.5 * cos(2.0 * M_PI * n / m) + 0.08 * cos(4.0 * M_PI * n / m);
        h[n] = sinc * w;
        sum += h[n];
    }
    for (int n = 0; n < taps; n++) s_coef[n] = (int32_t)lround(h[n] / sum * (double)(1 << 30));
}

static inline int32_t fir_out(int c)
{
    int64_t acc = 0;
    int idx = s_hpos;                      /* newest sample sits at s_hpos - 1 */
    for (int k = 0; k < s_taps; k++) {
        idx = idx ? idx - 1 : s_taps - 1;
        acc += (int64_t)s_coef[k] * s_hist[c][idx];
    }
    return (int32_t)((acc + (1LL << 29)) >> 30);
}

/* ═════════════════════════════════════════════════════════════════════════
 * Stats, ring
 * ═════════════════════════════════════════════════════════════════════════ */

static struct {
    double sum[ACQ_NCH], sumsq[ACQ_NCH];
    int32_t mn[ACQ_NCH], mx[ACQ_NCH];
    uint32_t clips[ACQ_NCH];
    uint32_t n, win;
    uint32_t crc0, ovr0;
} s_acc;

static void stats_reset(uint32_t win)
{
    memset(&s_acc, 0, sizeof(s_acc));
    s_acc.win = win ? win : 1;
    for (int c = 0; c < ACQ_NCH; c++) { s_acc.mn[c] = INT32_MAX; s_acc.mx[c] = INT32_MIN; }
    s_acc.crc0 = s_st.crc_errors;
    s_acc.ovr0 = s_st.overruns;
}

static void stats_push(const int32_t *v)
{
    for (int c = 0; c < ACQ_NCH; c++) {
        s_acc.sum[c] += v[c];
        s_acc.sumsq[c] += (double)v[c] * v[c];
        if (v[c] < s_acc.mn[c]) s_acc.mn[c] = v[c];
        if (v[c] > s_acc.mx[c]) s_acc.mx[c] = v[c];
    }
    if (++s_acc.n < s_acc.win) return;

    acq_ch_stats_t out[ACQ_NCH];
    bool clip = false;
    for (int c = 0; c < ACQ_NCH; c++) {
        double mean = s_acc.sum[c] / s_acc.n;
        double var = s_acc.sumsq[c] / s_acc.n - mean * mean;
        out[c].mean = mean;
        out[c].rms = var > 0 ? sqrt(var) : 0;
        out[c].min = s_acc.mn[c];
        out[c].max = s_acc.mx[c];
        out[c].clips = s_acc.clips[c];
        clip |= s_acc.clips[c] != 0;
    }
    bool crc = s_st.crc_errors != s_acc.crc0;
    bool ovr = s_st.overruns != s_acc.ovr0;
    portENTER_CRITICAL(&s_mux);
    memcpy(s_st.ch, out, sizeof(out));
    portEXIT_CRITICAL(&s_mux);

    if (clip) fault_raise(FAULT_ADC_CLIP); else fault_clear(FAULT_ADC_CLIP);
    if (crc)  fault_raise(FAULT_ADC_CRC);  else fault_clear(FAULT_ADC_CRC);
    if (ovr)  fault_raise(FAULT_ADC_OVERRUN); else fault_clear(FAULT_ADC_OVERRUN);
    stats_reset(s_acc.win);
}

static void ring_push(int64_t t_esp, const int32_t *v)
{
    acq_sample_t smp = { .seq = s_seq, .utc_us = timebase_esp_to_utc_us(t_esp) };
    memcpy(smp.v, v, sizeof(smp.v));
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    s_ring[s_seq % ACQ_RING_LEN] = smp;
    s_seq++;
    xSemaphoreGive(s_ring_lock);
}

int acq_read_since(uint32_t since, acq_sample_t *out, int max, uint32_t *next_seq)
{
    if (!s_ring_lock || max <= 0) { if (next_seq) *next_seq = since; return 0; }
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    uint32_t head = s_seq;
    uint32_t oldest = head > ACQ_RING_LEN ? head - ACQ_RING_LEN : 0;
    if ((int32_t)(since - oldest) < 0 || (int32_t)(since - head) > 0) since = oldest;
    uint32_t avail = head - since;
    if (avail > (uint32_t)max) since = head - (uint32_t)max;   /* newest max, not oldest */
    int n = 0;
    for (uint32_t s = since; s != head; s++) out[n++] = s_ring[s % ACQ_RING_LEN];
    xSemaphoreGive(s_ring_lock);
    if (next_seq) *next_seq = head;
    return n;
}

/* ═════════════════════════════════════════════════════════════════════════
 * Task
 * ═════════════════════════════════════════════════════════════════════════ */

static bool reconfigure(void)
{
    gpio_intr_disable(ADC_DRDY_GPIO);
    uint32_t clk = clock_setup(&s_set);
    vTaskDelay(pdMS_TO_TICKS(5));

    bool ok = ads_reset_and_probe() == ESP_OK;
    if (ok) {
        ok = ads_configure(s_set.osr, s_set.pga, s_set.phase, s_set.gcal, s_set.ocal, ACQ_NCH) == ESP_OK;
        if (!ok) ESP_LOGE(TAG, "ADC configuration did not read back");
    }
    if (ok) fault_clear(FAULT_ADC_MISSING); else fault_raise(FAULT_ADC_MISSING);

    int decim = s_set.decim ? s_set.decim : 1;
    fir_design(s_set.taps, decim);
    double fs_in = (double)clk / 2.0 / s_set.osr;

    portENTER_CRITICAL(&s_mux);
    s_st.running = ok;
    s_st.adc_ok = ok;
    s_st.adc_id = ads_info()->id;
    s_st.clk_hz_actual = clk;
    s_st.fs_in = fs_in;
    s_st.fs_out = fs_in / decim;
    s_st.group_delay_s = (s_taps - 1) / 2.0 / fs_in;
    s_st.n_in = 0;
    s_st.restarts++;
    s_n_in = 0;
    s_last_t = 0;
    s_epoch++;
    portEXIT_CRITICAL(&s_mux);
    stats_reset((uint32_t)lround(fs_in / decim));

    if (ok) {
        ESP_LOGI(TAG, "Running: %.4f SPS in, decimate by %d (%d taps), %.4f SPS out, delay %.1f ms",
                 fs_in, decim, s_taps, fs_in / decim, s_st.group_delay_s * 1000.0);
        ads_sync_pulse();
        ulTaskNotifyTake(pdTRUE, 0);
        gpio_intr_enable(ADC_DRDY_GPIO);
    }
    return ok;
}

static void handle_requests(void)
{
    if (s_sync_req) {
        s_sync_req = false;
        ads_sync_pulse();
        ESP_LOGI(TAG, "SYNC pulse sent");
    }
    int ch = s_mux_ch;
    if (ch >= 0) {
        s_mux_ch = -1;
        gpio_intr_disable(ADC_DRDY_GPIO);
        esp_err_t err = ads_set_mux((uint8_t)ch, (ads_mux_t)s_mux_val, s_set.phase[ch]);
        ESP_LOGI(TAG, "CH%d input mux -> %u (%s)", ch, s_mux_val, esp_err_to_name(err));
        ulTaskNotifyTake(pdTRUE, 0);
        gpio_intr_enable(ADC_DRDY_GPIO);
    }
}

static void acq_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    bool ok = reconfigure();
    uint32_t idle = 0;

    for (;;) {
        if (s_pending_flag) {
            s_pending_flag = false;
            s_set = s_pending;
            ok = reconfigure();
            idle = 0;
        }

        uint32_t n = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        esp_task_wdt_reset();

        if (s_paused) continue;
        if (!ok) {
            if (++idle >= RETRY_TICKS) { idle = 0; ok = reconfigure(); }
            continue;
        }
        if (n == 0) {
            if (s_st.clock_ok) ESP_LOGW(TAG, "No DRDY for 100 ms: CLKIN missing or the ADC lost power");
            s_st.clock_ok = false;
            if (++idle >= RETRY_TICKS) { idle = 0; ok = reconfigure(); }
            continue;
        }
        idle = 0;
        if (!s_st.clock_ok) { s_st.clock_ok = true; ESP_LOGI(TAG, "DRDY running"); }
        if (n > 1) s_st.overruns += n - 1;

        handle_requests();

        int64_t t = s_drdy_t;
        int32_t raw[ADS_MAX_CH] = { 0 };
        esp_err_t err = ads_read_frame(raw);
        if (err == ESP_ERR_INVALID_CRC) s_st.crc_errors++;
        else if (err != ESP_OK) continue;

        for (int c = 0; c < ACQ_NCH; c++) {
            if (raw[c] >= CLIP_COUNTS || raw[c] <= -CLIP_COUNTS) s_acc.clips[c]++;
            s_hist[c][s_hpos] = raw[c];
        }
        s_hpos = s_hpos + 1 == s_taps ? 0 : s_hpos + 1;

        portENTER_CRITICAL(&s_mux);
        s_n_in++;
        s_last_t = t;
        s_st.n_in = s_n_in;
        s_st.adc_status = ads_info()->status;
        portEXIT_CRITICAL(&s_mux);

        int decim = s_set.decim ? s_set.decim : 1;
        if (++s_dcount < decim) continue;
        s_dcount = 0;

        int32_t y[ACQ_NCH];
        for (int c = 0; c < ACQ_NCH; c++) y[c] = fir_out(c);
        int64_t t_center = t - (int64_t)llround(s_st.group_delay_s * 1e6);
        ring_push(t_center, y);
        s_st.seq = s_seq;
        stats_push(y);
    }
}

/* ═════════════════════════════════════════════════════════════════════════
 * Public
 * ═════════════════════════════════════════════════════════════════════════ */

esp_err_t acq_start(const acq_settings_t *s)
{
    s_ring_lock = xSemaphoreCreateMutex();
    if (!s_ring_lock) return ESP_ERR_NO_MEM;
    s_set = *s;
    esp_err_t err = ads_bus_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init: %s", esp_err_to_name(err));
        return err;
    }
    drdy_init();
    if (xTaskCreate(acq_task, "acq", 4096, NULL, PRIO_ACQ, &s_task) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void acq_apply(const acq_settings_t *s)
{
    s_pending = *s;
    s_pending_flag = true;
    s_paused = false;
    if (s_task) xTaskNotifyGive(s_task);
}

void acq_pause(void)
{
    s_paused = true;
    gpio_intr_disable(ADC_DRDY_GPIO);
}

void acq_request_sync(void) { s_sync_req = true; }

void acq_request_mux(uint8_t ch, uint8_t mux)
{
    if (ch >= ACQ_NCH || mux > 3) return;
    s_mux_val = mux;
    s_mux_ch = ch;
}

void acq_get_status(acq_status_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
    out->crc_errors = ads_info()->crc_errors;
    out->running = out->running && !s_paused;
}

void acq_get_position(uint64_t *n, int64_t *t_us, uint32_t *epoch)
{
    portENTER_CRITICAL(&s_mux);
    *n = s_n_in;
    *t_us = s_last_t;
    *epoch = s_epoch;
    portEXIT_CRITICAL(&s_mux);
}

double acq_fs_in(void)
{
    return s_st.fs_in;
}
