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
| GET | `/api/wave?since=SEQ&max=N&order=newest\|oldest` | Output samples from `SEQ` on (N up to 1000, default 400; order default newest) |
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

## /api/status: time

```
curl -s http://vseismo/api/status | jq .time
{"valid":true,"src":"ntp","ntp_syncs":12,"ntp_age":1834,"ntp_rejected":0,
 "quality":"pps","slew_ms":0.004,"pps_naming_errors":0}
```

| Field | Meaning |
|---|---|
| `valid`, `src` | The system clock has wall-clock time, and from where: `ntp`, `rtc` (DS323x seed, no SNTP yet) or `none` |
| `ntp_syncs`, `ntp_age` | SNTP syncs since boot, seconds since the last one |
| `ntp_rejected` | SNTP samples not applied since boot: outside the plausible window (build date minus a day to 2106), or held because they would move a synced clock by more than 200 ms plus 50 ppm drift since the last sync. A held sample is re-polled after 30 s and applied only when the next one agrees within 100 ms. |
| `quality` | How a sample timestamped now gets its time, best first: `pps`, `ntp`, `rtc`, `none`. See below. |
| `slew_ms` | Sample clock behind (positive) or ahead of its target, in ms, still being slewed in |
| `pps_naming_errors` | Times the UTC second assigned to the PPS edges had to be moved. Expect 1 at a leap second, otherwise 0. |

Sample timestamps come from one of two models.

- `pps`: GPS PPS present and the rate fit valid (`ref.rate_valid`). A sample's time is computed from its position in the ADC sample count, on the line fitted through the PPS edges. The PPS edge defines the second; SNTP only names it, rounded to the nearest second, so SNTP error, SNTP steps and interrupt jitter stay out of the timestamps. It stays `pps` for 30 s after the last good edge (holdover).
- `ntp` and `rtc`: no usable PPS. A sample's time is its DRDY time through a sample clock that follows the system clock but changes rate by at most 500 ppm. SNTP still steps the system clock every hour (log times stay exact), but sample timestamps never step: a 10 ms correction takes 20 s to slew in. A change over 0.5 s (a leap second, a first SNTP sync after a bad RTC seed) is applied at once. Once synced, a single sample that would move the clock by more than 200 ms (plus drift) is not applied until a second one agrees (see `ntp_rejected`), so one late reply can't shift the timestamps. While `pps` holds, the sample clock is steered to PPS time instead, so losing PPS hands over without a jump.

A DS323x SQW reference (`ref.type` `rtc`) improves the rate measurement only. Its edges are not UTC seconds, so it never gives `pps` quality.

## /api/status: 1 Hz reference

```
curl -s http://vseismo/api/status | jq .ref
{"type":"gps","edge":"rise","seen":true,"edges":1834,"age_ms":412,
 "rate_valid":true,"fs":1000.0234,"adc_ppm":23.412,"esp_ppm":11.991,
 "rate_sigma_ppm":0.0141,"rate_points":300,"rate_rejects":1}
```

The rate is the slope of a least-squares line through the ADC sample position at each reference edge, over the last 300 edges. It is not an average of second-to-second counts, which would be about 100 times noisier for the same edges.

| Field | Unit | Meaning |
|---|---|---|
| `type`, `edge` | | `none`, `gps` or `rtc`; `rise` or `fall` |
| `seen`, `edges`, `age_ms` | | Any edge since boot, edge count, time since the last one |
| `rate_valid` | | The fit holds at least 10 edges. `false` for 10 s after an acquisition restart, a reference gap over 30 s, or 5 outliers in a row. |
| `fs` | SPS | Measured ADC input rate |
| `adc_ppm` | ppm | Measured rate against nominal: the CLKIN error |
| `esp_ppm` | ppm | ESP32 timer against the reference: the board crystal error |
| `rate_sigma_ppm` | ppm | 1σ uncertainty of `adc_ppm` from the fit residuals. Shrinks as N^-1.5 while the window fills, then holds. Rising with a full window means timing jitter went up (WiFi load, a noisy PPS). |
| `rate_points` | edges | Edges in the fit window, up to 300 |
| `rate_rejects` | edges | Edges since boot that were a whole second apart but far off the fit (over 0.5 samples or 200 µs) and were dropped. An occasional one is a bad timestamp; a steady climb points at the reference or the wiring. |

