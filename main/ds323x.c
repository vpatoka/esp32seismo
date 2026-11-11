/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Vlad Patoka (VE3VPT) */
/**
 * @file ds323x.c
 * @brief DS3231 / DS3231M / DS3232 driver. See ds323x.h.
 *
 * Register facts used here (DS3231 datasheet 19-5170):
 *   0x00-0x06  sec, min, hour, day, date, month|century, year (BCD)
 *   0x0E CTRL  EOSC BBSQW CONV RS2 RS1 INTCN A2IE A1IE   (POR 0x1C)
 *   0x0F STAT  OSF 0 0 0 EN32kHz BSY A2F A1F
 *   0x10       aging offset, two's complement
 *   0x11/0x12  temperature, 10-bit two's complement, 0.25 C/LSB in 12h[7:6]
 * The DS3231 address pointer wraps from 0x12 back to 0x00. The DS3232 keeps
 * counting into its SRAM. That difference is how we tell them apart without
 * writing anything to a chip we have not identified yet.
 */

#include "ds323x.h"
#include "fault.h"
#include "io_map.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "DS323X";

#define REG_SECONDS   0x00
#define REG_MONTH     0x05
#define REG_CTRL      0x0E
#define REG_STATUS    0x0F
#define REG_AGING     0x10
#define REG_TEMP_MSB  0x11

#define CTRL_CONV     0x20
#define STAT_OSF      0x80
#define STAT_EN32K    0x08
#define STAT_BSY      0x04
#define STAT_A2F      0x02
#define STAT_A1F      0x01
#define MONTH_CENTURY 0x80

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static ds323x_model_t          s_model = DS323X_NONE;
static uint32_t                s_errors;
static uint32_t                s_consec_errors;
static bool                    s_online;

#define OFFLINE_AFTER_ERRORS   3    /* consecutive failures before we stop talking */
#define PROBE_ATTEMPTS         3
#define ERROR_LOG_GAP_US       (60 * 1000000LL)  /* at most one I2C error line a minute */

/* ── helpers ────────────────────────────────────────────────────────────── */

static inline uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static inline uint8_t bin2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/* Clock a stuck slave free: FSM reset, up to 9 SCL pulses until SDA rises,
 * then STOP. A DS3231 runs on its battery through our resets, so one caught
 * mid-read keeps SDA low until we do this. */
static esp_err_t bus_clear(void)
{
    return s_bus ? i2c_master_bus_reset(s_bus) : ESP_ERR_INVALID_STATE;
}

/* Every transfer goes through here. Bounded timeout; on failure clear the
 * bus. After a few failures in a row the RTC is taken offline and we stop
 * talking to it, so a dead chip costs nothing per second. The timekeeper
 * brings it back through ds323x_recover() with a backoff. Nothing here can
 * block longer than one timeout, and no caller depends on the RTC to keep
 * the displayed time moving. */
