/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Vlad Patoka (VE3VPT) */
/**
 * @file io_map.h
 * @brief Pin map and hardware constants for vseismo (ESP32-C5-DevKitC-1-N8R8).
 *
 * Signal chain: L-15B geophones -> Webtronics 3-channel EQAMP (two channels,
 * +-10 V out) -> 9.1k/1.0k 0.1 % divider + 47 nF C0G per channel -> MikroE
 * ADC 15 Click (TI ADS131M02) -> this board over SPI.
 *
 * Pins we never touch (same rules as esp32nixie):
 *   GPIO13/14   USB Serial/JTAG (console, flashing)
 *   GPIO15      SPICS1, wired to the PSRAM on the N8R8 module
 *   GPIO16-22   in-package flash / PSRAM
 *   GPIO11/12   UART0 on the DevKit's USB-to-UART port, left free (data
 *               leaves over WiFi through /api/wave)
 *
 * Strapping pins (union of DevKitC-1 v1.0 and v1.2): 2, 3, 7, 25, 26, 27,
 * 28. GPIO7 carries the optional LEDC clock to ADS131M02 CLKIN. CLKIN is a
 * high-impedance input and LEDC starts after boot, so the strap is not
 * disturbed. 27 (RGB LED) and 28 (BOOT) are the board's own.
 *
 * ADC 15 Click pull-ups (4.7k to 3.3 V): RST (SYNC/RESET), CS1 (ADC CS),
 * CS2 (LTC6903 SEN) and INT (DRDY). Every one of them idles high with the
 * ESP in reset, so the ADC is never held in reset by a floating pin.
 *
 * ADC clock options (cfg clk_src):
 *   ext      8.192 MHz CMOS oscillator or TCXO on CLKIN, LTC6903 output trace
 *            cut. Exactly 1000 SPS at OSR 4096. Recommended.
 *   ledc     GPIO7 drives CLKIN from the crystal-derived XTAL clock with an
 *            integer divider (48 MHz / 6 = 8.000 MHz, 976.5625 SPS at OSR 4096).
 *            Needs the same trace cut and a wire to GPIO7.
 *   ltc6903  the stock Click: the on-board LTC6903 is programmed over SPI
 *            (CS2 on GPIO24). About 1 % frequency error and temperature drift;
 *            usable only with a 1 Hz reference measuring the real rate.
 *
 * Board pins to verify on the bench against the DevKitC-1 v1.2 pinout before
 * the first power-up. These are proposals carried over from the esp32nixie
 * pin audit, not measurements.
 */

#ifndef IO_MAP_H
#define IO_MAP_H

#include "driver/gpio.h"

/* ── ADS131M02 on the ADC 15 Click (SPI2, mode 1) ───────────────────────── */
#define ADC_SPI_HOST            SPI2_HOST
#define ADC_SCLK_GPIO           GPIO_NUM_6
#define ADC_MOSI_GPIO           GPIO_NUM_8
#define ADC_MISO_GPIO           GPIO_NUM_9
#define ADC_CS_GPIO             GPIO_NUM_10   /* Click CS  (CS1) */
#define ADC_DRDY_GPIO           GPIO_NUM_1    /* Click INT (DRDY, active low) */
#define ADC_SYNC_GPIO           GPIO_NUM_0    /* Click RST (SYNC/RESET, active low) */
#define ADC_SPI_HZ              (8 * 1000 * 1000)   /* datasheet max 25 MHz; 8 MHz is
                                                        plenty and kinder to jumper wires */

/* ── ADC clock ─────────────────────────────────────────────────────────── */
#define ADC_CLKIN_LEDC_GPIO     GPIO_NUM_7    /* optional LEDC clock to CLKIN */
#define LTC6903_CS_GPIO         GPIO_NUM_24   /* Click PWM (CS2, LTC6903 SEN) */
#define LTC6903_SPI_HZ          (1 * 1000 * 1000)

/* ── 1 Hz timing reference: GPS PPS or DS3231 SQW (open drain) ────────── */
#define REF_1HZ_GPIO            GPIO_NUM_23   /* internal pull-up on; add 4.7k for SQW */

/* ── I2C bus: optional DS3231 / DS3231M / DS3232 holdover RTC ──────────── */
#define I2C_SDA_GPIO            GPIO_NUM_4
#define I2C_SCL_GPIO            GPIO_NUM_5
#define I2C_BUS_HZ              100000
#define I2C_XFER_TIMEOUT_MS     20            /* bounded: a stuck bus fails fast */
#define DS323X_I2C_ADDR         0x68          /* 7-bit */

/* ── On-board ──────────────────────────────────────────────────────────── */
#define STATUS_LED_GPIO         GPIO_NUM_27   /* addressable RGB LED (WS2812 type) */
#define BUTTON_GPIO             GPIO_NUM_28   /* BOOT button, active low */
#define STATUS_LED_BRIGHTNESS   24            /* 0-255 per channel, kept dim */

/* ── Button hold tiers (webcontrol convention) ─────────────────────────── */
#define BUTTON_HOLD_REBOOT_MS   3000
#define BUTTON_HOLD_WIFI_MS     6000
#define BUTTON_HOLD_FACTORY_MS  10000
#define BUTTON_SAMPLE_MS        50

/* ── WiFi STA reconnect backoff ────────────────────────────────────────── */
#define WIFI_RETRY_INITIAL_MS   5000
#define WIFI_RETRY_MAX_MS       60000
#define WIFI_DHCP_TIMEOUT_MS    30000         /* associated but no lease: drop and retry */

/* ── Provisioning AP ───────────────────────────────────────────────────── */
#define WIFI_AP_IP              "192.168.4.1"
#define WIFI_AP_GATEWAY         "192.168.4.1"
#define WIFI_AP_NETMASK         "255.255.255.0"
#define WIFI_AP_CHANNEL         1             /* 2.4 GHz: every phone can see it */
#define WIFI_AP_MAX_CONN        4

/* ── Task priorities (WiFi runs at 23, esp_timer at 22, lwIP tcpip at 18) ─
 * Acquisition sits above the WiFi driver. The ADC FIFO holds two
 * conversions, 2 ms at 1000 SPS, and a WiFi scan, connect or block-ack
 * setup can hold the single core longer than that. The task needs about
 * 30 to 60 us per input period and never blocks on I/O (its log lines go
 * out through the timebase task), so WiFi loses at most a few percent. */
#define PRIO_ACQ                24            /* configMAX_PRIORITIES - 1 */
#define PRIO_TIMEBASE           10
#define PRIO_CAPTIVE_DNS        5
#define PRIO_BUTTON             4
#define PRIO_STATUS_LED         2

#endif /* IO_MAP_H */
