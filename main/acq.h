/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Vlad Patoka (VE3VPT) */
/**
 * @file acq.h
 * @brief Acquisition: ADC clock, DRDY, FIR decimation, ring buffer, stats.
 *
 * One task owns the SPI bus. The DRDY ISR timestamps the edge and wakes it;
 * the task reads one frame, runs the decimating FIR and publishes output
 * samples into a ring that the web API and (later) the PSN stream read.
 *
 * An output sample stands for input-sample position n - D, where n is the
 * input count at its newest input and D the total group delay in input
 * periods (ADC sinc3 1.5, plus the FIR (taps - 1) / 2). timebase maps that
 * position to UTC: from the sample count against GPS PPS when it can,
 * otherwise from the DRDY time minus the delay through the slewed sample
 * clock. Either way the timestamp marks when the ground moved, not when the
 * sample left the filter, and tq says which model produced it.
 *
 * Every input period counts, read or not: a lost or corrupt input sample is
 * replaced by the last good one, the sample counter still advances, and the
 * output samples whose FIR window holds it carry a flag.
 */

#ifndef ACQ_H
#define ACQ_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define ACQ_NCH         2
#define ACQ_RING_LEN        4096      /* output samples kept, internal RAM fallback */
#define ACQ_RING_LEN_PSRAM  131072    /* with PSRAM: 22 min at 100 SPS, 3 MB */

/* Limits the FIR and the task can honour. A Blackman FIR needs about
 * 14 x decim taps to keep aliases out of 0..0.4 x f_out; 255 taps cover
 * decim 18. Below OSR 512 (8 kSPS) one task wake per sample is too slow. */
#define ACQ_DECIM_MAX   18
#define ACQ_RATE_TOL    0.05      /* DRDY count rate vs nominal: wider than the LTC6903's 1 % */
#define ACQ_OSR_MIN     512

/* acq_sample_t.flags: set on every output whose FIR window holds such an input */
#define ACQ_F_GAP       0x01      /* an input sample was lost and held */
#define ACQ_F_CRC       0x02      /* an input frame failed its CRC and was held */
#define ACQ_F_UNSETTLED 0x04      /* sinc3 settling after a SYNC, inputs held */
#define ACQ_F_CLIP      0x08      /* an input sample was at full scale */
#define ACQ_F_TEST0     0x10      /* CH0 input mux off the amplifier (bench short or test signal) */
#define ACQ_F_TEST1     0x20      /* CH1 likewise */
#define ACQ_F_TEST(c)   (ACQ_F_TEST0 << (c))

/* How DRDY and 1 Hz reference edges get their time. Chosen at boot. */
typedef enum {
    ACQ_CAPTURE_ISR = 0,  /* GPIO interrupt reads esp_timer: ~10 us jitter under WiFi load */
    ACQ_CAPTURE_MCPWM,    /* MCPWM capture timer latches the edge in hardware: 6.25 ns
                             counts, kept as us in the esp_timer domain */
} acq_capture_t;

typedef enum {
    ACQ_CLK_EXT = 0,      /* oscillator on CLKIN, LTC6903 output cut */
    ACQ_CLK_LEDC,         /* GPIO7 LEDC from XTAL, integer divider   */
    ACQ_CLK_LTC6903,      /* stock Click: program the LTC6903        */
} acq_clk_src_t;

typedef struct {
    acq_clk_src_t clk_src;
    uint32_t clk_hz;      /* nominal CLKIN */
    uint16_t osr;
    uint8_t  decim;       /* 1 = raw rate */
    uint8_t  taps;        /* odd, FIR length at the input rate */
    uint8_t  pga[ACQ_NCH];
    int16_t  phase[ACQ_NCH];
    float    gcal[ACQ_NCH];
    int32_t  ocal[ACQ_NCH];
} acq_settings_t;

typedef struct {
    double   mean;        /* counts */
    double   rms;         /* counts, AC (about the mean) */
    int32_t  min, max;    /* counts */
    uint32_t clips;       /* input samples at full scale in the last window */
} acq_ch_stats_t;

typedef struct {
    bool     running;
    bool     adc_ok;          /* ID matched at the last (re)start */
    bool     clock_ok;        /* DRDY seen within the last 100 ms */
    uint16_t adc_id;
    uint16_t adc_status;
    uint32_t clk_hz_actual;   /* what the clock path was set to */
    double   fs_in;           /* nominal input rate, SPS */
    double   fs_out;          /* nominal output rate, SPS */
    double   group_delay_s;   /* sinc3 + FIR */
    uint64_t n_in;            /* input periods since (re)start, lost ones included */
    uint32_t seq;             /* output samples since (re)start */
    uint32_t crc_errors;
    uint32_t overruns;        /* input samples lost and held */
    uint32_t stale_frames;    /* backlog frames dropped: older than the DRDY count allows */
    uint32_t status_events;   /* unexpected STATUS: self-reset, resync */
    uint32_t cap_bad;         /* MCPWM edge times outside the sanity window, since boot */
    int32_t  cap_bad_us;      /* the last one's distance from the ISR time, us */
    double   drdy_rate;       /* DRDY edges per esp_timer second, last 1 s check */
    uint32_t drdy_rate_bad;   /* 1 s checks off nominal by more than ACQ_RATE_TOL */
    uint32_t restarts;
    uint8_t  mux[ACQ_NCH];    /* ads_mux_t in effect: 0 = amplifier; a restart sets it back */
    acq_ch_stats_t ch[ACQ_NCH];   /* last full 1 s window of output samples */
} acq_status_t;