static esp_err_t xfer(const uint8_t *w, size_t wl, uint8_t *r, size_t rl)
{
    if (!s_dev || !s_online) return ESP_ERR_INVALID_STATE;
    esp_err_t err = rl
        ? i2c_master_transmit_receive(s_dev, w, wl, r, rl, I2C_XFER_TIMEOUT_MS)
        : i2c_master_transmit(s_dev, w, wl, I2C_XFER_TIMEOUT_MS);
    if (err == ESP_OK) {
        s_consec_errors = 0;
        return ESP_OK;
    }

    /* Rate limit: the first error logs at once, then at most one line a
     * minute, carrying how many errors went unlogged in between. A noisy
     * bus that glitches every few seconds stays readable. */
    static int64_t  s_next_log_us;
    static uint32_t s_unlogged;
    int64_t now = esp_timer_get_time();

    s_errors++;
    s_consec_errors++;
    if (now >= s_next_log_us) {
        if (s_unlogged) {
            ESP_LOGW(TAG, "I2C error %s, clearing bus (%lu errors since boot, %lu more not shown)",
                     esp_err_to_name(err), (unsigned long)s_errors, (unsigned long)s_unlogged);
        } else {
            ESP_LOGW(TAG, "I2C error %s, clearing bus (%lu errors since boot)",
                     esp_err_to_name(err), (unsigned long)s_errors);
        }
        s_unlogged = 0;
        s_next_log_us = now + ERROR_LOG_GAP_US;
    } else {
        s_unlogged++;
    }
    ESP_LOGD(TAG, "Failed transfer: reg 0x%02X, write %u, read %u, errors so far %lu",
             wl ? w[0] : 0xFF, (unsigned)wl, (unsigned)rl, (unsigned long)s_errors);
    if (bus_clear() == ESP_OK) fault_raise(FAULT_I2C_RECOVERED);

    if (s_consec_errors >= OFFLINE_AFTER_ERRORS) {
        s_online = false;
        fault_raise(FAULT_RTC_MISSING);
        /* The timekeeper logs the drop, with flap detection. */
        ESP_LOGD(TAG, "RTC offline after %d failed transfers in a row", OFFLINE_AFTER_ERRORS);
    }
    return err;
}

/* One bus clear per probe cycle. Clearing before every attempt adds nothing
 * after the first: if 9 clocks and a STOP did not free SDA, the next round
 * will not either. */
static esp_err_t probe(void)
{
    esp_err_t err = ESP_FAIL;
    bus_clear();
    for (int i = 0; i < PROBE_ATTEMPTS; i++) {
        err = i2c_master_probe(s_bus, DS323X_I2C_ADDR, I2C_XFER_TIMEOUT_MS);
        if (err == ESP_OK) break;
    }
    return err;
}

static esp_err_t read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    return xfer(&reg, 1, buf, len);
}

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    const uint8_t w[2] = { reg, val };
    return xfer(w, sizeof(w), NULL, 0);
}

/* Read-modify-write: bits in mask take their value from val, the rest stay.
 * Flag bits that can only be cleared (OSF, A1F, A2F) are safe to write back
 * as read: writing 1 to them does nothing. */
static esp_err_t modify_reg(uint8_t reg, uint8_t mask, uint8_t val)
{
    uint8_t cur;
    ESP_RETURN_ON_ERROR(read_regs(reg, &cur, 1), TAG, "read reg 0x%02X", reg);
    uint8_t next = (uint8_t)((cur & ~mask) | (val & mask));
    if (next == cur) return ESP_OK;
    ESP_RETURN_ON_ERROR(write_reg(reg, next), TAG, "write reg 0x%02X", reg);
    return ESP_OK;
}

/* Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant's
 * algorithm). Pure integer math, valid far beyond any year we care about. */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

time_t ds323x_timegm(const struct tm *tm)
{
    int64_t days = days_from_civil((int64_t)tm->tm_year + 1900,
                                   (unsigned)tm->tm_mon + 1, (unsigned)tm->tm_mday);
    return (time_t)(days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec);
}

/* ── identification ─────────────────────────────────────────────────────── */

/* Read 26 bytes from 0x00 in one transaction. On a DS3231 bytes 0x13..0x19
 * are the time registers again (pointer wrapped). The chip re-latches the
 * time when the pointer wraps, so seconds can differ by a tick; compare
 * minutes through year, and retry to ride out a minute rollover. */
static bool looks_like_ds3231(void)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        uint8_t b[0x1A];
        if (read_regs(REG_SECONDS, b, sizeof(b)) != ESP_OK) return false;
        if (memcmp(&b[0x14], &b[0x01], 6) == 0) return true;
    }
    return false;
}

/* Only called once the wrap test says "not a DS3231": flip one SRAM byte,
 * read it back, restore it. */
