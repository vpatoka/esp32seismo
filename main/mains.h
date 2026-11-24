/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Vlad Patoka (VE3VPT) */
/**
 * @file mains.h
 * @brief Mains (60 Hz) frequency from a zero-cross edge on MAINS_ZC_GPIO.
 *
 * A transformer at the mains end of the enclosure feeds a TLV3501 comparator
 * front end on 3.3 V; its push-pull rising edge reaches carrier P8-2 (GPIO3)
 * through a 1k series resistor. Each edge is timed
 * on the acquisition MCPWM capture timer when capture=mcpwm (the same counter
 * as DRDY and the 1 Hz reference), otherwise by a GPIO interrupt reading
 * esp_timer. A task fits the period once a second and corrects it for the ESP
 * crystal error measured by timebase when the rate fit is valid.
 *
 * Never in the sample path: nothing here touches acquisition or timestamps.
 *
 * Storm breaker: more than MAINS_STORM_EDGES edges in one MAINS_STORM_BUCKET_US
 * window (normal is 6) turns the pad input off from inside the ISR, so a
 * noisy or oscillating input cannot starve acquisition on the single core.
 * The task turns it back on after MAINS_STORM_RETRY_S and gives up for good
 * after MAINS_STORM_MAX_TRIPS trips, until reboot.
 *
 * Off by default; enabled by the "mains" setting, which lives in its own NVS
 * key so that firmware without this module keeps its config on rollback.
 */

#ifndef MAINS_H
#define MAINS_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define MAINS_NOMINAL_HZ        60.0
#define MAINS_GATE_US           12000     /* edges closer than this are glitches (0.72 period) */
#define MAINS_DIP_US            100000    /* a gap this long between edges is a dip */
#define MAINS_ABSENT_US         1000000   /* no edge for this long: not present */
#define MAINS_STORM_BUCKET_US   100000
#define MAINS_STORM_EDGES       30        /* per bucket; 6 is normal */
#define MAINS_STORM_RETRY_S     30
#define MAINS_STORM_MAX_TRIPS   3
#define MAINS_HIST_S            60

typedef struct {
    bool     enabled;         /* mains_init() ran and the edge path is set up */
    bool     capture_hw;      /* MCPWM capture (true) or GPIO interrupt (false) */
    bool     present;         /* an edge within MAINS_ABSENT_US and the input is on */
    bool     corrected;       /* freq_hz corrected by the timebase esp_ppm */
    double   freq_hz;         /* last 1 s fit; NAN when none */
    double   freq_avg_hz;     /* mean of the valid 1 s values over MAINS_HIST_S; NAN when none */
    double   freq_min_hz, freq_max_hz;
    bool     time_error_valid;
    double   time_error_ms;   /* grid time minus UTC since the anchor; positive: grid ahead */
    int64_t  anchor_age_s;    /* seconds since the time-error anchor, -1 = none */
    uint64_t cycles;          /* counted since boot, missed edges included */
    uint32_t missed;          /* edges inferred from gaps of 2 to 6 periods */
    uint32_t glitches;        /* edges inside the glitch gate */
    uint32_t dips;            /* gaps of MAINS_DIP_US or longer */
    uint32_t last_dip_ms;     /* length of the last one */
    uint32_t storm_trips;
    bool     storm_latched;   /* MAINS_STORM_MAX_TRIPS reached: input off until reboot */
} mains_status_t;

/**
 * @brief Set up the edge path and start the task. Call after timebase_init(),
 *        so the capture timer's DRDY and reference channels are taken first.
 */
esp_err_t mains_init(void);

/** @brief Copy of the latest status. Safe from any task; zeroed when not enabled. */
void mains_get_status(mains_status_t *out);

#endif /* MAINS_H */
