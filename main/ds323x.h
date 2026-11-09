/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Vlad Patoka (VE3VPT) */
/**
 * @file ds323x.h
 * @brief Maxim DS3231 / DS3231M / DS3232 RTC driver (I2C master, IDF v6).
 *
 * The RTC holds UTC, always. The legacy firmware stored local time and
 * rewrote it on DST changes; we do not. Local time is a display concern.
 *
 * Year range: the chip has two BCD year digits plus the century bit in the
 * month register. We map century=0 to 20xx and century=1 to 21xx, so the
 * register set covers 2000-2199. That is the 50-year target with margin.
 *
 * Not thread safe by design: one owner task (timekeeper) does all I2C to
 * this chip. Everyone else reads the cached status from timekeeper.
 */

#ifndef DS323X_H
#define DS323X_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

typedef enum {
    DS323X_NONE = 0,    /* nothing answered at 0x68                          */
    DS323X_DS3231,      /* DS3231 or DS3231M: register map wraps after 0x12  */
    DS323X_DS3232,      /* DS3232: 236 bytes battery-backed SRAM at 0x14-0xFF */
} ds323x_model_t;

#define DS3232_SRAM_FIRST   0x14
#define DS3232_SRAM_LAST    0xFF

/** @brief Attach to the bus, probe, and identify the chip. */
esp_err_t ds323x_init(i2c_master_bus_handle_t bus);

/**
 * @brief Bring an offline or missing RTC back: clear the bus, probe, and
 *        identify it if it was never seen. Caller re-configures on ESP_OK.
 */
esp_err_t ds323x_recover(void);

/**
 * @brief True while the RTC is identified and answering. Three failed
 *        transfers in a row take it offline; calls then fail at once with
 *        ESP_ERR_INVALID_STATE and put nothing on the bus.
 */
bool ds323x_online(void);

ds323x_model_t ds323x_model(void);
const char *ds323x_model_name(void);

/**
 * @brief Put the chip in our operating state.
 *
 * CTRL = 0x00: oscillator on, battery SQW off, 1 Hz on INT/SQW (INTCN=0),
 * alarms off. STATUS: EN32kHz off, alarm flags cleared, OSF left as found
 * (only a validated time write clears it).
 */
esp_err_t ds323x_configure(void);

/**
 * @brief Read the time as UTC seconds.
 * @param[out] utc  Seconds since 1970 (64-bit time_t).
 * @param[out] osf  Oscillator Stop Flag. True means the time is not trustworthy.
 */
esp_err_t ds323x_read_time(time_t *utc, bool *osf);

/** @brief Read only the seconds register (0-59). One byte, for edge probing. */
esp_err_t ds323x_read_seconds(uint8_t *sec);

/**
 * @brief Write UTC seconds and clear OSF.
 *
 * Writing the seconds register resets the chip's sub-second countdown, so the
 * RTC second starts at the moment this transaction completes. Call it as
 * close to the true second boundary as possible.
 */
esp_err_t ds323x_write_time(time_t utc);

/** @brief Die temperature in quarter degrees C (10-bit, 0.25 C steps). */
esp_err_t ds323x_read_temp_q(int16_t *quarter_c);

/** @brief Aging offset register (two's complement, positive slows the clock). */
esp_err_t ds323x_get_aging(int8_t *aging);

/** @brief Write aging and force a TCXO conversion so it takes effect now. */
esp_err_t ds323x_set_aging(int8_t aging);

/** @brief DS3232 SRAM access. Returns ESP_ERR_NOT_SUPPORTED on a DS3231. */
esp_err_t ds323x_sram_read(uint8_t addr, uint8_t *buf, size_t len);
esp_err_t ds323x_sram_write(uint8_t addr, const uint8_t *buf, size_t len);

/** @brief I2C errors since boot (each one also triggered a bus reset). */
uint32_t ds323x_error_count(void);

/** @brief UTC broken-down time to seconds since 1970. No TZ, no tables, 64-bit safe. */
time_t ds323x_timegm(const struct tm *tm);

#endif /* DS323X_H */
