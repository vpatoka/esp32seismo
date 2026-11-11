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
  - [Wall Clock](#wall-clock)
  - [Rate Measurement](#rate-measurement)
  - [Sample Timestamps](#sample-timestamps)
  - [Edge Timing](#edge-timing)
  - [Bench Checks for Timing](#bench-checks-for-timing)
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
| 1b | Timing: PPS rate fit, sample-count timestamps, slewed SNTP clock, MCPWM edge capture (opt-in), PSRAM ring | built, host-simulated, not yet on hardware |
| Bench | Pins, CLKIN modification, shorted-input floor, offset trim sign, CRC check, [timing checks](#bench-checks-for-timing) | next |
| 2 | PSN-ADC24 serial protocol on UART0, so WinSDR sees a PSN board | |
| 3 | Minimum-phase FIR, GPS NMEA if needed, channel phase match, enclosure | |

## Features

- **Simultaneous sampling** of both channels on one ADS131M02, at 1000 SPS from an 8.192 MHz clock
- **Decimating FIR** down to 100 SPS: 201 taps, linear phase, integer math on the sample path
- **UTC timestamps** on every output sample, corrected for the ADC and FIR delay: from the sample count against GPS PPS (µs-grade), or from an SNTP clock that is slewed, never stepped. Each sample says which.
- **Every sample period counted**: a sample read late or failing its CRC is held at the last good value and flagged, so the time grid never slips
- **Three ADC clock paths**: external oscillator, crystal-derived LEDC, or the stock LTC6903
- **Rate measurement** against a GPS PPS or DS3231 1 Hz edge: a least-squares fit over 300 edges, to about 0.01 ppm, with its own uncertainty
- **Hardware edge capture** (opt-in): DRDY and the 1 Hz edge latched by an MCPWM timer, free of interrupt jitter
- **22 minutes of history** in PSRAM, with catch-up reads for a recorder that fell behind
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
- DS3231 SQW: on the common breakout modules SQW sits right next to 32K. Wired to the wrong pin, the log says `No SQW edge on GPIO23 5 s after start`. `ref=rtc` drives the DS3231 even with `rtc=0`, since its SQW only runs once configured. The DS3231's seconds roll over on the falling SQW edge; the rate fit works on either edge, so `ref_edge` only matters when comparing phases on a scope.
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

### Wall Clock

SNTP sets the system clock hourly, in immediate mode, so log times are exact. An optional DS3231 seeds it at boot and holds it between syncs. Sample timestamps don't follow the system clock's steps; see below.

The first server comes from DHCP option 42, so a LAN time server is used when the network offers one; the four pool servers stay behind it as fallback. lwIP is built with round-trip compensation and reply checking. On pfSense: serve NTP on LAN (Services > NTP), then set NTP Server 1 to the router's LAN address (Services > DHCP Server > LAN). The boot log then shows `SNTP started: DHCP 10.10.10.1, then 0.pool.ntp.org, ...`; without option 42 it says `DHCP none` and the first sync comes about 15 s later. After a reconnect the clock polls right away.

Every SNTP reply is checked before it touches the clock. Times outside build date minus a day to 2106 are rejected. Once synced, a reply that would move the clock by more than 200 ms plus 50 ppm drift since the last sync is held, re-polled after 30 s, and applied only if the next one agrees within 100 ms. On a congested uplink one reply can sit in a queue for most of a second; without the check it would shift the timestamps and rename the PPS seconds. `time.ntp_rejected` in `/api/status` counts what was held or rejected.

The DS3231 is written at a second boundary only when it needs it: the first SNTP sync after boot, after its oscillator-stop flag, and once a day. Each write restarts its countdown and moves the SQW phase, so with the SQW as the rate reference the fit restarts once after a write.

### Rate Measurement

A 1 Hz edge on GPIO23 (GPS PPS or DS3231 SQW) is compared against the ADC sample count. At each edge the firmware interpolates the fractional sample position, then fits a straight line through the last 300 positions. The slope is the true sample rate, whatever drives CLKIN; the same fit gives the ESP32 crystal error.

- **Why a fit:** averaging second-to-second counts turns timing jitter straight into rate noise (about 1.4 ppm at 10 µs of interrupt jitter). The fitted slope has error σ·√(12/N³), about 0.01 ppm from the same edges.
- **Robust:** edges at the wrong spacing are ignored, edges far off the line are dropped as outliers, and the fit restarts after a gap over 30 s, five outliers in a row, or an acquisition restart.
- **Reported:** `/api/status` `ref` gives the rate, its ppm error, the fit's own 1σ, the edge count and the rejects.

### Sample Timestamps

An output sample stands for an input-sample position: the count at its newest input, minus the ADC sinc³ delay (1.5 periods) and the FIR delay ((taps − 1) / 2). That position is turned into UTC by one of two models. Every sample records which one, as its time quality.

| Quality | When | How | Grade |
|---|---|---|---|
| `pps` | GPS PPS and a valid rate fit | Position on the fitted line through the PPS edges. The PPS edge defines the second, SNTP only names it (to the nearest second). | µs |
| `ntp` | SNTP, no usable PPS | DRDY time through a sample clock that follows the system clock but changes rate by at most 500 ppm | ms, never steps |
| `rtc` | DS3231 seed, no SNTP yet | Same, from the RTC-seeded clock, set at the RTC's seconds rollover | about 1 ms at boot, then RTC drift (2 ppm, 0.17 s a day) |
| `none` | No wall-clock time | Samples carry no UTC | |

- **No steps:** an SNTP correction of 10 ms is slewed into the sample clock over 20 s. Only a change over 0.5 s (a leap second, a bad RTC seed) is applied at once.
- **Holdover:** the `pps` model holds for 30 s after the last good edge. Meanwhile the sample clock is steered to PPS time, with the crystal drift fed forward, so losing PPS hands over to `ntp` without a jump.
- **DS3231 SQW** improves the rate only. Its edges aren't UTC seconds, so it never gives `pps`.
- **No NMEA needed:** with GPS, SNTP names the second the PPS edge starts.

Host simulation, with the real timing code, a 23.4 ppm fast ADC clock, a 12 ppm fast ESP crystal, 10 µs of interrupt jitter and ±3 ms SNTP steps:

| Case | Result |
|---|---|
| GPS PPS | 1.7 µs rms from true time |
| PPS lost, 30 s holdover | within 1.3 µs |
| Handover to SNTP after holdover | no jump: 525 ppm worst between neighbouring samples |
| No PPS, an hour of SNTP steps | SNTP-grade (ms), never a step |

### Edge Timing

`capture` chooses how DRDY and the 1 Hz edge get their time. It takes a reboot.

| `capture` | How | Timing noise |
|---|---|---|
| `isr` (default) | A GPIO interrupt reads esp_timer | about 10 µs under WiFi load |
| `mcpwm` | One MCPWM capture timer latches both edges in hardware at 6.25 ns, kept as µs | 0.3 µs rms (the µs rounding) |

- With `mcpwm`, simulated rate-fit error drops from 0.014 to 0.0003 ppm. Interrupt latency spikes under WiFi load no longer reach the timestamps.
- **Self-checking:** each latched edge must land just before its interrupt. One that doesn't is counted in `/api/acq` `cap_bad` and timed from the interrupt instead, so a faulty capture path falls back to `isr` accuracy instead of giving wrong times. `drdy_rate` checks the edge count against the nominal rate once a second, on both paths.
- If the capture timer can't be set up, the station falls back to `isr` and says so in `/api/acq` `capture`.
- `mcpwm` stays opt-in until it has passed the bench checks below.

### Bench Checks for Timing

1. Boot log: PSRAM found and tested, `Output ring: 131072 samples (3072 KB) in PSRAM`.
2. ADC: STATUS DRDY bits set on new data, `stale_frames` and `overruns` at 0 in steady state, `drdy_rate` within a few tenths of a percent of `fs_in`.
3. With GPS PPS and `capture=isr`: `time.quality` reaches `pps` after 10 edges. Note `ref.rate_sigma_ppm`.
4. With `capture=mcpwm`: the boot log confirms both capture channels, `cap_bad` and `drdy_rate_bad` stay at 0, `overruns` stays flat across a settings save, and `rate_sigma_ppm` drops by more than ten times. Then `mcpwm` can become the default.

## Firmware Architecture

C only, native IDF APIs, no Arduino, no managed components.

| Context | Priority | Job |
|---|---|---|
| WiFi driver | 23 | IDF |
| Acquisition | 20 | DRDY accounting, SPI frames, FIR, output ring, statistics |
| lwIP tcpip | 18 | IDF, SNTP callback |
| Timebase | 10 | 1 Hz rate fit, PPS timestamp model, sample-clock slew, DS3231 (owns I2C) |
| httpd | default | API and page |
| Captive DNS, button, LED | 5, 4, 2 | Housekeeping |

- **One owner per peripheral:** acquisition owns SPI, timebase owns I2C.
- **ISRs only timestamp, count and notify,** from IRAM, and keep running through flash writes. Every DRDY edge is counted, so a sample read late or failing its CRC is held and flagged rather than dropped, and the sample count stays on the true grid.
- **Lock-free output ring** in PSRAM (131072 samples, 4096 in internal RAM without PSRAM): the acquisition task never waits on an HTTP reader.
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
curl 'http://vseismo/api/wave?since=118000&max=1000&order=oldest'   # catch up, see "pending"
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
| Process | ADC missing, RTC missing or lost power, I2C recovered, 1 Hz reference lost, CRC errors, overruns, clipping, no wall-clock time, ADC reset or resynced on its own | Web page and log, clear when the cause clears |

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
| Timing | `ref`, `ref_edge`, `rtc`, `capture` |
| Station | `station`, `network` |
| Network | `hostname`, `port`, `login`, `user`, `pass` |

## Repository Layout

| Path | Role |
|---|---|
| `main/vseismo.c` | Boot order, WiFi STA/AP, status LED, BOOT button, OTA self-test |
| `main/ads131m0x.c` | ADS131M0x driver, CRC check, LTC6903 programming |
| `main/acq.c` | DRDY interrupt and MCPWM capture, acquisition task, clock paths, FIR, output ring, statistics |
| `main/timebase.c` | SNTP, DS3231 holdover, 1 Hz rate fit, PPS timestamp model, slewed sample clock |
| `main/web_server.c` | Config, HTTP API, OTA writer, captive portal routes |
| `main/console.c` | USB console |
| `main/captive_dns.c`, `event_log.c`, `ds323x.c` | Shared with esp32nixie |
| `main/io_map.h` | Pins, priorities, constants |
| `main/www/` | Tuning page and portal page |
| `doc/API.md` | HTTP API contract |
| `partitions.csv` | Dual-slot OTA layout |
| `hardware/carrier/` | Carrier board rev A: generator, schematic and placement drawings, KiCad netlist |

## Roadmap

- **Bench bring-up:** pins, CLKIN modification, noise floor with shorted inputs, offset trim sign, CRC coverage, the [timing checks](#bench-checks-for-timing)
- **Edge capture:** make `capture=mcpwm` the default once it passes the bench
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
