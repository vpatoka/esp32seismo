/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Vlad Patoka (VE3VPT) */
/**
 * @file ads131m0x.h
 * @brief TI ADS131M02 / M03 / M04 driver (SPI mode 1, 24-bit words).
 *
 * Frame layout, both directions, one word per slot, 24-bit words:
 *   MOSI: command | (WREG data) | 0 ... | 0 (CRC slot, input CRC off)
 *   MISO: response | CH0 | CH1 [| CH2 | CH3] | CRC-16
 * Commands are 16 bits left-aligned in the 24-bit word. The response to a
 * command arrives in the NEXT frame; the response to NULL is the STATUS
 * register. Channel data is 24-bit two's complement; full scale is
 * +-1.2 V / PGA gain on the internal reference.
 *
 * Sample rate: f_data = f_CLKIN / 2 / OSR. 8.192 MHz at OSR 4096 = 1000 SPS.
 *
 * Not thread safe by design: the acquisition task owns the SPI device.
 */

#ifndef ADS131M0X_H
#define ADS131M0X_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/spi_master.h"

/* Commands (16-bit) */
#define ADS_CMD_NULL        0x0000
#define ADS_CMD_RESET       0x0011
#define ADS_CMD_STANDBY     0x0022
#define ADS_CMD_WAKEUP      0x0033
#define ADS_CMD_LOCK        0x0555
#define ADS_CMD_UNLOCK      0x0655
#define ADS_CMD_RREG(a, n)  (0xA000 | (((a) & 0x3F) << 7) | (((n) - 1) & 0x7F))
#define ADS_CMD_WREG(a, n)  (0x6000 | (((a) & 0x3F) << 7) | (((n) - 1) & 0x7F))

/* Registers */
#define ADS_REG_ID          0x00
#define ADS_REG_STATUS      0x01
#define ADS_REG_MODE        0x02
#define ADS_REG_CLOCK       0x03
#define ADS_REG_GAIN1       0x04
#define ADS_REG_CFG         0x06
#define ADS_REG_THRSHLD_MSB 0x07
#define ADS_REG_THRSHLD_LSB 0x08
#define ADS_REG_CH_CFG(c)   (0x09 + 5 * (c))
#define ADS_REG_CH_OCAL_MSB(c) (0x0A + 5 * (c))
#define ADS_REG_CH_OCAL_LSB(c) (0x0B + 5 * (c))
#define ADS_REG_CH_GCAL_MSB(c) (0x0C + 5 * (c))
#define ADS_REG_CH_GCAL_LSB(c) (0x0D + 5 * (c))
#define ADS_REG_REGMAP_CRC  0x3E

/* STATUS bits */
#define ADS_ST_LOCK         (1u << 15)
#define ADS_ST_F_RESYNC     (1u << 14)
#define ADS_ST_REG_MAP      (1u << 13)
#define ADS_ST_CRC_ERR      (1u << 12)
#define ADS_ST_RESET        (1u << 10)
#define ADS_ST_WLENGTH_MASK (3u << 8)
#define ADS_ST_WLENGTH_24   (1u << 8)
#define ADS_ST_DRDY0        (1u << 0)   /* new data on CH0 in this frame */
#define ADS_ST_DRDY1        (1u << 1)
#define ADS_ST_DRDY_MASK(nch) ((uint16_t)((1u << (nch)) - 1))

/* sinc3 decimator: symmetric impulse response 3 x OSR modulator clocks long,
 * so the output lags the input by 1.5 data periods; the first 3 periods
 * after a SYNC or a reset are not settled. Confirm the latency on the bench. */
#define ADS_SINC3_DELAY_PERIODS 1.5
#define ADS_SINC3_SETTLE        3

/* Data FIFO depth per channel ("Collecting Data for the First Time or After
 * a Pause"): two unread conversions are kept, the oldest is dropped first. */
#define ADS_FIFO_DEPTH      2

/* MODE: 24-bit words, SPI timeout on, DRDY from the lagging channel, DRDY
 * as a level (low until the frame is read), RESET flag cleared. */
#define ADS_MODE_RUN        0x0110

/* Input mux per channel (CHn_CFG[1:0]) */
typedef enum {
    ADS_MUX_NORMAL = 0,   /* AINxP - AINxN                  */
    ADS_MUX_SHORT  = 1,   /* inputs shorted: offset, noise  */
    ADS_MUX_TESTP  = 2,   /* positive DC test signal        */
    ADS_MUX_TESTN  = 3,   /* negative DC test signal        */
} ads_mux_t;

#define ADS_MAX_CH          4
#define ADS_FS_COUNTS       8388607     /* 2^23 - 1 */
#define ADS_VREF_V          1.2f

typedef struct {
    uint16_t id;          /* ID register, 0x22xx for M02 */
    uint8_t  nch;         /* channel count from the ID */
    uint16_t status;      /* last STATUS seen */
    uint32_t frames;
    uint32_t crc_errors;
} ads_info_t;

/** @brief Bus, device and SYNC/RESET pin. No traffic to the chip yet. */
esp_err_t ads_bus_init(void);

/** @brief Hardware reset through SYNC/RESET, then read and check the ID. */
esp_err_t ads_reset_and_probe(void);

/** @brief Short SYNC pulse: restarts the digital filters of all channels together. */
void ads_sync_pulse(void);

/** @brief Register access. A read costs two frames, a write two frames plus a check. */
esp_err_t ads_rreg(uint8_t addr, uint16_t *val);
esp_err_t ads_wreg(uint8_t addr, uint16_t val);

/**
 * @brief Full configuration for continuous conversion.
 * @param osr      128, 256, 512, 1024, 2048, 4096, 8192 or 16256
 * @param pga      per channel, 1..128 (powers of two)
 * @param phase    per channel, modulator clocks, -512..511
 * @param gcal     per channel, 0 .. 2, 1.0 = no correction
 * @param ocal     per channel, counts, +-2^23
 */
esp_err_t ads_configure(uint16_t osr, const uint8_t *pga, const int16_t *phase,
                        const float *gcal, const int32_t *ocal, uint8_t nch);

/** @brief Set one channel's input mux (keeps its phase setting). */
esp_err_t ads_set_mux(uint8_t ch, ads_mux_t mux, int16_t phase);

/**
 * @brief Read one data frame (call on DRDY). ch[] gets nch sign-extended samples.
 * @return ESP_OK, or ESP_ERR_INVALID_CRC when the output CRC does not match
 *         (data is still filled in, for diagnostics).
 */
esp_err_t ads_read_frame(int32_t *ch);

const ads_info_t *ads_info(void);

/** @brief OSR to register code, -1 if not a valid OSR. */
int ads_osr_code(uint16_t osr);

/** @brief Program the LTC6903 on the ADC 15 Click (stock clock path). */
esp_err_t ltc6903_set_hz(uint32_t hz, uint32_t *actual_hz);

#endif /* ADS131M0X_H */
