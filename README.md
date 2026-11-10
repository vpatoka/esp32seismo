# esp32seismo

**Two geophones, one instant: a Webtronics seismic amplifier digitized by an ADS131M02 on the ESP32-C5**

    ╔═══════════════════════════════════════════════════════════════════════╗
    ║                                                                       ║
    ║   ██╗   ██╗███████╗███████╗██╗███████╗███╗   ███╗ ██████╗             ║
    ║   ██║   ██║██╔════╝██╔════╝██║██╔════╝████╗ ████║██╔═══██╗  ADS131M02 ║
    ║   ██║   ██║███████╗█████╗  ██║███████╗██╔████╔██║██║   ██║  ESP32-C5  ║
    ║   ╚██╗ ██╔╝╚════██║██╔══╝  ██║╚════██║██║╚██╔╝██║██║   ██║  IDF v6.1  ║
    ║    ╚████╔╝ ███████║███████╗██║███████║██║ ╚═╝ ██║╚██████╔╝  2 x L-15B ║
    ║     ╚═══╝  ╚══════╝╚══════╝╚═╝╚══════╝╚═╝     ╚═╝ ╚═════╝             ║
    ║                                                                       ║
    ║                    ││ │ │          ││ │││  │ │    │                   ║
    ║   Z  ──────────────┼┴┬┼─┴─┬────────┼┼┬┼┴┼─┬┼─┴─┬──┴──┬────────────    ║
    ║                    │ ││   │        ││││ │ ││   │     │                ║
    ║                    │   │           ││││ ││││ │ │   │     │            ║
    ║   H  ──────────────┴─┬─┴───────────┼┼┼┼┬┼┼┴┼┬┼─┼─┬─┴──┬──┴────────    ║
    ║                      │             │││││││ │││ │ │    │               ║
    ║                    P               S                                  ║
    ╠═══════════════════════════════════════════════════════════════════════╣
    ║    L-15B  ──►  EQAMP  ──►  ADS131M02  ──►  ESP32-C5  ──►  web, OTA    ║
    ║                            de VE3VPT - Vlad                           ║
    ╚═══════════════════════════════════════════════════════════════════════╝

## Table of Contents

