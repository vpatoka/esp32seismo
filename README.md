# vseismo

A two-channel geophone digitizer on an ESP32-C5. It takes the vertical and horizontal channels of a Webtronics (Larry Cochrane) 3-channel seismic amplifier and samples both at the same instant on a TI ADS131M02. The station is tuned from a web page on the device, and the firmware updates over WiFi.

```
L-15B Z ─┐                                  9.1k/1.0k + 47n   ADC 15 Click        ESP32-C5-DevKitC-1
L-15B H ─┴─ Webtronics EQAMP (±10 V out) ── divider + RC ───── ADS131M02 ── SPI ── N8R8
                                                               CLKIN 8.192 MHz     web UI, OTA, console
```

## Build

```
idf61                         # source ~/esp/esp-idf-v6.1.0/export.sh
idf.py set-target esp32c5     # once, or after deleting sdkconfig
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

- `sdkconfig.local` is layered over `sdkconfig.defaults`. After editing either one: `rm sdkconfig && idf.py build`.
- The partition table (8 MB, two 3 MB OTA slots) only changes over USB. After that, firmware goes up through the web page or `POST /api/fw/upload`.

## First run

1. Flash over USB. With no WiFi saved, the board opens the `VSEISMO-xxxx` access point and a captive portal.
2. Pick your network. 2.4 and 5 GHz both work. On a dual-band AP the station takes the stronger radio.
3. Open `http://<ip>/`. The trace starts drawing as soon as the ADC answers.
4. On the bench, switch each channel to "Shorted" and read the ADC floor. Then zero the offset and switch back.
5. Set the amplifier gain and geophone sensitivity per channel, and the readouts show ground velocity.

## Pins

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

These follow the esp32nixie pin audit. Check them against the DevKitC-1 pinout before the first power-up. GPIO 11/12 (UART0) are kept free for the WinSDR stream.

## Code

| File | Role |
|---|---|
| `main/vseismo.c` | Boot order, WiFi STA/AP with 2.4 + 5 GHz, status LED, BOOT button, OTA self-test |
| `main/ads131m0x.c` | ADS131M0x SPI driver, register setup, CRC check, LTC6903 programming |
| `main/acq.c` | DRDY ISR, acquisition task, clock source, FIR decimation, ring, statistics |
| `main/timebase.c` | SNTP, DS323x holdover, 1 Hz reference rate measurement |
| `main/web_server.c` | NVS config, settings by name, HTTP API, OTA writer, captive portal routes |
| `main/console.c` | USB Serial/JTAG command console |
| `main/captive_dns.c`, `event_log.c`, `ds323x.c` | Shared with esp32nixie |
| `main/www/index.html` | Tuning page (one file, no external resources) |
| `doc/API.md` | HTTP API contract |

## Status

The phase 1 firmware builds and hasn't been run on hardware yet. Phase 2 is the PSN-ADC24 serial protocol on UART0 for WinSDR, a minimum-phase FIR option, and GPS NMEA if PPS plus SNTP proves not enough.
