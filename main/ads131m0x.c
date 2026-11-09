/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Vlad Patoka (VE3VPT) */
/**
 * @file ads131m0x.c
 * @brief TI ADS131M0x driver. See ads131m0x.h.
 *
 * The frame is short (12 bytes on an M02), so transfers use polling mode
 * with no DMA: a frame at 8 MHz takes about 15 us and never sleeps.
 *
 * The LTC6903 on the ADC 15 Click shares SCK and MOSI. It gets its own SPI
 * device (mode 0, CS2 on LTC6903_CS_GPIO) on the same bus.
 */

#include "ads131m0x.h"
#include "io_map.h"

#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ADS";

static spi_device_handle_t s_dev;
static spi_device_handle_t s_ltc;
static ads_info_t s_info = { .nch = 2 };

#define WORD_BYTES 3

static inline int frame_words(void) { return 2 + s_info.nch; }

/* CRC-16-CCITT, polynomial 0x1021, seed 0xFFFF (the chip's default CRC type). */
static uint16_t crc16_ccitt(const uint8_t *p, size_t n)
{
    uint16_t crc = 0xFFFF;
    while (n--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; i++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

static inline void put_word16(uint8_t *w, uint16_t v)
{
    w[0] = (uint8_t)(v >> 8);
    w[1] = (uint8_t)v;
    w[2] = 0;
}

static inline uint16_t get_word16(const uint8_t *w)
{
    return (uint16_t)((w[0] << 8) | w[1]);
}

static inline int32_t get_sample(const uint8_t *w)
{
    int32_t v = ((int32_t)w[0] << 16) | ((int32_t)w[1] << 8) | w[2];
    return (v & 0x800000) ? v - 0x1000000 : v;
}

/* One full frame. tx may carry a command and WREG data; rx gets everything. */
static esp_err_t xfer(const uint8_t *tx, uint8_t *rx)
{
    spi_transaction_t t = {
        .length = (size_t)frame_words() * WORD_BYTES * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    return spi_device_polling_transmit(s_dev, &t);
}

static esp_err_t command(uint16_t cmd, uint16_t *resp_of_previous)
{
    uint8_t tx[ADS_MAX_CH * 3 + 6] = { 0 }, rx[sizeof(tx)];
    put_word16(tx, cmd);
    esp_err_t err = xfer(tx, rx);
    if (err == ESP_OK && resp_of_previous) *resp_of_previous = get_word16(rx);
    return err;
}

esp_err_t ads_bus_init(void)
{
    const spi_bus_config_t bus = {
        .sclk_io_num = ADC_SCLK_GPIO,
        .mosi_io_num = ADC_MOSI_GPIO,
        .miso_io_num = ADC_MISO_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64,
    };
    esp_err_t err = spi_bus_initialize(ADC_SPI_HOST, &bus, SPI_DMA_DISABLED);
    if (err != ESP_OK) return err;

    const spi_device_interface_config_t dev = {
        .mode = 1,                      /* CPOL 0, CPHA 1 */
        .clock_speed_hz = ADC_SPI_HZ,
        .spics_io_num = ADC_CS_GPIO,
        .queue_size = 1,
        .cs_ena_pretrans = 1,
        .cs_ena_posttrans = 1,
    };
    err = spi_bus_add_device(ADC_SPI_HOST, &dev, &s_dev);
    if (err != ESP_OK) return err;

    const spi_device_interface_config_t ltc = {
        .mode = 0,
        .clock_speed_hz = LTC6903_SPI_HZ,
        .spics_io_num = LTC6903_CS_GPIO,
        .queue_size = 1,
    };
    err = spi_bus_add_device(ADC_SPI_HOST, &ltc, &s_ltc);
    if (err != ESP_OK) return err;

    /* SYNC/RESET: drive high (run) before switching to output. */
    gpio_set_level(ADC_SYNC_GPIO, 1);
    const gpio_config_t sync = {
        .pin_bit_mask = 1ULL << ADC_SYNC_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&sync);
}

esp_err_t ads_reset_and_probe(void)
{
    /* Low for more than 2048 CLKIN periods is a reset (256 us at 8 MHz).
     * 2 ms covers a slow LTC6903 setting too. */
    gpio_set_level(ADC_SYNC_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(ADC_SYNC_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(2));

    /* Assume four channels for the very first read: a longer frame is
     * harmless, the chip ignores extra words once CS rises. */
    s_info.nch = 4;
    uint16_t id = 0;
    esp_err_t err = ads_rreg(ADS_REG_ID, &id);
    if (err != ESP_OK) return err;
    uint8_t n = (uint8_t)((id >> 8) & 0x0F);
    s_info.id = id;
    if ((id & 0xF000) != 0x2000 || n < 1 || n > ADS_MAX_CH) {
        ESP_LOGE(TAG, "No ADS131M0x: ID read 0x%04X (check CLKIN, SPI wiring, 3.3 V)", id);
        s_info.nch = 2;
        return ESP_ERR_NOT_FOUND;
    }
    s_info.nch = n;
    ESP_LOGI(TAG, "ADS131M0%u found, ID 0x%04X", n, id);
    return ESP_OK;
}

void ads_sync_pulse(void)
{
    /* Between 1 and 2048 CLKIN periods is a sync, not a reset. 20 us is
     * well inside that window for any CLKIN from 1 to 8.4 MHz. */
    gpio_set_level(ADC_SYNC_GPIO, 0);
    esp_rom_delay_us(20);
    gpio_set_level(ADC_SYNC_GPIO, 1);
}

esp_err_t ads_rreg(uint8_t addr, uint16_t *val)
{
    esp_err_t err = command(ADS_CMD_RREG(addr, 1), NULL);
    if (err != ESP_OK) return err;
    return command(ADS_CMD_NULL, val);    /* single-register read: data is the response word */
}

esp_err_t ads_wreg(uint8_t addr, uint16_t val)
{
    uint8_t tx[ADS_MAX_CH * 3 + 6] = { 0 }, rx[sizeof(tx)];
    put_word16(tx, ADS_CMD_WREG(addr, 1));
    put_word16(tx + WORD_BYTES, val);
    esp_err_t err = xfer(tx, rx);
    if (err != ESP_OK) return err;

    uint16_t ack = 0;
    err = command(ADS_CMD_NULL, &ack);
    if (err != ESP_OK) return err;
    uint16_t want = (uint16_t)(0x4000 | ((addr & 0x3F) << 7));
    if (ack != want) {
        ESP_LOGW(TAG, "WREG 0x%02X: ack 0x%04X, expected 0x%04X", addr, ack, want);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

int ads_osr_code(uint16_t osr)
{
    switch (osr) {
        case 128:   return 0;
        case 256:   return 1;
        case 512:   return 2;
        case 1024:  return 3;
        case 2048:  return 4;
        case 4096:  return 5;
        case 8192:  return 6;
        case 16256: return 7;
        default:    return -1;
    }
}

static int pga_code(uint8_t g)
{
    for (int c = 0; c < 8; c++) if ((1u << c) == g) return c;
    return -1;
}

static uint16_t ch_cfg(int16_t phase, ads_mux_t mux)
{
    if (phase < -512) phase = -512;
    if (phase > 511) phase = 511;
    return (uint16_t)((((uint16_t)phase) & 0x3FF) << 6) | (uint16_t)mux;
}

static esp_err_t write_checked(uint8_t addr, uint16_t val)
{
    esp_err_t err = ads_wreg(addr, val);
    if (err != ESP_OK) return err;
    uint16_t rb = 0;
    err = ads_rreg(addr, &rb);
    if (err == ESP_OK && rb != val) {
        ESP_LOGW(TAG, "Reg 0x%02X wrote 0x%04X, read back 0x%04X", addr, val, rb);
        return ESP_ERR_INVALID_STATE;
    }
    return err;
}

esp_err_t ads_configure(uint16_t osr, const uint8_t *pga, const int16_t *phase,
                        const float *gcal, const int32_t *ocal, uint8_t nch)
{
    int oc = ads_osr_code(osr);
    if (oc < 0) return ESP_ERR_INVALID_ARG;
    if (nch > s_info.nch) nch = s_info.nch;

    esp_err_t err = write_checked(ADS_REG_MODE, ADS_MODE_RUN);
    if (err != ESP_OK) return err;

    uint16_t en = (uint16_t)(((1u << s_info.nch) - 1) << 8);
    uint16_t clock = (uint16_t)(en | ((unsigned)oc << 2) | 0x2);   /* PWR = high resolution */
    if ((err = write_checked(ADS_REG_CLOCK, clock)) != ESP_OK) return err;

    uint16_t gain = 0;
    for (uint8_t c = 0; c < nch && c < 4; c++) {
        int g = pga_code(pga[c]);
        if (g < 0) return ESP_ERR_INVALID_ARG;
        gain |= (uint16_t)(g << (4 * c));
    }
    if ((err = write_checked(ADS_REG_GAIN1, gain)) != ESP_OK) return err;

    for (uint8_t c = 0; c < nch; c++) {
        if ((err = write_checked(ADS_REG_CH_CFG(c), ch_cfg(phase[c], ADS_MUX_NORMAL))) != ESP_OK) return err;

        int32_t o = ocal[c];
        if (o > 0x7FFFFF) o = 0x7FFFFF;
        if (o < -0x800000) o = -0x800000;
        uint32_t ou = (uint32_t)o & 0xFFFFFF;
        if ((err = write_checked(ADS_REG_CH_OCAL_MSB(c), (uint16_t)(ou >> 8))) != ESP_OK) return err;
        if ((err = write_checked(ADS_REG_CH_OCAL_LSB(c), (uint16_t)((ou & 0xFF) << 8))) != ESP_OK) return err;

        float g = gcal[c];
        if (!(g >= 0.0f)) g = 1.0f;
        if (g > 1.999999f) g = 1.999999f;
        uint32_t gu = (uint32_t)lroundf(g * 8388608.0f) & 0xFFFFFF;   /* 1.0 = 0x800000 */
        if ((err = write_checked(ADS_REG_CH_GCAL_MSB(c), (uint16_t)(gu >> 8))) != ESP_OK) return err;
        if ((err = write_checked(ADS_REG_CH_GCAL_LSB(c), (uint16_t)((gu & 0xFF) << 8))) != ESP_OK) return err;
    }

    ESP_LOGI(TAG, "Configured: OSR %u, CLOCK 0x%04X, GAIN 0x%04X", osr, clock, gain);
    return ESP_OK;
}

esp_err_t ads_set_mux(uint8_t ch, ads_mux_t mux, int16_t phase)
{
    if (ch >= s_info.nch) return ESP_ERR_INVALID_ARG;
    return write_checked(ADS_REG_CH_CFG(ch), ch_cfg(phase, mux));
}

esp_err_t ads_read_frame(int32_t *ch)
{
    uint8_t tx[ADS_MAX_CH * 3 + 6] = { 0 }, rx[sizeof(tx)];
    esp_err_t err = xfer(tx, rx);
    if (err != ESP_OK) return err;

    int words = frame_words();
    s_info.status = get_word16(rx);
    for (int c = 0; c < s_info.nch; c++) ch[c] = get_sample(rx + WORD_BYTES * (1 + c));
    s_info.frames++;

    /* Output CRC covers every byte before the CRC word, pad bytes included. */
    uint16_t want = crc16_ccitt(rx, (size_t)(words - 1) * WORD_BYTES);
    uint16_t got = get_word16(rx + WORD_BYTES * (words - 1));
    if (want != got) {
        s_info.crc_errors++;
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

const ads_info_t *ads_info(void)
{
    return &s_info;
}

/* LTC6903: f = 2^OCT * 2078 / (2 - DAC/1024) Hz, OCT 0..15, DAC 0..1023.
 * Word = OCT[15:12] DAC[11:2] CNF[1:0]; CNF 10 = CLK on, /CLK off. */
esp_err_t ltc6903_set_hz(uint32_t hz, uint32_t *actual_hz)
{
    if (!s_ltc || hz < 1039 || hz > 68000000) return ESP_ERR_INVALID_ARG;
    int oct = (int)floor(log2((double)hz / 1039.0));
    if (oct < 0) oct = 0;
    if (oct > 15) oct = 15;
    long dac = lround(2048.0 - (2078.0 * pow(2.0, 10 + oct)) / (double)hz);
    if (dac < 0) dac = 0;
    if (dac > 1023) dac = 1023;
    uint16_t word = (uint16_t)((oct << 12) | ((unsigned)dac << 2) | 0x2);
    uint8_t tx[2] = { (uint8_t)(word >> 8), (uint8_t)word };
    spi_transaction_t t = { .length = 16, .tx_buffer = tx };
    esp_err_t err = spi_device_polling_transmit(s_ltc, &t);
    double f = pow(2.0, oct) * 2078.0 / (2.0 - (double)dac / 1024.0);
    if (actual_hz) *actual_hz = (uint32_t)lround(f);
    ESP_LOGI(TAG, "LTC6903: OCT %d DAC %ld -> %.0f Hz nominal (part tolerance about 1 %%)", oct, dac, f);
    return err;
}
