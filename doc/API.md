# vseismo HTTP API

API version 1. Plain HTTP/1.1, no TLS on the device. Reads are `GET` and return JSON. Writes are `POST` with a form-urlencoded body (`curl -d`).

The API is the product. The web page is one client of it, and it gets no private endpoints. Changes are additive only: fields and endpoints are never renamed or removed, and units never change. A breaking change would need `SEISMO_API_VERSION` 2.

- With `login=1`, every endpoint except the portal ones needs HTTP Basic Auth.
- A browser POST must carry an `Origin` that matches `Host`. curl sends no `Origin`, so it passes.

## Endpoints

| Method | Path | Purpose |
|---|---|---|
| GET | `/` | Tuning page |
| GET | `/api/status` | Firmware, time, 1 Hz reference, RTC, WiFi, faults, OTA state |
| GET | `/api/acq` | Acquisition state and per-channel statistics |
| GET | `/api/wave?since=SEQ&max=N` | Output samples newer than `SEQ` (N up to 1000, default 400) |
| POST | `/api/acq/cmd` | `cmd=sync`, `cmd=restart`, `cmd=mux&ch=N&mode=...`, `cmd=zero&ch=N` |
| GET / POST | `/api/config` | Read or change settings |
| GET | `/api/config/export` | Settings as `key=value` text |
| POST | `/api/config/import` | Apply a settings file (all or nothing) |
| GET | `/api/logs` | Recent console lines, JSON array |
| GET / POST | `/api/loglevel` | Show levels, or set `tag=ACQ&level=debug` until reboot |
| POST | `/api/reboot` | Reboot after the reply |
| GET | `/api/fw/info` | Running and next OTA slot |
| POST | `/api/fw/upload` | Raw `vseismo.bin` body, written to the inactive slot |
| GET | `/portal`, `/api/wifi/scan`, `/api/wifi/status` | WiFi setup |
| POST | `/api/wifi/connect`, `/api/wifi/forget` | Save or clear WiFi credentials |

## /api/wave

```
curl 'http://vseismo/api/wave?since=0&max=5'
{"ok":true,"next":1234,"seq0":1229,"n":5,"fs":100.0000,"t0":1794240527736,
 "ch0":[12,-3,8,1,-6],"ch1":[40,38,41,44,39],"flags":[0,0,0,1,1]}
```

- Values are ADC counts after the decimating FIR, 24-bit scale.
- `t0` is the UTC time of the first sample in ms. It's 0 until the station has wall-clock time.
- Timestamps are corrected for the full group delay (ADC sinc3, 1.5 input periods, plus the FIR, (taps − 1) / 2 input periods), so they mark when the ground moved.
- `flags` has one entry per sample. A bit is set when the FIR window of that sample held an input of that kind. A flagged sample is still on the correct time grid, but its value is not a clean measurement.

| Bit | Value | Meaning |
|---|---|---|
| 0 | 1 | Gap: an input sample was lost (read late) and replaced by the last good one |
| 1 | 2 | CRC: an input frame failed its CRC and was replaced by the last good one |
| 2 | 4 | Unsettled: inputs held while the ADC filter settled after a SYNC |
| 3 | 8 | Clip: an input sample was within 16 counts of full scale |

- After a restart, output starts only once the FIR history holds settled data (taps + 3 input samples), so there's no ramp up from zero.
- Poll again with `since=<next>`. If `seq0` isn't the `next` you asked for, samples rolled out of the 4096-sample ring and there's a gap.

## /api/acq counters

| Field | Unit | Meaning |
|---|---|---|
| `delay_ms` | ms | Total group delay subtracted from timestamps: ADC sinc3 plus FIR |
| `n_in` | input samples | Input periods since the last restart, lost ones included |
| `crc_errors` | frames | Frames whose output CRC did not match |
| `overruns` | input samples | Samples lost because the read came too late, held at the last good value |
| `stale_frames` | frames | Frames dropped because they were older than the DRDY count allows (FIFO backlog). Should stay 0. |
| `status_events` | events | The ADC reset itself (brownout) or resynced without a SYNC from the station |
| `restarts` | | Acquisition (re)starts since boot |

