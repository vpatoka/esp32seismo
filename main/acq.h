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
 * Output sample timestamps are the DRDY time of the newest input sample
 * minus the total group delay (ADC sinc3, 1.5 input periods, plus the FIR,
 * (taps - 1) / 2 input periods), mapped to UTC through the system clock.
 * They mark when the ground moved, not when the sample left the filter.
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
#define ACQ_RING_LEN    4096      /* output samples kept for the web API */

/* Limits the FIR and the task can honour. A Blackman FIR needs about
 * 14 x decim taps to keep aliases out of 0..0.4 x f_out; 255 taps cover
 * decim 18. Below OSR 512 (8 kSPS) one task wake per sample is too slow. */
#define ACQ_DECIM_MAX   18
#define ACQ_OSR_MIN     512

/* acq_sample_t.flags: set on every output whose FIR window holds such an input */
#define ACQ_F_GAP       0x01      /* an input sample was lost and held */
#define ACQ_F_CRC       0x02      /* an input frame failed its CRC and was held */
#define ACQ_F_UNSETTLED 0x04      /* sinc3 settling after a SYNC, inputs held */
#define ACQ_F_CLIP      0x08      /* an input sample was at full scale */

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
    uint32_t restarts;
    acq_ch_stats_t ch[ACQ_NCH];   /* last full 1 s window of output samples */
} acq_status_t;

typedef struct {
    uint32_t seq;
    uint8_t  flags;           /* ACQ_F_* */
    int64_t  utc_us;          /* 0 when no wall-clock time yet */
    int32_t  v[ACQ_NCH];
} acq_sample_t;

/** @brief Create the task and start acquiring with s. */
esp_err_t acq_start(const acq_settings_t *s);

/** @brief Apply new settings: the task reconfigures itself and restarts. */
void acq_apply(const acq_settings_t *s);

/** @brief Stop reading (before OTA flash writes or a restart). */
void acq_pause(void);

/** @brief Runtime commands, executed by the acquisition task. */
void acq_request_sync(void);
void acq_request_mux(uint8_t ch, uint8_t mux);   /* ads_mux_t, not persisted */

void acq_get_status(acq_status_t *out);

/**
 * @brief Copy samples newer than since_seq, oldest first.
 * @return number copied. *next_seq is the seq to ask for next time.
 */
int acq_read_since(uint32_t since_seq, acq_sample_t *out, int max, uint32_t *next_seq);

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
