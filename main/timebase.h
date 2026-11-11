/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Vlad Patoka (VE3VPT) */
/**
 * @file timebase.h
 * @brief Wall-clock time and the 1 Hz rate reference.
 *
 * Two separate jobs:
 *   UTC labels    SNTP sets the system clock. A DS323x (optional) seeds it
 *                 at boot and holds it between syncs.
 *   Rate          a 1 Hz edge (GPS PPS, or DS323x SQW) on REF_1HZ_GPIO is
 *                 compared against the ADC sample counter. That measures the
 *                 true sample rate in ppm whatever drives CLKIN, and the ESP
 *                 crystal error as a by-product.
 *
 * Sample timestamps come from one of two models, reported per sample as a
 * time quality (tb_tq_t):
 *   pps       GPS PPS present and the rate fit valid: the time of a sample is
 *             computed from its position in the ADC sample count,
 *               t(pos) = UTC_x0 + x(pos),  x(pos) = (pos - pos0 - a) / (fs + b)
 *             where the fitted line pos = pos0 + fs x + a + b x runs through
 *             the PPS edges. The PPS edge defines the second, SNTP only names
 *             it (rounded to the nearest second), so SNTP error and steps and
 *             DRDY interrupt jitter stay out of the timestamps. NMEA is not
 *             needed. Held up to TB_PPS_HOLDOVER_S after the last good edge.
 *   ntp, rtc  no usable PPS: the DRDY esp_timer time plus an offset that
 *             follows the system clock at no more than TB_SLEW_PPM. SNTP keeps
 *             stepping the system clock (so log times are exact), but sample
 *             timestamps never step: a correction is slewed in.
 * A DS323x SQW reference measures the rate only. Its edges are not UTC
 * seconds, so it never selects the pps model.
 *
 * The task owns the I2C bus and the DS323x.
 */

#ifndef TIMEBASE_H
#define TIMEBASE_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

typedef enum {
    TB_REF_NONE = 0,
    TB_REF_GPS,           /* GPS PPS, push-pull */
    TB_REF_RTC,           /* DS323x INT/SQW at 1 Hz, open drain */
} tb_ref_t;

#define TB_PPS_HOLDOVER_S   30      /* pps model kept this long after the last good edge */
#define TB_SLEW_PPM         500.0   /* sample-clock slew limit without pps */
#define TB_STEP_US          500000  /* offsets beyond this are applied at once: a leap
                                       second or a bad RTC seed steps, SNTP drift slews */

/* Time quality of a sample timestamp, best last */
typedef enum {
    TB_TQ_NONE = 0,       /* no wall-clock time: utc_us is 0 */
    TB_TQ_RTC,            /* RTC-seeded system clock, no SNTP yet */
    TB_TQ_NTP,            /* SNTP system clock, slewed */
    TB_TQ_PPS,            /* sample count against GPS PPS */
} tb_tq_t;

typedef struct {
    bool     time_valid;
    const char *time_src;     /* "none", "rtc", "ntp" */
    tb_tq_t  quality;         /* what a sample timestamped now would get */
    double   slew_ms;         /* system clock minus sample clock, still being slewed in */
    uint32_t pps_naming_errors;  /* times the SNTP second name of PPS edges was re-anchored */
    uint32_t ntp_syncs;
    uint32_t ntp_rejected;    /* SNTP samples held for confirmation or implausible */
    int64_t  ntp_age_s;       /* -1 = never */

    tb_ref_t ref;
    bool     ref_rising;
    bool     ref_seen;
    uint32_t ref_edges;
    int64_t  ref_age_ms;      /* -1 = never */
    bool     rate_valid;
    double   fs_measured;     /* ADC samples per reference second, least-squares fit */
    double   adc_ppm;         /* measured vs nominal input rate */
    double   esp_ppm;         /* ESP timer vs reference */
    double   rate_sigma_ppm;  /* 1-sigma uncertainty of adc_ppm from the fit residuals */
    uint32_t rate_points;     /* reference edges in the fit window */
    uint32_t rate_rejects;    /* edges dropped as outliers since boot */

    bool     rtc_enabled;
    bool     rtc_online;
    const char *rtc_model;
    bool     rtc_osf;
    float    rtc_temp_c;
} tb_status_t;

/** @brief Start the task. bus may be NULL when no RTC is fitted. */
esp_err_t timebase_init(tb_ref_t ref, bool rising, bool use_rtc, i2c_master_bus_handle_t bus);

/** @brief Start SNTP. Call on the first IP address; later calls do nothing. */
void timebase_start_sntp(void);

/**
 * @brief esp_timer time to UTC microseconds through the slewed sample clock,
 *        0 when there is no wall-clock time yet. Never steps once set.
 */
int64_t timebase_esp_to_utc_us(int64_t t_esp_us);

/**
 * @brief UTC of an ADC input-sample position.
 * @param epoch     acquisition epoch the position belongs to
 * @param pos       fractional input-sample count (acq_get_position units)
 * @param t_esp_us  esp_timer time of that position, used without pps
 * @param[out] tq   time quality of the result
 */
int64_t timebase_sample_utc_us(uint32_t epoch, double pos, int64_t t_esp_us, tb_tq_t *tq);

const char *timebase_tq_name(tb_tq_t q);

bool timebase_time_valid(void);
void timebase_get_status(tb_status_t *out);
const char *timebase_ref_name(tb_ref_t r);

#endif /* TIMEBASE_H */