`fs`, `adc_ppm`, `esp_ppm` and `rate_sigma_ppm` are `null` while `rate_valid` is `false`. Edges that aren't a whole number of seconds apart (within 20 ms) are ignored and not counted as rejects.

## /api/status: hardware

```
curl -s http://vseismo/api/status | jq .hw
{"cpu_mhz":240,"cores":1,"chip_rev":"v1.2","flash":"8MB","mac":"02:00:00:00:00:01","reset":"USB",
 "mem":{"int_free":98304,"int_min":91200,"int_largest":73728,"int_total":214000,
        "psram_free":5242880,"psram_min":5242880,"psram_largest":5177344,"psram_total":8388608},
 "radio":{"up":true,"rssi":-52,"ch":6,"bssid":"02:00:00:00:00:02","phy":"11ax",
          "tx_power_dbm":20.00,"disconnects":0,"last_reason":0},
 "stack_free":{"acq":2100,"timebase":1500,"httpd":4800,"tcpip":1900,"wifi":3000}}
```

The top-level `heap` and `heap_min` stay as they were: internal RAM and PSRAM added together. They say little on this board, because the 3 MB output ring sits in PSRAM. Watch `hw.mem.int_*` instead.

| Field | Unit | Meaning |
|---|---|---|
| `cpu_mhz`, `cores`, `chip_rev`, `flash`, `mac` | | CPU clock, core count, silicon revision, flash size, station MAC |
| `reset` | | Why the chip last reset: `power-on`, `USB`, `software`, `panic`, `task watchdog`, `brownout`, ... |
| `mem.int_free`, `mem.int_min` | bytes | Internal RAM free now, and the lowest it has been since boot. WiFi, lwIP, task stacks and the sample path all live here. |
| `mem.int_largest` | bytes | Largest single free block in internal RAM. Far below `int_free` means fragmentation. |
| `mem.int_total` | bytes | Internal RAM the heap manages |
| `mem.psram_*` | bytes | The same for PSRAM. All zero on a board without it. |
| `radio.up` | | Station associated, with an address |
| `radio.rssi`, `radio.ch`, `radio.bssid`, `radio.phy` | dBm, , , | Signal, channel, AP radio and protocol (`11ax`, `11ac`, `11n`, `11g`, `11b`). Empty while not associated. |
| `radio.tx_power_dbm` | dBm | Configured maximum transmit power |
| `radio.disconnects`, `radio.last_reason` | | Station disconnects since boot, and the IDF `wifi_err_reason_t` code of the last one (0 = none yet) |
| `stack_free.*` | bytes | Least free stack each task has had since boot (0 = task not running) |

## /api/wave

```
curl 'http://vseismo/api/wave?since=0&max=5'
{"ok":true,"next":1234,"seq0":1229,"n":5,"pending":0,"fs":100.0000,"t0":1794240527736,"tq":"pps",
 "t0_us":1794240527736412,"epoch":3,"boot":2840193377,
 "ch0":[12,-3,8,1,-6],"ch1":[40,38,41,44,39],"flags":[0,0,0,1,1]}
```

- Values are ADC counts after the decimating FIR, 24-bit scale.
- `t0` is the UTC time of the first sample in ms, `t0_us` the same in µs. Both are 0 until the station has wall-clock time. A recorder should use `t0_us`: with `pps` the timestamps are µs-grade, and `t0` truncates them.
- `fs` is the output rate of the samples in this reply. After a settings change, a catch-up read of older samples reports their rate, not the current one (for samples from the last 16 restarts; older ones get the current rate).
- `tq` is the time quality of the samples: `pps`, `ntp`, `rtc` or `none` (see /api/status: time). It is the same for every sample in a reply.
- One reply is one unbroken run: every sample is `t0_us + i / fs` to well within a sample period. Exactly so with `pps`; with `ntp` the slewed sample clock and DRDY interrupt jitter (tens of µs) add to that. A reply ends early, rather than span a break, where:
  - `epoch` changes: an acquisition restart (settings change, ADC reset), a SYNC, an ADC resync, or a gap too long to hold. The time grid moved, and `seq` alone doesn't show it.
  - `tq` changes, including the first wall-clock time arriving.
  - two neighbouring samples aren't one period apart to within half a period: a sample-clock step (a leap second, a bad RTC seed corrected).