typedef struct {
    uint32_t seq;
    uint8_t  flags;           /* ACQ_F_* */
    uint8_t  tq;              /* tb_tq_t: how utc_us was obtained */
    uint16_t epoch;           /* low bits of the time-grid epoch; sits in what was padding */
    int64_t  utc_us;          /* 0 when no wall-clock time yet */
    int32_t  v[ACQ_NCH];
} acq_sample_t;

/* One unbroken run of output samples, from acq_window_run(). */
typedef struct {
    double   fs;              /* output rate of these samples, SPS */
    int64_t  t0_us;           /* UTC of the first one, 0 when no wall-clock time */
    uint16_t epoch;
    uint8_t  tq;              /* tb_tq_t of every sample in the run */
} acq_run_t;

/**
 * @brief Create the task and start acquiring with s.
 * @param capture  edge timing for the whole run; ACQ_CAPTURE_MCPWM falls back
 *                 to the ISR path if the capture timer cannot be set up
 */
esp_err_t acq_start(const acq_settings_t *s, acq_capture_t capture);

/** @brief Edge timing in use (after any fallback). */
acq_capture_t acq_capture_mode(void);

/**
 * @brief Add a capture channel on the acquisition capture timer, so that edge
 *        shares a time base with DRDY. For the 1 Hz reference.
 * @param on_edge  called from the capture ISR with the edge time in esp_timer
 *                 microseconds; must be IRAM_ATTR and ISR-safe
 * @return ESP_ERR_NOT_SUPPORTED when capture is not in use
 */
esp_err_t acq_capture_add(int gpio, bool rising, void (*on_edge)(int64_t t_us));

/** @brief Apply new settings: the task reconfigures itself and restarts. */
void acq_apply(const acq_settings_t *s);

/** @brief Stop reading (before OTA flash writes or a restart). */
void acq_pause(void);

/** @brief Name of an ads_mux_t value: "normal", "short", "testp", "testn". */
const char *acq_mux_name(uint8_t mux);

/** @brief Runtime commands, executed by the acquisition task. */
void acq_request_sync(void);
void acq_request_mux(uint8_t ch, uint8_t mux);   /* ads_mux_t, not persisted */

void acq_get_status(acq_status_t *out);

/**
 * @brief Pick the window of samples from since_seq on, in seq order.
 *
 * When more than max are waiting, oldest_first picks which max: false
 * gives the newest (live display, skips the backlog), true gives the
 * oldest still served (recorder catching up, nothing skipped). Once the
 * ring has wrapped, its oldest 512 samples are not served, so a reader
 * has time to walk the window with acq_read_block().
 *
 * Lock-free: the acquisition task is the only writer and never waits on a
 * reader. Any number of readers.
 * @param[out] first     seq of the first sample in the window
 * @param[out] next_seq  seq to ask for next time
 * @param[out] pending   samples newer than the window
 * @return number of samples in the window
 */
int acq_read_window(uint32_t since_seq, int max, bool oldest_first,
                    uint32_t *first, uint32_t *next_seq, uint32_t *pending);

/**
 * @brief Copy n output samples starting at seq first (from acq_read_window).
 *
 * No allocation: a reader walks its window in small blocks. Returns how
 * many leading entries the writer overwrote during the copy; those are not
 * valid. 0 is the normal case.
 */
int acq_read_block(uint32_t first, acq_sample_t *out, int n);

/**
 * @brief Trim a window to one unbroken run, so t0 + i / fs holds across it.
 *
 * A run ends where the epoch changes (restart, SYNC, resync, long gap), the
 * time quality changes, or two neighbours are not one sample period apart
 * to within half a period (a sample-clock step). keep_newest keeps the run
 * that ends the window (live display), otherwise the one that starts it
 * (recorder: the rest follows on the next read). Leading samples the writer
 * already overwrote are dropped too.
 * @param[in,out] first, cnt  the window from acq_read_window, trimmed in place
 * @param[out]    run         rate, first timestamp, epoch and tq of the run
 * @return samples dropped
 */
int acq_window_run(uint32_t *first, int *cnt, bool keep_newest, acq_run_t *run);

/**
 * @brief Print the log lines the acquisition task queued.
 *
 * The acquisition task never writes the console while it streams. Called
 * from the timebase task, at least once a second.
 */
void acq_log_drain(void);

/** @brief Output samples the ring holds (ACQ_RING_LEN_PSRAM or ACQ_RING_LEN). */
uint32_t acq_ring_len(void);

/**
 * @brief Input-sample position of the latest DRDY, for the 1 Hz rate loop.
 * @param[out] n      input samples since (re)start
 * @param[out] t_us   esp_timer time of that DRDY
 * @param[out] epoch  increments on every restart; positions across epochs do not compare
 */
void acq_get_position(uint64_t *n, int64_t *t_us, uint32_t *epoch);

/** @brief Nominal input rate for the current settings. */
double acq_fs_in(void);

#endif /* ACQ_H */
