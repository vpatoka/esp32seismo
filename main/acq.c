/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Vlad Patoka (VE3VPT) */
/**
 * @file acq.c
 * @brief Acquisition task. See acq.h.
 *
 * Timing budget at 1000 SPS: up to two SPI frames per DRDY (about 15 us
 * each at 8 MHz) plus two FIR dot products every tenth sample (2 x 201
 * int64 MACs). The task runs at priority 20, above lwIP, below the WiFi
 * driver; a 1 ms deadline per frame leaves a wide margin.
 *
 * DRDY accounting: the ISR counts edges and stores the esp_timer time of
 * each one in a small ring indexed by that count. The task works from the
 * count, not from the notification value, so a settings wakeup is never
 * mistaken for a sample and a late wakeup still knows how many conversions
 * happened. On each wakeup it reads frames until STATUS shows no new data
 * (at most the two the ADC FIFO holds). Valid frames are the newest
 * conversions; any shortfall is lost samples, replaced by the last good
 * value so the FIR history and the sample counter stay on the true grid.
 *
 * STATUS is checked on every good frame: a RESET flag or a wrong word
 * length means the converter restarted on its own (brownout) and is back at
 * its defaults, so the task reconfigures it. An unexpected F_RESYNC moves
 * the sample grid, so the rate loop epoch is bumped.
 *
 * FIR: windowed sinc (Blackman), linear phase, unity DC gain, cutoff at
 * 0.4 x the output rate (40 Hz at 100 SPS). Coefficients in Q30, samples
 * 24-bit, accumulated in int64: no float on the sample path. Output is
 * held back after a restart until the history holds only settled data.
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
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ACQ";

#define FIR_MAX           255
#define CLIP_COUNTS       (ADS_FS_COUNTS - 16)
#define RETRY_US          5000000     /* 5 s between ADC probes */
#define NO_DRDY_US        100000      /* DRDY silent this long: clock or power gone */
#define DRDY_TS_LEN       16          /* DRDY timestamp ring, power of two */
#define SYNC_GUARD_FRAMES 8           /* F_RESYNC expected this soon after our own SYNC */
#define BITS_PROBE_FRAMES 64

/* Index into s_taint[] = bit position of the ACQ_F_* flag */
#define TAINT_N           4

static TaskHandle_t s_task;
static volatile uint32_t s_drdy_cnt;                /* written by the ISR only */
static volatile int64_t  s_drdy_ts[DRDY_TS_LEN];
static uint32_t s_seen;                             /* DRDY count already accounted for */
static volatile bool     s_paused;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;   /* status, position */

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

static int32_t  s_hold[ACQ_NCH];        /* last good input sample */
static uint32_t s_skip_in;              /* inputs still to hold for sinc3 settling */
static uint32_t s_mute_out;             /* inputs before the FIR history is all settled data */
static uint32_t s_taint[TAINT_N];       /* inputs left in the FIR window per flag */
static uint32_t s_sync_guard;
static bool     s_bits_seen;            /* STATUS DRDY bits observed since the restart */
static uint32_t s_bits_probe;
static int64_t  s_period_us;
static int64_t  s_gd_us;
static int64_t  s_last_evt, s_last_try;

/* Output ring. Single writer (this task), lock-free readers. The writer
 * fills slot seq & mask, then publishes seq + 1 with release order; a
 * reader loads the head with acquire order, copies, loads it again and
 * drops whatever the writer may have reached in between. One slot is kept
 * as slack for the write in progress. */
static acq_sample_t *s_ring;
static uint32_t s_ring_len;             /* power of two */
static uint32_t s_seq;                  /* next output seq; monotonic across restarts */
static uint32_t s_ring_fill;            /* slots written, saturates at s_ring_len */

static bool s_ledc_on;

/* ═════════════════════════════════════════════════════════════════════════
 * DRDY
 * ═════════════════════════════════════════════════════════════════════════ */