- [Introduction](#introduction)
- [Project Status](#project-status)
- [Features](#features)
- [Hardware](#hardware)
  - [Signal Chain](#signal-chain)
  - [Board and Pin Map](#board-and-pin-map)
  - [ADC 15 Click Changes](#adc-15-click-changes)
  - [Power](#power)
- [Timing](#timing)
  - [ADC Clock](#adc-clock)
  - [Time Labels and the 1 Hz Reference](#time-labels-and-the-1-hz-reference)
- [Firmware Architecture](#firmware-architecture)
- [Talking to the Station](#talking-to-the-station)
  - [First Boot](#first-boot)
  - [Web Page](#web-page)
  - [HTTP API and curl](#http-api-and-curl)
  - [USB Console](#usb-console)
  - [BOOT Button](#boot-button)
- [Faults](#faults)
- [Building and Flashing](#building-and-flashing)
- [Over-the-Air Updates](#over-the-air-updates)
- [Configuration](#configuration)
- [Repository Layout](#repository-layout)
- [Roadmap](#roadmap)
- [License and Author](#license-and-author)

---

## Introduction

A home seismic station built around Larry Cochrane's Webtronics 3-channel seismic amplifier, the board many Public Seismic Network stations ran with WinSDR and WinQuake. Two L-15B 4.5 Hz geophones, one vertical and one horizontal, feed two amplifier channels.

The original plan used an AD7177. Its channels share one converter behind a mux, so they're never sampled at the same instant. esp32seismo replaces it with a TI **ADS131M02** on a MikroE ADC 15 Click: one modulator per channel, both clocked together. An **ESP32-C5-DevKitC-1-N8R8** on **ESP-IDF v6.1** reads it, filters it down to 100 SPS, timestamps every sample in UTC and serves a tuning page over WiFi.

The infrastructure (WiFi on 2.4 and 5 GHz, captive portal, OTA with rollback, USB console, event log) comes from the esp32nixie project. The signal chain and the input network follow Larry's PSN-ADC24 digitizer.

## Project Status

| Phase | Content | State |
|---|---|---|
| 1 | ADS131M02 driver, acquisition, FIR decimation, timing, tuning page, portal, OTA, console | built, not yet on hardware |
| Bench | Pins, CLKIN modification, shorted-input floor, offset trim sign, CRC check | next |
| 2 | PSN-ADC24 serial protocol on UART0, so WinSDR sees a PSN board | |
| 3 | Minimum-phase FIR, GPS NMEA if needed, channel phase match, enclosure | |

## Features

- **Simultaneous sampling** of both channels on one ADS131M02, at 1000 SPS from an 8.192 MHz clock
- **Decimating FIR** down to 100 SPS: 201 taps, linear phase, integer math on the sample path
- **UTC timestamps** on every output sample, corrected for the filter delay
- **Three ADC clock paths**: external oscillator, crystal-derived LEDC, or the stock LTC6903
- **Rate measurement** against a GPS PPS or DS3231 1 Hz edge, in ppm
- **Tuning page**: live drum-recorder trace, per-channel readouts in counts, µV, mV and µm/s, a noise spectrum, settings, bench tests
- **Bench tests** without a soldering iron: shorted inputs, test levels, offset zero, filter sync
- **WiFi** on 2.4 and 5 GHz, IPv4 and IPv6, open setup AP with a captive portal
- **OTA**: upload a `.bin` from the browser or curl, header checks before erase, rollback if the new image fails its self-test
- **Four ways in**: web page, HTTP API, USB console, BOOT button

## Hardware

### Signal Chain

```
L-15B Z ─┐                                    9.1k / 1.0k 0.1 %    ADC 15 Click        ESP32-C5-DevKitC-1
L-15B H ─┴─ Webtronics EQAMP (±10 V out) ──── 47 nF C0G ────────── ADS131M02 ── SPI ── N8R8
                                                                   CLKIN 8.192 MHz     web, OTA, console
```

- **Amplifier:** LT1007 input stage, LF444 gain and filter stages, a 20 Hz multi-pole low-pass and a 0.05 Hz high-pass, ±10 V out. At gain ×1000 the analog chain lands near 105 to 110 dB, which sets the bar for the ADC.
- **Divider:** 9.1k over 1.0k gives 0.099, so ±10 V becomes ±0.99 V. A ±12 V rail clip still stays inside the ±1.2 V full scale.
- **RC:** 47 nF across the 1.0k puts a pole near 3.7 kHz. That's ample anti-aliasing for the ΔΣ modulator.

### Board and Pin Map

ESP32-C5-DevKitC-1-N8R8: single-core RISC-V, 8 MB flash, 8 MB PSRAM.

| Signal | GPIO | Click pin |
|---|---|---|
| SPI SCK / MOSI / MISO | 6 / 8 / 9 | SCK / SDI / SDO |
| ADC chip select | 10 | CS |
| DRDY | 1 | INT |
| SYNC/RESET | 0 | RST |
| LTC6903 select (stock clock only) | 24 | PWM |
| LEDC clock to CLKIN (optional) | 7 | CLKIN pad, after the trace cut |
| 1 Hz reference (GPS PPS or DS3231 SQW) | 23 | |
| I2C SDA / SCL (DS3231, optional) | 4 / 5 | |
| RGB LED / BOOT button | 27 / 28 | on board |

- Never touched: 13/14 (USB JTAG), 15 (PSRAM CS) and 16 to 22 (flash and PSRAM).
- GPIO 11/12 (UART0, the DevKit's second USB port) are kept for the WinSDR stream.
- The map follows the esp32nixie pin audit. Check it against the DevKitC-1 pinout before the first power-up.

### ADC 15 Click Changes

The stock Click wires its screw terminals straight to the ADC pins and clocks the ADC from an LTC6903 that's only good to about 1 %. For seismic use:

1. **Inputs:** fit the divider and RC above on each channel. 0.1 % resistors keep the two channels matched.
2. **Clock:** cut the LTC6903 to CLKIN trace and fit an 8.192 MHz 3.3 V CMOS oscillator or TCXO.
3. **Supply:** feed the Click from its own low-noise 3.3 V LDO with a ferrite, not from the DevKit 3V3 pin. The Click ties AVDD and DVDD together.

### Power

The [carrier board](hardware/carrier/README.md) holds the DevKit, the ADC 15 Click and a ZS-042 DS3231 on headers, plus the power, the analog front end and the ADC clock.

- **One adapter, 18 to 24 V DC, 0.5 A or more,** feeds the EQAMP. The carrier takes the EQAMP's J8: raw DC after its bridge rectifier and before its regulators, about 15 to 27 V. It goes through a fuse, a reverse diode and a TVS.
- **Never the EQAMP's J9.** That's the amplifier's own regulated ±12 V analog supply, and WiFi transmit bursts would land in its gain-1700 chain.
- A Murata switching module makes 5 V for the DevKit. A separate ferrite and LDO make a quiet 3.3 V just for the Click.
- Each EQAMP channel arrives as a signal and ground pair, and stays a pair down to its ADC input (pseudo-differential). That cancels the few mV across the J8 power return, so the extra ground link R30 stays unfitted unless hum shows up.

## Timing

### ADC Clock

| Source | Accuracy | Rate at OSR 4096 | `clk_src` |
|---|---|---|---|
| 8.192 MHz oscillator or TCXO | ±20 to 50 ppm, or ±0.5 to 2.5 ppm | 1000 SPS exactly | `ext` |
| ESP32 LEDC, 48 MHz XTAL ÷ 6 | ±10 to 20 ppm | 976.5625 SPS | `ledc` |
| On-board LTC6903 | about ±1.1 % plus drift | about 1000 SPS | `ltc6903` |

### Time Labels and the 1 Hz Reference

- **Wall clock:** SNTP sets it, hourly, from four servers. An optional DS3231 seeds it at boot and holds it between syncs.
- **Timestamps:** each output sample carries its DRDY time minus the FIR group delay, mapped to UTC.
- **Rate reference:** a 1 Hz edge on GPIO23 (GPS PPS or DS3231 SQW) measures the true sample rate in ppm, whatever drives CLKIN. With GPS, the PPS edge defines the second and SNTP only names it, so no NMEA parsing is needed.

## Firmware Architecture

C only, native IDF APIs, no Arduino, no managed components.

| Context | Priority | Job |
|---|---|---|
| WiFi driver | 23 | IDF |
| Acquisition | 20 | One SPI frame per DRDY, FIR, ring buffer, statistics |
| lwIP tcpip | 18 | IDF, SNTP callback |
| Timebase | 10 | 1 Hz rate loop, DS3231 (owns I2C) |
| httpd | default | API and page |
| Captive DNS, button, LED | 5, 4, 2 | Housekeeping |

- **One owner per peripheral:** acquisition owns SPI, timebase owns I2C.
- **ISRs only timestamp and notify.** A DRDY served late is counted as an overrun rather than passing silently.
- **Settings changes** restart acquisition in place, with no reboot.

## Talking to the Station

### First Boot

1. Flash over USB.
2. With no WiFi saved, the board opens the open network `VSEISMO-xxxx`, and the setup page pops up.
3. Pick a network. Both bands are fine, and on a dual-band AP the station takes the stronger radio.
4. Open `http://<ip>/`. The trace starts as soon as the ADC answers.

### Web Page

- A drum-recorder trace of both channels, plus readouts in counts, µV at the ADC, mV at the amplifier and µm/s of ground velocity.
- A noise spectrum, with panels for settings, bench tests, firmware updates and the log.
- The page is one file with no external resources, and it shows the curl hints even with scripts off.

### HTTP API and curl

The API is the contract; the page is one client of it. Full reference: [`doc/API.md`](doc/API.md).

```
curl http://vseismo/api/status
curl http://vseismo/api/acq
curl 'http://vseismo/api/wave?since=0&max=100'
curl -d osr=4096 -d decim=10 http://vseismo/api/config
curl -d cmd=mux -d ch=0 -d mode=short http://vseismo/api/acq/cmd
```

### USB Console

Open the USB Serial/JTAG port in any terminal and type `help`.

```
vseismo> status
vseismo> set ch0_amp 1000
vseismo> set ch1_name N
vseismo> sync
```

`status` shows the ADC, rates, per-channel statistics, time, WiFi and faults. Settings follow the same rules as the HTTP API.

### BOOT Button

| Hold | Action |
|---|---|
| 3 s | Reboot |
| 6 s | Forget WiFi, reboot into the setup portal |
| 10 s | Factory reset |

## Faults

| Class | Bits | Behaviour |
|---|---|---|
| A | Abnormal reset, I2C init, task init | Latched until reboot, solid red LED, blocks the OTA self-test |
| Process | ADC missing, RTC missing or lost power, I2C recovered, 1 Hz reference lost, CRC errors, overruns, clipping, no wall-clock time | Web page and log, clear when the cause clears |

A missing ADC is deliberately a process fault, so a bare DevKit can still take firmware updates.

## Building and Flashing

```
idf61                         # source ~/esp/esp-idf-v6.1.0/export.sh
idf.py set-target esp32c5     # once, or after deleting sdkconfig
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

`sdkconfig.local` is layered over `sdkconfig.defaults`. After editing either one: `rm sdkconfig && idf.py build`.

## Over-the-Air Updates

```
curl --data-binary @build/vseismo.bin http://vseismo/api/fw/upload
```

The page has the same upload under "Network and firmware".

- **Checks before erasing:** the chip ID, the app descriptor and the project name.
- **Probation:** the new image boots on probation and must reach the network with the web server up within 300 s, or the bootloader rolls back.
- **USB once:** the partition table (two 3 MB slots on 8 MB flash) only changes over USB.

## Configuration

Settings live in one versioned NVS blob and can be exported and imported as plain text:

```
curl http://vseismo/api/config/export > vseismo.cfg
curl --data-binary @vseismo.cfg http://vseismo/api/config/import
```

| Group | Keys |
|---|---|
| Acquisition | `clk_src`, `clk_hz`, `osr`, `decim`, `taps` |
| Per channel (N = 0, 1) | `chN_name`, `chN_pga`, `chN_phase`, `chN_gcal`, `chN_ocal`, `chN_div`, `chN_amp`, `chN_sens` |
| Timing | `ref`, `ref_edge`, `rtc` |
| Station | `station`, `network` |
| Network | `hostname`, `port`, `login`, `user`, `pass` |

## Repository Layout

| Path | Role |
|---|---|
| `main/vseismo.c` | Boot order, WiFi STA/AP, status LED, BOOT button, OTA self-test |
| `main/ads131m0x.c` | ADS131M0x driver, CRC check, LTC6903 programming |
| `main/acq.c` | DRDY ISR, acquisition task, clock paths, FIR, ring, statistics |
| `main/timebase.c` | SNTP, DS3231 holdover, 1 Hz rate loop |
| `main/web_server.c` | Config, HTTP API, OTA writer, captive portal routes |
| `main/console.c` | USB console |
| `main/captive_dns.c`, `event_log.c`, `ds323x.c` | Shared with esp32nixie |
| `main/io_map.h` | Pins, priorities, constants |
| `main/www/` | Tuning page and portal page |
| `doc/API.md` | HTTP API contract |
| `partitions.csv` | Dual-slot OTA layout |
| `hardware/carrier/` | Carrier board rev A: generator, schematic and placement drawings, KiCad netlist |

## Roadmap

- **Bench bring-up:** pins, CLKIN modification, noise floor with shorted inputs, offset trim sign, CRC coverage
- **WinSDR:** the PSN-ADC24 packet format on UART0
- **Filters:** a minimum-phase FIR option for sharper P-wave onsets
- **Phase match:** between the two amplifier channels, measured with one test tone into both
- **Timing:** GPS NMEA, only if PPS plus SNTP proves not enough

## License and Author

MIT. See the SPDX headers in the sources.

**Author:** Vlad Patoka, VE3VPT

The amplifier and the PSN-ADC24 design are Larry Cochrane's (Webtronics). Their schematics and firmware are on his GitHub.

    ╔═══════════════════════════════════════════════════════════════════════╗
    ║                                                                       ║
    ║                              73 de VE3VPT                             ║
    ║                                                                       ║
    ║                  "Larry's amplifier, TI's converter,                  ║
    ║                             a 2026 brain"                             ║
    ║                                                                       ║
    ╚═══════════════════════════════════════════════════════════════════════╝