- `epoch` is 0..65535 and wraps. Compare it for equality only. A recorder starts a new segment (a new miniSEED record) when it changes.
- `boot` is a random nonzero number drawn after each boot. `seq` starts at 0 on every boot, so a changed `boot` means a reboot and the old `next` no longer applies. Resume from `since=0`, which returns the oldest samples held.
- Timestamps are corrected for the full group delay (ADC sinc3, 1.5 input periods, plus the FIR, (taps − 1) / 2 input periods), so they mark when the ground moved.
- `flags` has one entry per sample. A bit is set when the FIR window of that sample held an input of that kind. A flagged sample is still on the correct time grid, but its value is not a clean measurement.

| Bit | Value | Meaning |
|---|---|---|
| 0 | 1 | Gap: an input sample was lost (read late) and replaced by the last good one |
| 1 | 2 | CRC: an input frame failed its CRC and was replaced by the last good one |
| 2 | 4 | Unsettled: inputs held while the ADC filter settled after a SYNC |
| 3 | 8 | Clip: an input sample was within 16 counts of full scale |

- After a restart, output starts only once the FIR history holds settled data (taps + 3 input samples), so there's no ramp up from zero.
- Poll again with `since=<next>`. If `seq0` isn't the `next` you asked for, there's a gap.
- The station keeps the last `ring_len` output samples (see `/api/acq`): 131072 with PSRAM, about 22 minutes at 100 SPS, or 4096 (41 s) on a board without it. Once the ring has wrapped, its oldest 512 samples are not served. The reply is streamed from the ring in small blocks, and this keeps the writer away from the samples being sent.
- If a sample is overwritten while its reply is still going out (only on a connection stalled for seconds), it's sent as the previous value with flag bit 0 set.
- One reply carries at most `max` samples. `order` picks which ones when more are waiting:
  - `order=newest` (the default): the newest `max`, cut back to the run that ends them. The rest are skipped, which suits a live display.
  - `order=oldest`: the oldest `max` the ring still holds, in order, up to the first break. Nothing is skipped. `pending` says how many newer samples follow, so a recorder polls again at once while `pending` is above 0. After an outage it can catch up on the whole ring, up to 22 minutes at 100 SPS, in steps of `max`.
- `pending` is the number of samples newer than the last one returned. With `order=newest` it is normally 0.
- Any other `order` value is an error.

```
curl 'http://vseismo/api/wave?since=118000&max=1000&order=oldest'
{"ok":true,"next":119000,"seq0":118000,"n":1000,"pending":5321,...}
```

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
| `capture` | | Edge timing in use: `isr` or `mcpwm`. Differs from the `capture` setting only if MCPWM setup failed at boot. |
| `cap_bad` | edges | With `mcpwm`: latched edge times (DRDY or 1 Hz reference) that did not land just before their interrupt, between 100 µs after and 5 ms before it. Each one is timed by the interrupt instead, so a broken capture path degrades to `isr` accuracy rather than wrong times. Should stay 0; a steady climb means a calibration, routing or unwrap fault. Always 0 with `isr`. |
| `cap_bad_us` | µs | How far the last such edge time was from its interrupt time. |
| `drdy_rate` | edges/s | DRDY edges counted per second over the last 1 s check, on either path. Matches `fs_in` to within a few tenths of a percent. |
| `drdy_rate_bad` | checks | 1 s checks where `drdy_rate` was more than 5 % off `fs_in`: edges counted twice (wrong edge, glitches) or missed. Should stay 0. |
| `ring_len` | output samples | Samples the station keeps for `/api/wave`: 131072 in PSRAM, or 4096 in internal RAM when the board has no PSRAM. Divide by `fs_out` for the history in seconds. |

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
| `ref_edge` | `rise`, `fall` | Reboot. GPS PPS marks the second on its rising edge. A DS3231 SQW rolls its seconds over on the falling edge, but the rate fit works on either, so for `ref=rtc` the choice only shows in the edge phase. |
| `rtc` | 0, 1 | Reboot. Implied by `ref=rtc`: the SQW only runs once the DS323x is configured, so with `ref=rtc` the RTC is driven (and `/api/status` `rtc.enabled` is `true`) whatever this value says. |
| `capture` | `isr` (default), `mcpwm` | Reboot. How DRDY and 1 Hz reference edges are timed. `isr`: a GPIO interrupt reads esp_timer, about 10 µs of jitter under WiFi load. `mcpwm`: one MCPWM capture timer latches both edges in hardware at 6.25 ns, kept as µs (0.3 µs rms rounding). If the capture timer can't be set up, the station falls back to `isr`; `/api/acq` `capture` shows what is in use. |
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