static void IRAM_ATTR drdy_isr(void *arg)
{
    (void)arg;
    uint32_t k = s_drdy_cnt;
    s_drdy_ts[k & (DRDY_TS_LEN - 1)] = esp_timer_get_time();
    s_drdy_cnt = k + 1;
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

/* Time of DRDY number idx. cnt is a count read after idx happened. Entries
 * the ISR has overwritten are extrapolated back from the newest one. */
static int64_t drdy_time(uint32_t idx, uint32_t cnt)
{
    int64_t t = s_drdy_ts[idx & (DRDY_TS_LEN - 1)];
    if ((uint32_t)(s_drdy_cnt - idx) <= DRDY_TS_LEN) return t;
    int64_t newest = s_drdy_ts[(cnt - 1) & (DRDY_TS_LEN - 1)];
    return newest - (int64_t)(cnt - 1 - idx) * s_period_us;
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
    if (taps < 14 * decim) {
        ESP_LOGW(TAG, "%d taps is short for decimation by %d (want %d or more): expect aliasing",
                 taps, decim, 14 * decim);
    }

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
    uint32_t crc0, ovr0, stat0;
} s_acc;

static void stats_reset(uint32_t win)
{
    memset(&s_acc, 0, sizeof(s_acc));
    s_acc.win = win ? win : 1;
    for (int c = 0; c < ACQ_NCH; c++) { s_acc.mn[c] = INT32_MAX; s_acc.mx[c] = INT32_MIN; }
    s_acc.crc0 = s_st.crc_errors;
    s_acc.ovr0 = s_st.overruns;
    s_acc.stat0 = s_st.status_events;
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
    bool sta = s_st.status_events != s_acc.stat0;
    portENTER_CRITICAL(&s_mux);
    memcpy(s_st.ch, out, sizeof(out));
    portEXIT_CRITICAL(&s_mux);

    if (clip) fault_raise(FAULT_ADC_CLIP); else fault_clear(FAULT_ADC_CLIP);
    if (crc)  fault_raise(FAULT_ADC_CRC);  else fault_clear(FAULT_ADC_CRC);
    if (ovr)  fault_raise(FAULT_ADC_OVERRUN); else fault_clear(FAULT_ADC_OVERRUN);
    if (sta)  fault_raise(FAULT_ADC_STATUS);  else fault_clear(FAULT_ADC_STATUS);
    stats_reset(s_acc.win);
}

static void ring_push(int64_t t_esp, const int32_t *v, uint8_t flags)
{
    uint32_t seq = s_seq;                              /* only this task writes it */
    acq_sample_t *slot = &s_ring[seq & (s_ring_len - 1)];
    slot->seq = seq;
    slot->flags = flags;
    slot->utc_us = timebase_esp_to_utc_us(t_esp);
    memcpy(slot->v, v, sizeof(slot->v));
    if (s_ring_fill < s_ring_len) __atomic_store_n(&s_ring_fill, s_ring_fill + 1, __ATOMIC_RELEASE);
    __atomic_store_n(&s_seq, seq + 1, __ATOMIC_RELEASE);
}

/* Oldest seq a reader may trust when the head is at head. */
static inline uint32_t ring_oldest(uint32_t head)
{
    uint32_t fill = __atomic_load_n(&s_ring_fill, __ATOMIC_ACQUIRE);
    uint32_t keep = fill < s_ring_len ? fill : s_ring_len - 1;    /* one slot of slack */
    return head - keep;                                            /* wraps correctly */
}

int acq_read_since(uint32_t since, acq_sample_t *out, int max, uint32_t *next_seq)
{
    if (!s_ring || max <= 0) { if (next_seq) *next_seq = since; return 0; }
    const uint32_t mask = s_ring_len - 1;
    uint32_t head = __atomic_load_n(&s_seq, __ATOMIC_ACQUIRE);
    uint32_t oldest = ring_oldest(head);
    if ((int32_t)(since - oldest) < 0 || (int32_t)(since - head) > 0) since = oldest;
    if (head - since > (uint32_t)max) since = head - (uint32_t)max;   /* newest max, not oldest */
    int n = 0;
    for (uint32_t s = since; s != head; s++) out[n++] = s_ring[s & mask];

    /* Anything the writer reached during the copy is gone: drop it from the front. */
    uint32_t valid = ring_oldest(__atomic_load_n(&s_seq, __ATOMIC_ACQUIRE));
    int drop = 0;
    while (drop < n && ((int32_t)(since + (uint32_t)drop - valid) < 0 ||
                        out[drop].seq != since + (uint32_t)drop)) drop++;
    if (drop) {
        memmove(out, out + drop, sizeof(*out) * (size_t)(n - drop));
        n -= drop;
    }
    if (next_seq) *next_seq = head;
    return n;
}

uint32_t acq_ring_len(void)
{
    return s_ring_len;
}

/* ═════════════════════════════════════════════════════════════════════════
 * Input path
 * ═════════════════════════════════════════════════════════════════════════ */

static void bump_epoch(void)
{
    portENTER_CRITICAL(&s_mux);
    s_epoch++;
    portEXIT_CRITICAL(&s_mux);
}

/* One input period. raw is NULL for a lost sample; ev carries ACQ_F_GAP or
 * ACQ_F_CRC when the value must not be trusted. Held inputs repeat the last
 * good sample: the FIR keeps its grid and the output carries the flag. */
static void push_input(const int32_t *raw, int64_t t, uint8_t ev)
{
    bool real = raw && !(ev & (ACQ_F_GAP | ACQ_F_CRC));
    if (real && s_skip_in) {
        s_skip_in--;
        ev |= ACQ_F_UNSETTLED;
        real = false;
    }
    for (int c = 0; c < ACQ_NCH; c++) {
        if (real) {
            if (raw[c] >= CLIP_COUNTS || raw[c] <= -CLIP_COUNTS) {
                s_acc.clips[c]++;
                ev |= ACQ_F_CLIP;
            }
            s_hold[c] = raw[c];
        }
        s_hist[c][s_hpos] = s_hold[c];
    }
    s_hpos = s_hpos + 1 == s_taps ? 0 : s_hpos + 1;

    uint8_t flags = 0;
    for (int b = 0; b < TAINT_N; b++) {
        if (ev & (1u << b)) s_taint[b] = (uint32_t)s_taps;
        if (s_taint[b]) { flags |= (uint8_t)(1u << b); s_taint[b]--; }
    }

    portENTER_CRITICAL(&s_mux);
    s_n_in++;
    s_last_t = t;
    s_st.n_in = s_n_in;
    portEXIT_CRITICAL(&s_mux);

    int decim = s_set.decim ? s_set.decim : 1;
    bool muted = s_mute_out > 0;
    if (muted) s_mute_out--;
    if (++s_dcount < decim) return;
    s_dcount = 0;
    if (muted) return;

    int32_t y[ACQ_NCH];
    for (int c = 0; c < ACQ_NCH; c++) y[c] = fir_out(c);
    ring_push(t - s_gd_us, y, flags);
    s_st.seq = s_seq;
    stats_push(y);
}

/* A gap too long to bridge with held samples: count it, restart the rate
 * epoch and refill the FIR before the next output. */
static void big_gap(uint32_t lost)
{
    portENTER_CRITICAL(&s_mux);
    s_n_in += lost;
    s_st.n_in = s_n_in;
    s_last_t = 0;                       /* the rate loop skips until the next real DRDY */
    s_epoch++;
    portEXIT_CRITICAL(&s_mux);
    s_mute_out = (uint32_t)s_taps + ADS_SINC3_SETTLE;
    s_taint[0] = s_mute_out + (uint32_t)(s_set.decim ? s_set.decim : 1);   /* ACQ_F_GAP on the first output */
    ESP_LOGW(TAG, "%lu input samples lost: FIR restarted", (unsigned long)lost);
}

/* Read what the ADC has for us. Returns false when it needs a reconfigure. */
static bool service_drdy(void)
{
    int32_t raw[ADS_FIFO_DEPTH][ADS_MAX_CH];
    uint8_t ev[ADS_FIFO_DEPTH];
    uint32_t valid = 0;
    const uint16_t dmask = ADS_ST_DRDY_MASK(ads_info()->nch);

    /* Until STATUS has shown its DRDY bits we cannot tell a stale frame from
     * a new one, so read only as many frames as there were edges. After
     * that, read until the FIFO is empty: a backlog left behind once would
     * otherwise pair every later sample with the next DRDY's timestamp. */
    uint32_t want = s_drdy_cnt - s_seen;
    uint32_t reads = s_bits_seen ? ADS_FIFO_DEPTH : (want < ADS_FIFO_DEPTH ? want : ADS_FIFO_DEPTH);

    for (uint32_t j = 0; j < reads; j++) {
        memset(raw[valid], 0, sizeof(raw[valid]));
        esp_err_t err = ads_read_frame(raw[valid]);
        if (s_sync_guard) s_sync_guard--;
        if (err == ESP_ERR_INVALID_CRC) {
            s_st.crc_errors++;
            ev[valid++] = ACQ_F_CRC;           /* a conversion was consumed; its value is not trusted */
            continue;
        }
        if (err != ESP_OK) break;

        uint16_t st = ads_info()->status;
        if ((st & ADS_ST_RESET) || (st & ADS_ST_WLENGTH_MASK) != ADS_ST_WLENGTH_24) {
            ESP_LOGE(TAG, "ADC STATUS 0x%04X: the converter restarted on its own, reconfiguring", st);
            s_st.status_events++;
            fault_raise(FAULT_ADC_STATUS);
            s_seen = s_drdy_cnt;
            return false;
        }
        if ((st & ADS_ST_F_RESYNC) && !s_sync_guard) {
            ESP_LOGW(TAG, "ADC STATUS 0x%04X: resync without a SYNC from us, sample grid moved", st);
            s_st.status_events++;
            fault_raise(FAULT_ADC_STATUS);
            bump_epoch();
            s_skip_in = ADS_SINC3_SETTLE;
            s_sync_guard = (uint32_t)s_st.fs_in;   /* at most one report per second */
        }
        if (st & dmask) {
            s_bits_seen = true;
        } else if (s_bits_seen) {
            break;                             /* FIFO empty: the normal end of a pass */
        } else if (++s_bits_probe == BITS_PROBE_FRAMES) {
            ESP_LOGW(TAG, "STATUS DRDY bits never set in %d frames: stale-frame check off", BITS_PROBE_FRAMES);
        }
        portENTER_CRITICAL(&s_mux);
        s_st.adc_status = st;
        portEXIT_CRITICAL(&s_mux);
        ev[valid++] = 0;
    }

    /* Count again after the reads: an edge during them belongs to this pass. */
    uint32_t cnt = s_drdy_cnt;
    uint32_t avail = cnt - s_seen;
    s_seen = cnt;

    uint32_t first = 0;
    if (valid > avail) {                       /* backlog from before the last resume */
        first = valid - avail;
        s_st.stale_frames += first;
        valid = avail;
    }
    uint32_t lost = avail - valid;
    if (lost) {
        s_st.overruns += lost;
        if (lost > 2u * (uint32_t)s_taps + 16u) {
            big_gap(lost);
        } else {
            for (uint32_t i = 0; i < lost; i++) {
                uint32_t idx = cnt - avail + i;
                push_input(NULL, drdy_time(idx, cnt), ACQ_F_GAP);
            }
        }
    }
    for (uint32_t j = 0; j < valid; j++) {
        uint32_t idx = cnt - valid + j;
        push_input(raw[first + j], drdy_time(idx, cnt), ev[first + j]);
    }
    return true;
}

/* ═════════════════════════════════════════════════════════════════════════
 * Task
 * ═════════════════════════════════════════════════════════════════════════ */

/* Empty the ADC FIFO, then start counting DRDY edges from here. */
static void resume_stream(void)
{
    for (int i = 0; i < ADS_FIFO_DEPTH; i++) {
        int32_t junk[ADS_MAX_CH];
        ads_read_frame(junk);
    }
    ulTaskNotifyTake(pdTRUE, 0);
    s_seen = s_drdy_cnt;
    s_last_evt = esp_timer_get_time();
    gpio_intr_enable(ADC_DRDY_GPIO);
}

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
    double gd = ((s_taps - 1) / 2.0 + ADS_SINC3_DELAY_PERIODS) / fs_in;

    memset(s_hold, 0, sizeof(s_hold));
    memset(s_taint, 0, sizeof(s_taint));
    s_skip_in = 0;
    s_mute_out = (uint32_t)s_taps + ADS_SINC3_SETTLE;
    s_bits_seen = false;
    s_bits_probe = 0;
    s_sync_guard = SYNC_GUARD_FRAMES;
    s_period_us = (int64_t)llround(1e6 / fs_in);
    s_gd_us = (int64_t)llround(gd * 1e6);
    s_last_try = esp_timer_get_time();
    s_last_evt = s_last_try;

    portENTER_CRITICAL(&s_mux);
    s_st.running = ok;
    s_st.adc_ok = ok;
    s_st.adc_id = ads_info()->id;
    s_st.clk_hz_actual = clk;
    s_st.fs_in = fs_in;
    s_st.fs_out = fs_in / decim;
    s_st.group_delay_s = gd;
    s_st.n_in = 0;
    s_st.restarts++;
    s_n_in = 0;
    s_last_t = 0;
    s_epoch++;
    portEXIT_CRITICAL(&s_mux);
    stats_reset((uint32_t)lround(fs_in / decim));

    if (ok) {
        ESP_LOGI(TAG, "Running: %.4f SPS in, decimate by %d (%d taps), %.4f SPS out, delay %.2f ms",
                 fs_in, decim, s_taps, fs_in / decim, gd * 1000.0);
        ads_sync_pulse();
        resume_stream();
    }
    return ok;
}

