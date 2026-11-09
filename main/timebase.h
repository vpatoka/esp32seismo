/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Vlad Patoka (VE3VPT) */
/**
 * @file timebase.h
 * @brief Wall-clock time and the 1 Hz rate reference.
 *
 * Two separate jobs:
 *   UTC labels    SNTP sets the system clock. A DS323x (optional) seeds it
 *                 at boot and holds it between syncs. Sample timestamps are
 *                 esp_timer DRDY times mapped through the system clock.
 *   Rate          a 1 Hz edge (GPS PPS, or DS323x SQW) on REF_1HZ_GPIO is
 *                 compared against the ADC sample counter. That measures the
 *                 true sample rate in ppm whatever drives CLKIN, and the ESP
 *                 crystal error as a by-product.
 *
 * With GPS PPS the second boundary itself comes from the PPS edge and SNTP
 * only names the second, the usual amateur-station split (PSN-ADC24 does
 * the same with its GPS). NMEA parsing is not needed for that.
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

typedef struct {
    bool     time_valid;
    const char *time_src;     /* "none", "rtc", "ntp" */
    uint32_t ntp_syncs;
    int64_t  ntp_age_s;       /* -1 = never */

    tb_ref_t ref;
    bool     ref_rising;
    bool     ref_seen;
    uint32_t ref_edges;
    int64_t  ref_age_ms;      /* -1 = never */
    bool     rate_valid;
    double   fs_measured;     /* ADC samples per reference second (smoothed) */
    double   adc_ppm;         /* measured vs nominal input rate */
    double   esp_ppm;         /* ESP timer vs reference */

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

/** @brief esp_timer time to UTC microseconds, 0 when there is no wall-clock time yet. */
int64_t timebase_esp_to_utc_us(int64_t t_esp_us);

bool timebase_time_valid(void);
void timebase_get_status(tb_status_t *out);
const char *timebase_ref_name(tb_ref_t r);

#endif /* TIMEBASE_H */