static bool sram_responds(void)
{
    const uint8_t addr = DS3232_SRAM_LAST;
    uint8_t orig, check;
    if (read_regs(addr, &orig, 1) != ESP_OK) return false;
    if (write_reg(addr, (uint8_t)~orig) != ESP_OK) return false;
    bool ok = (read_regs(addr, &check, 1) == ESP_OK) && check == (uint8_t)~orig;
    write_reg(addr, orig);
    return ok;
}

esp_err_t ds323x_init(i2c_master_bus_handle_t bus)
{
    static bool s_missing_logged;

    s_bus = bus;
    s_model = DS323X_NONE;
    s_online = false;

    /* i2c_master_bus_reset() re-reserves SDA/SCL, which the bus already
     * holds, and the driver warns "GPIO n is not usable" on every clear.
     * Real I2C errors still log at error level. */
    esp_log_level_set("i2c.common", ESP_LOG_ERROR);

    /* Clear first: the chip may still hold SDA from a read our last reset
     * interrupted. */
    esp_err_t err = probe();
    if (err != ESP_OK) {
        /* Loud once; later re-probes of a chip that is not fitted stay quiet. */
        if (!s_missing_logged) {
            ESP_LOGE(TAG, "No RTC at 0x%02X (%s)", DS323X_I2C_ADDR, esp_err_to_name(err));
            s_missing_logged = true;
        } else {
            ESP_LOGD(TAG, "No RTC at 0x%02X (%s)", DS323X_I2C_ADDR, esp_err_to_name(err));
        }
        return ESP_ERR_NOT_FOUND;
    }
    s_missing_logged = false;

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = DS323X_I2C_ADDR,
        .scl_speed_hz    = I2C_BUS_HZ,
    };
    if (!s_dev) {
        err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "add_device failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    s_online = true;
    s_consec_errors = 0;

    if (looks_like_ds3231()) {
        s_model = DS323X_DS3231;
    } else if (sram_responds()) {
        s_model = DS323X_DS3232;
    } else if (!s_online) {
        /* Went offline during identification. */
        return ESP_ERR_TIMEOUT;
    } else {
        /* Answers but fits neither pattern. Run it as a DS3231, no SRAM. */
        s_model = DS323X_DS3231;
        ESP_LOGW(TAG, "RTC identification inconclusive, treating as DS3231");
    }
    ESP_LOGI(TAG, "RTC found: %s at 0x%02X (I2C SDA %d, SCL %d, %d kHz)",
             ds323x_model_name(), DS323X_I2C_ADDR, (int)I2C_SDA_GPIO, (int)I2C_SCL_GPIO,
             I2C_BUS_HZ / 1000);
    return ESP_OK;
}

esp_err_t ds323x_recover(void)
{
    if (!s_bus) return ESP_ERR_INVALID_STATE;
    if (s_model == DS323X_NONE) return ds323x_init(s_bus);   /* never identified */
    if (probe() != ESP_OK) return ESP_ERR_NOT_FOUND;
    s_online = true;
    s_consec_errors = 0;
    ESP_LOGD(TAG, "RTC answering again");     /* the timekeeper logs it with the outage time */
    return ESP_OK;
}

bool ds323x_online(void) { return s_online && s_model != DS323X_NONE; }

ds323x_model_t ds323x_model(void) { return s_model; }

const char *ds323x_model_name(void)
{
    switch (s_model) {
        case DS323X_DS3231: return "DS3231";
        case DS323X_DS3232: return "DS3232";
        default:            return "none";
    }
}

uint32_t ds323x_error_count(void) { return s_errors; }

/* ── operation ──────────────────────────────────────────────────────────── */