## /api/acq channel fields

| Field | Unit | Meaning |
|---|---|---|
| `mean`, `rms`, `min`, `max` | counts | Last full one-second window of output samples. `rms` is AC (taken about the mean). |
| `clips` | input samples | Samples within 16 counts of full scale in that window |
| `lsb_uv` | µV | One count at the ADC pin: 1.2 V / PGA / 2^23 |
| `rms_uv` | µV | `rms` at the ADC pin |
| `amp_mv` | mV | At the EQAMP output: `rms_uv / div` |
| `vel_ums` | µm/s | Ground velocity in the passband: `amp_mv / amp / sens`. `null` until both `amp` and `sens` are set. |
| `dbfs` | dB | Sine-equivalent level relative to full scale |

## Settings keys

| Key | Values | Effect |
|---|---|---|
| `hostname`, `port` | | Reboot |
| `login`, `user`, `pass` | | Immediate. An empty `user` or `pass` keeps the current value. |
| `station`, `network` | A-Z 0-9, 1-5 and 1-2 characters | Immediate |
| `clk_src` | `ext`, `ledc`, `ltc6903` | Restarts acquisition |
| `clk_hz` | 1000000..8400000 | Restarts acquisition |
| `osr` | 512, 1024, 2048, 4096, 8192, 16256 | Restarts acquisition |
| `decim` | 1..18 | Restarts acquisition |
| `taps` | 0 (auto = 20 x decim + 1) or odd 3..255 | Restarts acquisition |
| `ref` | `none`, `gps`, `rtc` | Reboot |
| `ref_edge` | `rise`, `fall` | Reboot |
| `rtc` | 0, 1 | Reboot |
| `chN_name` | 1-7 characters | Immediate |
| `chN_pga` | 1, 2, 4 .. 128 | Restarts acquisition |
| `chN_phase` | -512..511 modulator clocks | Restarts acquisition |
| `chN_gcal` | 0.5..1.5 | Restarts acquisition |
| `chN_ocal` | ±8388607 counts | Restarts acquisition |
| `chN_div` | 0.001..1 | Immediate (scaling only) |
| `chN_amp` | 0 (unknown) .. 100000 | Immediate (scaling only) |
| `chN_sens` | 0 (unknown) .. 10000 V/(m/s) | Immediate (scaling only) |
| `wifi_ssid`, `wifi_pass` | | Import and console only. Reboot. |

`osr` 128 and 256 (16 and 32 kSPS) and `decim` above 18 used to be accepted. Neither worked: one task wake per sample can't keep up above 8 kSPS, and the 255-tap FIR stops rejecting aliases past decimation 18. A stored config with those values falls back to OSR 4096 or decim 10 at boot.

`N` is 0 or 1. A POST to `/api/config` applies every known key in the body. One bad value rejects the whole request and changes nothing. The reply says whether acquisition restarted and whether a reboot is needed.

## Bench commands

```
curl -d cmd=mux -d ch=0 -d mode=short http://vseismo/api/acq/cmd   # ADC noise floor on CH0
curl -d cmd=zero -d ch=0 http://vseismo/api/acq/cmd                # fold the mean into OCAL, save
curl -d cmd=mux -d ch=0 -d mode=normal http://vseismo/api/acq/cmd
curl -d cmd=sync http://vseismo/api/acq/cmd
```

`mux` isn't saved. `zero` assumes the chip subtracts OCAL from each result, as the ADS131M02 datasheet describes. Confirm the sign once on the bench: after `zero`, the shorted-input mean should land near 0, not double.

## Backup and restore

```
curl http://vseismo/api/config/export > vseismo.cfg
curl --data-binary @vseismo.cfg http://vseismo/api/config/import
```

Use `--data-binary`, since plain `-d` strips the newlines. Passwords aren't exported.

## Firmware

```
curl --data-binary @build/vseismo.bin http://vseismo/api/fw/upload
```

The image is checked (chip ID, app descriptor, project name `vseismo`) before anything is erased. The new image boots on probation. It must reach the network with the web server up and no Class-A fault within `CONFIG_SEISMO_OTA_VERIFY_S` (300 s), or the bootloader rolls back.