static void handle_requests(void)
{
    if (s_sync_req) {
        s_sync_req = false;
        ads_sync_pulse();
        s_sync_guard = SYNC_GUARD_FRAMES;
        s_skip_in = ADS_SINC3_SETTLE;
        bump_epoch();                          /* the sample grid moves */
        ESP_LOGI(TAG, "SYNC pulse sent");
    }
    int ch = s_mux_ch;
    if (ch >= 0) {
        s_mux_ch = -1;
        /* DRDY keeps counting: the frames this costs show up as held samples. */
        esp_err_t err = ads_set_mux((uint8_t)ch, (ads_mux_t)s_mux_val, s_set.phase[ch]);
        ESP_LOGI(TAG, "CH%d input mux -> %u (%s)", ch, s_mux_val, esp_err_to_name(err));
    }
}

static void acq_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    bool ok = reconfigure();

    for (;;) {
        if (s_pending_flag) {
            s_pending_flag = false;
            s_set = s_pending;
            ok = reconfigure();
        }

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        esp_task_wdt_reset();

        if (s_paused) continue;
        int64_t now = esp_timer_get_time();
        if (!ok) {
            if (now - s_last_try >= RETRY_US) ok = reconfigure();
            continue;
        }

        if (s_drdy_cnt == s_seen) {            /* timeout or a control wakeup */
            if (now - s_last_evt >= NO_DRDY_US) {
                if (s_st.clock_ok) ESP_LOGW(TAG, "No DRDY for 100 ms: CLKIN missing or the ADC lost power");
                s_st.clock_ok = false;
                if (now - s_last_evt >= RETRY_US && now - s_last_try >= RETRY_US) {
                    ok = reconfigure();
                    continue;
                }
            }
            handle_requests();
            continue;
        }

        s_last_evt = now;
        if (!s_st.clock_ok) { s_st.clock_ok = true; ESP_LOGI(TAG, "DRDY running"); }

        if (!service_drdy()) {
            ok = reconfigure();
            continue;
        }
        handle_requests();
    }
}

/* ═════════════════════════════════════════════════════════════════════════
 * Public
 * ═════════════════════════════════════════════════════════════════════════ */

esp_err_t acq_start(const acq_settings_t *s)
{
    /* PSRAM when the build has it, else the old internal-RAM size. */
    s_ring_len = ACQ_RING_LEN_PSRAM;
    s_ring = heap_caps_calloc(s_ring_len, sizeof(acq_sample_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring) {
        s_ring_len = ACQ_RING_LEN;
        s_ring = heap_caps_calloc(s_ring_len, sizeof(acq_sample_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!s_ring) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "Output ring: %lu samples (%lu KB) in %s", (unsigned long)s_ring_len,
             (unsigned long)(s_ring_len * sizeof(acq_sample_t) / 1024),
             s_ring_len == ACQ_RING_LEN_PSRAM ? "PSRAM" : "internal RAM");
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