esp_err_t ds323x_configure(void)
{
    /* Every CTRL bit is ours: oscillator on, no battery SQW, 1 Hz, INTCN=0,
     * alarms off. A plain write is the intent, not a merge. */
    ESP_RETURN_ON_ERROR(write_reg(REG_CTRL, 0x00), TAG, "write CTRL");
    /* EN32kHz off, alarm flags cleared, OSF left as found. */
    ESP_RETURN_ON_ERROR(modify_reg(REG_STATUS, STAT_EN32K | STAT_A2F | STAT_A1F, 0),
                        TAG, "clear STATUS flags");

    /* Read back, so the log shows what the chip holds, not what we sent.
     * CONV self-clears when a conversion ends; leave it out of the check. */
    uint8_t cs[2];
    ESP_RETURN_ON_ERROR(read_regs(REG_CTRL, cs, sizeof(cs)), TAG, "read back CTRL/STATUS");
    if ((cs[0] & (uint8_t)~CTRL_CONV) != 0x00) {
        ESP_LOGW(TAG, "CTRL reads 0x%02X after writing 0x00: SQW will not run", cs[0]);
    } else {
        ESP_LOGI(TAG, "RTC configured: CTRL 0x%02X (1 Hz on SQW, 32 kHz off), STATUS 0x%02X (OSF %d)",
                 cs[0], cs[1], (cs[1] & STAT_OSF) ? 1 : 0);
    }
    return ESP_OK;
}

esp_err_t ds323x_read_time(time_t *utc, bool *osf)
{
    /* 0x00..0x0F in one go: time plus STATUS, consistent snapshot. */
    uint8_t b[0x10];
    ESP_RETURN_ON_ERROR(read_regs(REG_SECONDS, b, sizeof(b)), TAG, "read time");

    struct tm tm = { 0 };
    tm.tm_sec  = bcd2bin(b[0] & 0x7F);
    tm.tm_min  = bcd2bin(b[1] & 0x7F);
    if (b[2] & 0x40) {                       /* 12-hour mode: legacy setting */
        int h = bcd2bin(b[2] & 0x1F) % 12;
        tm.tm_hour = h + ((b[2] & 0x20) ? 12 : 0);
    } else {
        tm.tm_hour = bcd2bin(b[2] & 0x3F);
    }
    tm.tm_mday = bcd2bin(b[4] & 0x3F);
    tm.tm_mon  = bcd2bin(b[5] & 0x1F) - 1;
    int year   = 2000 + bcd2bin(b[6]) + ((b[5] & MONTH_CENTURY) ? 100 : 0);
    tm.tm_year = year - 1900;

    bool sane = tm.tm_sec < 60 && tm.tm_min < 60 && tm.tm_hour < 24 &&
                tm.tm_mday >= 1 && tm.tm_mday <= 31 && tm.tm_mon >= 0 && tm.tm_mon < 12;
    ESP_RETURN_ON_FALSE(sane, ESP_ERR_INVALID_RESPONSE, TAG,
                        "time registers out of range: %02X %02X %02X %02X %02X %02X",
                        b[0], b[1], b[2], b[4], b[5], b[6]);

    if (utc) *utc = ds323x_timegm(&tm);
    if (osf) *osf = (b[REG_STATUS] & STAT_OSF) != 0;
    return ESP_OK;
}

esp_err_t ds323x_read_seconds(uint8_t *sec)
{
    uint8_t v;
    ESP_RETURN_ON_ERROR(read_regs(REG_SECONDS, &v, 1), TAG, "read seconds");
    if (sec) *sec = bcd2bin(v & 0x7F);
    return ESP_OK;
}

esp_err_t ds323x_write_time(time_t utc)
{
    struct tm tm;
    gmtime_r(&utc, &tm);
    int year = tm.tm_year + 1900;
    ESP_RETURN_ON_FALSE(year >= 2000 && year <= 2199, ESP_ERR_INVALID_ARG, TAG,
                        "year %d outside 2000-2199", year);

    const uint8_t w[8] = {
        REG_SECONDS,
        bin2bcd((uint8_t)tm.tm_sec),
        bin2bcd((uint8_t)tm.tm_min),
        bin2bcd((uint8_t)tm.tm_hour),                /* bit6=0: 24-hour mode */
        (uint8_t)(tm.tm_wday == 0 ? 7 : tm.tm_wday), /* 1..7, Monday=1 */
        bin2bcd((uint8_t)tm.tm_mday),
        (uint8_t)(bin2bcd((uint8_t)(tm.tm_mon + 1)) | (year >= 2100 ? MONTH_CENTURY : 0)),
        bin2bcd((uint8_t)(year % 100)),
    };
    ESP_RETURN_ON_ERROR(xfer(w, sizeof(w), NULL, 0), TAG, "write time");
    /* The time is good again: clear OSF, keep 32 kHz and alarm flags off. */
    ESP_RETURN_ON_ERROR(modify_reg(REG_STATUS, STAT_OSF | STAT_EN32K | STAT_A2F | STAT_A1F, 0),
                        TAG, "clear OSF");
    return ESP_OK;
}

esp_err_t ds323x_read_temp_q(int16_t *quarter_c)
{
    uint8_t b[2];
    ESP_RETURN_ON_ERROR(read_regs(REG_TEMP_MSB, b, sizeof(b)), TAG, "read temperature");
    /* MSB is signed integer degrees; LSB bits 7:6 are the quarters. */
    int16_t raw = (int16_t)(((uint16_t)b[0] << 8) | b[1]);
    if (quarter_c) *quarter_c = (int16_t)(raw / 64);   /* exact: low 6 bits are 0 */
    return ESP_OK;
}

esp_err_t ds323x_get_aging(int8_t *aging)
{
    uint8_t v;
    ESP_RETURN_ON_ERROR(read_regs(REG_AGING, &v, 1), TAG, "read aging");
    if (aging) *aging = (int8_t)v;
    return ESP_OK;
}

esp_err_t ds323x_set_aging(int8_t aging)
{
    ESP_RETURN_ON_ERROR(write_reg(REG_AGING, (uint8_t)aging), TAG, "write aging");

    /* Force a TCXO conversion so the new value applies now, not at the next
     * 64 s auto conversion. One already in flight (BSY) picks it up. */
    uint8_t st;
    ESP_RETURN_ON_ERROR(read_regs(REG_STATUS, &st, 1), TAG, "read STATUS");
    if (!(st & STAT_BSY)) {
        ESP_RETURN_ON_ERROR(modify_reg(REG_CTRL, CTRL_CONV, CTRL_CONV), TAG, "start CONV");
    }
    return ESP_OK;
}

static bool sram_range_ok(uint8_t addr, size_t len)
{
    return addr >= DS3232_SRAM_FIRST && len > 0 && (size_t)addr + len - 1 <= DS3232_SRAM_LAST;
}

esp_err_t ds323x_sram_read(uint8_t addr, uint8_t *buf, size_t len)
{
    ESP_RETURN_ON_FALSE(s_model == DS323X_DS3232, ESP_ERR_NOT_SUPPORTED, TAG, "no SRAM on %s",
                        ds323x_model_name());
    ESP_RETURN_ON_FALSE(sram_range_ok(addr, len), ESP_ERR_INVALID_ARG, TAG,
                        "SRAM range 0x%02X+%u", addr, (unsigned)len);
    ESP_RETURN_ON_ERROR(read_regs(addr, buf, len), TAG, "read SRAM");
    return ESP_OK;
}

esp_err_t ds323x_sram_write(uint8_t addr, const uint8_t *buf, size_t len)
{
    ESP_RETURN_ON_FALSE(s_model == DS323X_DS3232, ESP_ERR_NOT_SUPPORTED, TAG, "no SRAM on %s",
                        ds323x_model_name());
    ESP_RETURN_ON_FALSE(sram_range_ok(addr, len) && len <= 32, ESP_ERR_INVALID_ARG, TAG,
                        "SRAM range 0x%02X+%u", addr, (unsigned)len);
    uint8_t w[33];
    w[0] = addr;
    memcpy(&w[1], buf, len);
    ESP_RETURN_ON_ERROR(xfer(w, len + 1, NULL, 0), TAG, "write SRAM");
    return ESP_OK;
}
