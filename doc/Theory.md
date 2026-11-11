# vseismo: Theory of Operation

Vlad Patoka, VE3VPT. Revision November 2026.

This document is the design basis for the vseismo digitizer: an ADS131M02 delta-sigma ADC read by an ESP32-C5, behind a Webtronics EQAMP and two L-15B geophones. It covers the signal chain, the converter and its configuration, how samples are acquired and accounted for, the decimating filter, and the timing system that puts a UTC time on every output sample. Each part states the theory first, then what the firmware does with it, then the numbers.

Notation: $f_\text{CLKIN}$ is the ADC master clock, $f_\text{MOD}$ the modulator rate, $f_s$ the ADC output data rate (the firmware's input rate), $D$ the decimation factor, $f_\text{out} = f_s / D$ the station output rate, $N$ the FIR length, $T_s = 1/f_s$.

Default operating point used for every worked number: $f_\text{CLKIN}$ = 8.192 MHz, OSR 4096, $f_s$ = 1000 SPS, $D$ = 10, $N$ = 201, $f_\text{out}$ = 100 SPS.

---

## 1. Signal chain

```
L-15B Z ─┐                               9.1k / 1.0k 0.1 %
L-15B H ─┴─ Webtronics EQAMP ─────────── 47 nF C0G ───────── ADS131M02 ── SPI ── ESP32-C5
            ±10 V out, 20 Hz LP,                              CLKIN 8.192 MHz
            0.05 Hz HP
```

- Geophone: L-15B, 4.5 Hz natural frequency, velocity output above it.
- Amplifier: LT1007 input stage, LF444 gain and filter stages, a multi-pole 20 Hz low-pass and a 0.05 Hz high-pass, ±10 V full swing. At gain ×1000 the analog chain reaches about 105 to 110 dB of dynamic range, which sets the bar for the converter.
- Divider: 9.1 kΩ over 1.0 kΩ, 0.1 %, gain 0.0990. ±10 V becomes ±0.99 V, and a ±12 V rail clip still stays inside the ±1.2 V converter full scale at PGA 1.
- RC: 47 nF C0G across the 1.0 kΩ leg. The source resistance is the parallel combination, 901 Ω, so the pole is at

$$ f_c = \frac{1}{2\pi \cdot 901\,\Omega \cdot 47\,\text{nF}} \approx 3.76\ \text{kHz} $$

The input is pseudo-differential: each EQAMP channel arrives as a signal and ground pair and stays a pair down to the ADC, which cancels the few mV of power-return drop between the boards.

### 1.1 Anti-aliasing, end to end

A delta-sigma converter samples at $f_\text{MOD}$ and filters digitally, so there are three places where out-of-band energy could fold into the 0 to 50 Hz output band.

| Fold point | What protects it | Attenuation |
|---|---|---|
| Around $f_\text{MOD}$ = 4.096 MHz | RC pole at 3.76 kHz, single pole | $20\log_{10}(4.096\,\text{MHz}/3.76\,\text{kHz}) \approx 61$ dB, plus the EQAMP |
| Around $k f_s$ (1 kHz, 2 kHz ...) | sinc³ nulls, section 3, plus the EQAMP 20 Hz low-pass | 79 dB at 1050 Hz from the sinc³ alone |
| Around $k f_\text{out}$ (100 Hz ...) | Decimating FIR stopband, section 6, plus the EQAMP | Blackman stopband, about 74 dB |

The EQAMP's own multi-pole 20 Hz low-pass dominates everywhere above a few tens of Hz. The digital filters are there for modulator noise and for whatever the amplifier passes, not as the primary anti-alias filter.

---

## 2. Converter clock and data rate

### 2.1 Rate equations

The ADS131M02 divides CLKIN by 2 for its modulator and by OSR for its output:

$$ f_\text{MOD} = \frac{f_\text{CLKIN}}{2}, \qquad f_s = \frac{f_\text{CLKIN}}{2\,\text{OSR}} $$

At 8.192 MHz:

| OSR | $f_s$ | $T_s$ | Used by vseismo |
|---|---|---|---|
| 16256 | 251.97 SPS | 3.969 ms | allowed; non-integer rate |
| 8192 | 500 SPS | 2 ms | allowed |
| 4096 | 1000 SPS | 1 ms | default |
| 2048 | 2000 SPS | 0.5 ms | allowed |
| 1024 | 4000 SPS | 250 µs | allowed |
| 512 | 8000 SPS | 125 µs | lowest allowed |
| 256, 128 | 16, 32 kSPS | 62.5, 31.25 µs | rejected |

OSR 256 and 128 are rejected because the acquisition task wakes once per sample: at 16 kSPS one task switch plus a 12-byte SPI frame does not fit reliably in 62 µs under WiFi load. OSR 64 (turbo mode) is not used.

### 2.2 Clock sources

| Source | `clk_src` | Accuracy | $f_s$ at OSR 4096 |
|---|---|---|---|
| 8.192 MHz CMOS oscillator or TCXO on CLKIN | `ext` | ±20 to 50 ppm, or ±0.5 to 2.5 ppm | 1000 SPS |
| ESP32 LEDC from the 48 MHz XTAL, integer divider ÷ 6 | `ledc` | ±10 to 20 ppm | 976.5625 SPS |
| On-board LTC6903 on the stock Click | `ltc6903` | about ±1.1 % plus drift | about 1000 SPS |

The LEDC divider is kept integer: a fractional LEDC divider dithers the period, which is phase noise on the converter clock. The price is a non-round rate, and an output rate of 97.65625 SPS after decimation by 10.

### 2.3 Tolerance, stability and the reference

A constant frequency error $\varepsilon$ accumulates a time error of $\varepsilon \cdot 86400$ s per day:

| Frequency error | Per day |
|---|---|
| 10 ppm | 0.864 s |
| 2 ppm | 0.173 s |
| 0.5 ppm | 0.043 s |
| 0.1 ppm | 0.009 s |

With a 1 Hz reference the firmware measures the true sample rate (section 7), so the oscillator's initial tolerance hardly matters. What matters is stability over the measurement window (seconds to minutes) and, during reference loss, holdover. A plain ±50 ppm oscillator with GPS PPS outperforms a ±2 ppm TCXO without it. A TCXO earns its place when the station must hold time without PPS through temperature swings.

The oscillator sets sample-rate stability. Only an external reference sets alignment to UTC.

### 2.4 Electrical requirements and layout

CLKIN takes LVCMOS at DVDD levels. There is no clock-select register and no mode pins: the oscillator simply drives CLKIN.

| Parameter | Requirement |
|---|---|
| Nominal CLKIN | 8.192 MHz |
| Maximum, high-resolution mode | 8.4 MHz |
| Minimum | 0.3 MHz |
| Duty cycle | 40 to 60 % |
| Levels | LVCMOS, compatible with DVDD (3.3 V here) |

Layout rules followed on the carrier board:

1. Short oscillator-to-CLKIN trace over a continuous ground plane, no stubs, few vias.
2. A small series resistor at the oscillator output, chosen for its output impedance and the trace.
3. The clock never runs alongside the analog inputs. Its harmonics and return currents stay away from the divider, the RC and the EQAMP connector.
4. Analog and digital sections are physically separated. The converter's resolution is only reachable if the layout does not couple the clock and the ESP32 into the inputs.

Decoupling at the ADS131M02, close to the pins:

| Pin | Capacitor | Return |
|---|---|---|
| AVDD | 1 µF | AGND |
| DVDD | 1 µF | DGND |
| CAP | 220 nF | DGND (internal digital LDO, mandatory) |

The oscillator gets its own 100 nF plus bulk. The Click is fed from its own low-noise 3.3 V LDO behind a ferrite, never from the DevKit 3V3 pin, since the Click ties AVDD and DVDD together.

---

## 3. The sinc³ decimation filter

### 3.1 Response

Each channel's modulator stream is filtered by a sinc³ filter and decimated by OSR:

$$ H(f) = \left[ \frac{\sin(\pi f\,\text{OSR}/f_\text{MOD})}{\text{OSR}\,\sin(\pi f/f_\text{MOD})} \right]^3 \approx \left[ \frac{\sin(\pi f/f_s)}{\pi f/f_s} \right]^3 \quad (f \ll f_\text{MOD}) $$

It has nulls at every multiple of $f_s$ and passband droop toward $f_s/2$.

At $f_s$ = 1000 SPS:

| Frequency | Response |
|---|---|
| 20 Hz | −0.017 dB |
| 40 Hz | −0.069 dB |
| 1050 Hz (50 Hz from the first null) | −79.4 dB |

The droop is negligible inside the EQAMP's 20 Hz band. It belongs in the instrument response, not in a correction filter.

### 3.2 Delay and settling

The sinc³ impulse response is symmetric and $3\,\text{OSR}$ modulator clocks long, so its group delay is

$$ \tau_\text{sinc} = \frac{3\,\text{OSR}}{2 f_\text{MOD}} = 1.5\,T_s \quad (1.5 \text{ ms at } 1000 \text{ SPS}) $$

and its output is fully settled $3\,T_s$ after any discontinuity: a reset, a SYNC, or an input step. The firmware subtracts the delay from every timestamp (section 6.4) and holds back or flags the first three samples after a SYNC (section 5.5). The exact latency is a bench check.

---

## 4. Converter configuration and interface

### 4.1 Registers

MODE = `0x0110`:

| Bits | Field | Value | Meaning |
|---|---|---|---|
| 13:11 | CRC options | 0 | Register-map CRC off, input CRC off, CCITT polynomial |
| 10 | RESET | 0 | Clears the reset flag |
| 9:8 | WLENGTH | 01 | 24-bit words |
| 4 | TIMEOUT | 1 | SPI timeout on |
| 3:2 | DRDY_SEL | 00 | DRDY from the most lagging channel |
| 1 | DRDY_HiZ | 0 | DRDY driven, not high-impedance |
| 0 | DRDY_FMT | 0 | DRDY is a level, low until the frame is read |

CLOCK:

| Bits | Field | Value |
|---|---|---|
| 9 | CH1_EN | 1 |
| 8 | CH0_EN | 1 |
| 7:6 | reserved | 00 |
| 5 | TBM (turbo) | 0 |
| 4:2 | OSR | 101 for 4096, 110 for 8192 |
| 1:0 | PWR | 10, high resolution |

That gives CLOCK = `0x0316` at OSR 4096 and `0x031A` at OSR 8192:

```
0x031A = 0000 0011 0001 1010
         CH1_EN = 1, CH0_EN = 1, reserved = 00, TBM = 0, OSR = 110, PWR = 10
```

The firmware computes the value from the fields rather than storing constants.

Other per-channel registers:

- GAIN1: PGA code per channel, 1 to 128 in powers of two.
- CHn_CFG: input mux (normal, shorted, positive or negative test signal) and a phase trim in modulator clocks, $t_\text{MOD} = 1/f_\text{MOD}$ = 244 ns, range −512 to 511 (±125 µs). The phase trim shifts one channel's sampling instant against the other; it is per channel and is not folded into the shared timestamp.
- CHn_GCAL: 24-bit unsigned gain, 0x800000 = 1.0.
- CHn_OCAL: 24-bit two's complement offset, subtracted from each result per the datasheet. The sign is a bench check: after folding a shorted-input mean into OCAL, the mean must land near zero, not double.

### 4.2 SPI frame

SPI mode 1 (CPOL 0, CPHA 1), 8 MHz, 24-bit words. On the M02 a frame is four words in each direction:

```
MOSI:  command | (WREG data) | 0 | 0
MISO:  response | CH0 | CH1 | CRC-16
```

Commands are 16 bits left-aligned in a 24-bit word. The response to a command arrives in the next frame. NULL (`0x0000`) is a no-operation whose response is the STATUS register. It does not reset anything or clear faults. A single-register RREG costs two frames and a WREG two frames plus the acknowledge check.

The output CRC is CRC-16-CCITT, polynomial 0x1021, seed 0xFFFF, computed over every byte of the frame before the CRC word, pad bytes included. A frame whose CRC fails is not trusted (section 5.4).

### 4.3 STATUS

| Bit | Field | Use |
|---|---|---|
| 14 | F_RESYNC | The ADC resynchronised. Expected after our own SYNC; otherwise the sample grid moved. |
| 10 | RESET | The converter reset itself (brownout). It is back at defaults: OSR 1024, PGA 1. |
| 9:8 | WLENGTH | Must read 01. Anything else means the configuration is gone. |
| 1:0 | DRDY1, DRDY0 | New data for that channel in this frame |

Every good frame's STATUS is checked. RESET or a wrong word length triggers a full reconfigure. An unexpected F_RESYNC starts a new timing epoch. The DRDY bits tell a new conversion from a stale re-read (section 5.2).

### 4.4 Start-up and reconfiguration sequence

1. Start the clock source and wait for it (5 ms).
2. Hardware reset: hold SYNC/RESET low for more than 2048 CLKIN periods (2 ms, which also covers slow LTC6903 settings), release, wait 2 ms.
3. Read the ID register with a four-channel frame length, since the channel count is not known yet. Reject anything that isn't 0x2nxx with n from 1 to 4.
4. Write MODE, CLOCK, GAIN1 and each channel's CFG, OCAL and GCAL, reading every register back.
5. Design the FIR, compute the rates and delays, reset the sample counter and start a new timing epoch.
6. Issue a SYNC pulse (low for 1 to 2048 CLKIN periods; 20 µs is used) inside a critical section. An interrupt stretching the low time past 2048 periods (250 µs at 8.192 MHz) would turn the SYNC into a reset and silently discard the configuration.
7. Drain the two-deep data FIFO, start counting DRDY edges, and arm the DRDY path.

The clock must be present before the reset. With the LEDC path, CLKIN only appears after the ESP32 boots, which is why the reset comes after `clock_setup()` and not at power-up.

---

## 5. Acquisition and sample accounting

### 5.1 Principle

The ADC is the clock of the whole system. Every conversion it makes is one tick on the sample grid, and the firmware's job is to keep an exact count of those ticks, even when it fails to read one. A missed sample that silently disappears does two kinds of damage:

- The FIR history loses a sample, so the filter output is wrong for the next $N$ inputs.
- The sample count lags the true count. The rate measurement (section 7) uses that count: each lost sample per second biases the measured rate by $1/f_s$, which is 1000 ppm at 1000 SPS.

So the count must advance with the ADC whatever happens to the data.

### 5.2 Counting DRDY

The DRDY interrupt does three things only: it stores the edge time in a 16-entry ring indexed by the edge count, increments the count, and wakes the acquisition task. The task works from the count, never from the notification value, so a settings wakeup is never mistaken for a sample and a late wakeup still knows exactly how many conversions happened.

The ADS131M0x keeps a two-deep data FIFO per channel and, per the datasheet, drops the oldest when a third conversion arrives; both that and the STATUS DRDY bits are bench checks (section 11.2). On each wakeup the task reads frames until STATUS shows no new data, at most two. The frames read are the newest conversions. If the count says $k$ conversions happened and $v$ were read, the $k - v$ older ones are lost.

Reading until the FIFO is empty matters for timing too. Without it, a backlog left after one late wakeup would pair every following sample with the next DRDY's timestamp: a permanent one-period error.

Until STATUS has shown its DRDY bits once after a restart, a stale frame cannot be told from a new one, so the task reads only as many frames as there were edges. If the bits never appear, the check is switched off and logged.

### 5.3 Lost samples

A lost sample is replaced by the last good value. It still gets its own DRDY time from the timestamp ring, still advances the sample count, and still enters the FIR history, so the filter keeps its grid. A gap longer than $2N + 16$ samples is not bridged: the count is advanced, the timing epoch restarts, and output is held back until the FIR has refilled with real data.

### 5.4 Corrupt frames

A frame that fails its CRC still consumed one conversion, so it counts as a sample, but its value is replaced by the last good one.

### 5.5 Settling and restarts

After a restart the FIR history is empty. Rather than emit a ramp from zero, output is held back for $N + 3$ input samples: $N$ to fill the filter, 3 for the sinc³ to settle. After a SYNC requested at run time, the next 3 inputs are held at the last good value and the timing epoch restarts, since the sample grid has moved.

### 5.6 Flags

Each output sample carries a flag byte. A flag is set when the FIR window that produced the sample contained an input of that kind:

| Bit | Flag | Input |
|---|---|---|
| 0 | gap | lost and held |
| 1 | CRC | failed its CRC, held |
| 2 | unsettled | held during sinc³ settling after a SYNC |
| 3 | clip | within 16 counts of full scale |

A flag lasts $N$ inputs after the event, exactly as long as the event influences the output. A flagged sample is on the correct time grid, but its value is not a clean measurement.

### 5.7 Output ring

Output samples go into a ring of 131072 entries (3 MB) in PSRAM, 22 minutes at 100 SPS, or 4096 entries in internal RAM on a board without PSRAM. The ring is single-writer and lock-free. The writer fills a slot and then publishes the new head with release ordering. A reader loads the head, copies, loads the head again and drops whatever the writer may have reached meanwhile. It never returns a torn sample, and the acquisition task never waits on a slow reader.

No interrupt handler touches the ring. Everything an interrupt handler reads or writes (the DRDY timestamp ring, the counters, the capture state) is in internal RAM, so the handlers keep running while a flash write has the cache off.

---

## 6. Decimating FIR

### 6.1 Design

A windowed-sinc low-pass with a Blackman window, linear phase, unity DC gain, cutoff at $0.4 f_\text{out}$ (40 Hz at 100 SPS). With $f_c = 0.4/D$ in cycles per input sample and $m = N - 1$:

$$ h_n = \frac{\sin\left(2\pi f_c (n - m/2)\right)}{\pi (n - m/2)} \left[ 0.42 - 0.5\cos\frac{2\pi n}{m} + 0.08\cos\frac{4\pi n}{m} \right], \qquad n = 0 \ldots m $$

normalised so that $\sum h_n = 1$. The default length is $N = 20D + 1$, so 201 taps at $D$ = 10.

### 6.2 Length versus decimation

A Blackman window gives a transition width of about $5.5/N$ cycles per input sample, centred on $f_c$. For aliases to stay out of the band up to $0.4 f_\text{out}$, the stopband must begin by $0.6 f_\text{out}$:

$$ \frac{0.4}{D} + \frac{2.75}{N} \le \frac{0.6}{D} \quad \Rightarrow \quad N \ge 13.75\,D $$

At $N$ = 201, $D$ = 10: transition about 27 Hz wide, stopband from about 54 Hz, so only 46 to 50 Hz of the output band sees partial attenuation. The maximum length of 255 taps limits $D$ to 18, and the firmware rejects anything larger. Shorter hand-set lengths are allowed but logged when $N < 14 D$.

### 6.3 Integer arithmetic

The sample path has no floating point. Coefficients are Q30, samples 24-bit, and the accumulator int64. The worst case is

$$ |acc| \le 2^{23} \sum |h_n| \cdot 2^{30} \approx 2^{53} $$

well inside 63 bits. The result is rounded with $(acc + 2^{29}) \gg 30$. The DC gain error from coefficient rounding is on the order of $10^{-9}$. Only every $D$-th output is computed.

### 6.4 Total delay

Linear phase gives a constant group delay of $(N-1)/2$ input periods. Added to the converter's:

$$ D_\text{total} = 1.5 + \frac{N - 1}{2} \ \text{input periods} = 101.5 \text{ ms at the default point} $$

An output sample therefore stands for input-sample position $n - D_\text{total}$, where $n$ is the input count at its newest input. Timestamps mark when the ground moved, not when the sample left the filter.

The analog chain's own delay (the EQAMP low-pass, the geophone) is frequency-dependent. It belongs in the instrument response, not in the timestamp.

### 6.5 Rate and noise choice

Converter noise is shaped out of band by the modulator and removed by the sinc³. What is left in band is close to white thermal noise. After decimation to 100 SPS, the noise in 0 to 50 Hz is set by that density, not by OSR: OSR 4096 with $D$ = 10 and OSR 8192 with $D$ = 5 land within a fraction of a dB of each other. 1000 SPS is chosen because it leaves room above the band for the FIR's transition and keeps the sinc³ droop negligible. 500 SPS is the natural choice only when the raw converter rate is itself the output.

---

## 7. Time, part 1: measuring the sample rate

### 7.1 Three timelines

| Timeline | Source | Role |
|---|---|---|
| Sample count $n$ | DRDY edges | The grid the data lives on: exact order, no gaps |
| esp_timer, µs | ESP32 crystal | Local interval measurement, latency, ages |
| UTC | GPS PPS, SNTP, DS3231 | Absolute time |

Timestamps are not taken when an SPI transfer finishes. They are derived from the sample count, which the ADC defines, and from edge times, which the hardware latches.

### 7.2 Position at a reference edge

At a 1 Hz reference edge at esp_timer time $t_e$, the latest DRDY was number $n$ at time $t_d$. The fractional sample position of the edge is

$$ p = n + (t_e - t_d)\,f_\text{nom} $$

Using the nominal rate here costs a ppm-sized fraction of $|t_e - t_d| \le T_s$, which is negligible. Successive edges give pairs $(x_k, p_k)$, where $x_k$ is the reference second, counted by edge spacing rounded to whole seconds.

### 7.3 Least-squares rate

The positions follow a straight line whose slope is the true sample rate:

$$ p(x) = p_0 + f_\text{nom}\,x + a + b\,x, \qquad f_s^{\text{true}} = f_\text{nom} + b $$

The fit is done on residuals $y = p - p_0 - f_\text{nom} x$, so the sums stay small, using a two-pass centred least-squares over the last 300 edges. The same fit of the edge times, $e = t_e - t_0 - 10^6 x$, gives the ESP crystal error as its slope, in µs per second, which is ppm.

Error analysis. Let each position carry white noise $\sigma_p$, from the jitter on both the DRDY and the reference timestamp:

$$ \sigma_p = f_s \sqrt{\sigma_{t,d}^2 + \sigma_{t,e}^2} $$

For equally spaced points, $S_{xx} = N(N^2 - 1)/12$, so the slope error is

$$ \sigma_b = \frac{\sigma_p}{\sqrt{S_{xx}}} = \sigma_p \sqrt{\frac{12}{N(N^2-1)}} \approx \sigma_p \sqrt{\frac{12}{N^3}} $$

The usual alternative, averaging per-second differences $p_k - p_{k-1}$ with an exponential filter of weight $\alpha$, turns white phase noise into frequency noise:

$$ \sigma_f \approx \alpha\,\sigma_p \sqrt{1 + \tfrac{\alpha}{2 - \alpha}} $$

With 10 µs of jitter on each stamp at 1000 SPS, $\sigma_p$ = 0.0141 samples:

| Estimator | Rate error |
|---|---|
| EMA of differences, $\alpha$ = 0.1 | 1.45 ppm |
| Least-squares slope, $N$ = 300 | 0.0094 ppm |

The window is a trade-off. 300 s averages jitter well, but follows oscillator temperature drift with a lag of about half the window.

### 7.4 Robustness

- Spacing: an edge that is not a whole number of seconds (±20 ms) after the last good edge is ignored as a glitch. Five in a row mean the reference phase moved (an RTC write, a GPS re-lock), and the fit restarts. Ignored edges never move the last good edge, so without that restart every later edge would be ignored too.
- RTC writes: each write restarts the DS3231 countdown and moves the SQW phase. With the SQW as reference the fit restarts on the first edge after a write. Writes are rare on purpose: the first SNTP sync after boot, after OSF, and once a day (2 ppm is 0.17 s a day).
- Outliers: an edge whose position is more than max(0.5 samples, 200 µs) off the fitted line is dropped and counted. Five in a row mean the rate itself moved, and the fit restarts.
- Gaps up to 30 s are bridged by the second count. A longer gap restarts the fit.
- Any acquisition restart, big gap or SYNC changes the timing epoch, and positions from different epochs are never mixed.
- The rate is reported after 10 edges, together with the fit's own $1\sigma$ from its residuals.

---

## 8. Time, part 2: timestamping samples

### 8.1 Two models

Every output sample is timestamped by one of two models, and carries the name of the one used as its time quality:

| Quality | Condition | Model |
|---|---|---|
| `pps` | GPS PPS present, rate fit valid | Sample-count model, section 8.2 |
| `ntp` | SNTP time, no usable PPS | Slewed sample clock, section 8.3 |
| `rtc` | DS3231-seeded time (set at its seconds rollover), no SNTP yet | Slewed sample clock |
| `none` | No wall-clock time | No UTC |

A DS3231 SQW reference measures the rate only. Its edges are aligned to the RTC's own seconds, which were set from SNTP and drift at ±2 ppm, so they are not UTC seconds and never select the `pps` model.

### 8.2 Sample-count model (GPS PPS)

The fitted line of section 7.3 maps reference seconds to sample position. Inverted, it maps any sample position to reference seconds:

$$ x(p) = \frac{p - p_0 - a}{f_\text{nom} + b} $$

The PPS edge marks the start of a UTC second. SNTP is used only to name it. At each edge the system clock's reading is rounded to the nearest second, giving the UTC second of $x = 0$:

$$ \text{UTC}_{x_0} = \text{round}\left(\text{UTC}_\text{sys}(t_e)\right) - x_k $$

and the timestamp of an output sample at position $p$ is

$$ t_\text{UTC}(p) = \text{UTC}_{x_0} + x(p), \qquad p = n - D_\text{total} $$

Properties:

- SNTP error, SNTP steps and interrupt jitter stay out of the timestamps. SNTP only has to be right to within half a second.
- The phase comes from the fit, not from the last edge. The variance of a fitted line at the end of its window is

$$ \sigma_\text{end}^2 = \sigma_p^2 \left( \frac{1}{N} + \frac{(N/2)^2}{S_{xx}} \right) \approx \frac{4\sigma_p^2}{N}, \qquad \sigma_\text{end} = \frac{2\sigma_p}{\sqrt N} $$

which is 1.63 µs for 10 µs stamp jitter and $N$ = 300.
- Naming errors: a single edge whose rounded name disagrees is ignored. Three in a row move the anchor and are counted. That is the expected behaviour at a leap second, or at the first SNTP sync after an RTC seed that was seconds off. Only a clock with an SNTP sync in the last 2 h may move the anchor; after a longer outage the crystal-driven system clock could be off by more than half a second, and the edge count keeps the names. Without network at boot, the names come from the RTC seed. That seed is set at the RTC's seconds rollover: the seconds register is polled every 2 ms until it changes, and the edge is placed midway between the two reads, so it is off by at most half their gap. A change seen across a gap over 6 ms is skipped and the next rollover is used, which keeps the seed within 3 ms and the rounding safe. The poll gives up after 3.5 s. The skip matters at boot: the seed runs while WiFi starts, and PHY calibration (priority 23) can hold the timebase task (priority 10) off for most of a second. Before the skip, midpoints taken across such a gap put the seed 78 and 494 ms off on the bench. A plain whole-second seed would land 0 to 1 s behind and name every PPS second one early whenever it landed past the half second.
- Holdover: the model is used for 30 s after the last good edge. Over that time the frequency error of the fit adds $\sigma_b \cdot 30$ s, a fraction of a µs.

### 8.3 Slewed sample clock (no PPS)

Without PPS, a sample's time is its DRDY time (minus the delay) mapped through an offset from esp_timer to UTC. Taking that offset straight from the system clock would copy every hourly SNTP step into the samples. Instead the offset is a continuous piecewise-linear function

$$ \text{off}(t) = \text{off}_0 + r\,(t - t_0) $$

re-based about once a second at the current time, with a new rate toward the target offset $\text{off}^*$:

$$ r = \text{clamp}\left( \frac{\text{off}^* - \text{off}(t)}{H}, \pm 500\ \text{ppm} \right) + r_\text{ff}, \qquad H = 4\ \text{s} $$

- A 10 ms SNTP correction is slewed in over 20 s. A change larger than 0.5 s (a leap second, a bad RTC seed) is applied at once.
- Samples are stamped up to $D_\text{total}$ in the past. A time before the latest re-base is evaluated on the previous segment, so neighbouring samples never straddle a discontinuity of (rate change) × (delay).
- Between SNTP syncs the system clock runs on the ESP timer, so its offset to esp_timer is constant and $r_\text{ff} = 0$.

IDF's own smooth SNTP mode is not used. It slews at 1/64, which is 15 625 ppm, and would squeeze a 5 ms correction into 0.3 s of samples.

The system clock itself keeps immediate SNTP, so log times stay exact.

### 8.3.1 SNTP sample checks

The firmware replaces IDF's `sntp_sync_time()`, which would apply every reply. A reply is checked first:

- Plausible window: build date minus one day to 2106-02-07 (the end of lwIP's 32-bit NTP seconds).
- The first accepted sample of a boot is applied as is: an RTC seed can be up to a second off.
- After that, the system clock can only have drifted by the ESP crystal error since the last good sample. A correction $|\Delta|$ larger than

$$ 200\ \text{ms} + 50\ \text{ppm} \cdot (t - t_\text{last}) $$

  is held, the server is re-polled after 30 s, and the held value is applied only when the next sample is within 100 ms of it.

Stock lwIP takes the server's transmit time as is, so a reply's error is the time it spent in the network. On a congested uplink that reached 0.83 s on the bench (mtr showed 275 ms average and 1.48 s worst RTT past the ISP edge). Applied, that would move `ntp` timestamps by 0.83 s and, being over 0.5 s, rename the PPS seconds.

The firmware builds lwIP with `SNTP_COMP_ROUNDTRIP=1`, which computes the RFC 4330 offset from all four timestamps, and `SNTP_CHECK_RESPONSE=2`, which accepts only a reply from the server asked that echoes our request timestamp. Symmetric delay cancels; asymmetric delay still leaks through at half its size, and the hold rule above catches what is left. The first server slot is filled from DHCP option 42, so a LAN server (1-2 ms away, its own clock filter already applied) is preferred over the pool.

### 8.4 Steering by PPS and handover

While the `pps` model holds, the slewed clock's target is PPS time rather than the system clock. At each good edge,

$$ \text{off}^*_\text{pps} = 10^6\,(\text{UTC}_{x_0} + x_k) - t_e, \qquad r_\text{ff} = -\varepsilon_\text{esp} $$

where $\varepsilon_\text{esp}$ is the crystal error from the rate fit. The target therefore drifts with the crystal between edges, and keeps drifting through holdover. When holdover ends, the fallback clock is already at PPS time, and slews from there toward SNTP at the 500 ppm limit. There is no jump.

Without the feed-forward, a 12 ppm crystal over 30 s of holdover would leave 360 µs to jump at handover.

### 8.5 What the timestamp does not include

- The per-channel phase trim: a few µs, configured, per channel.
- The analog chain's frequency-dependent delay: instrument response.
- The GPS module's own PPS error and the antenna cable delay: module-dependent, typically tens of ns, not modelled.

---

## 9. Time, part 3: edge timing

### 9.1 Interrupt timing

With `capture=isr`, an edge's time is `esp_timer_get_time()` read inside its GPIO interrupt. The error is the interrupt latency, typically a few µs but about 10 µs rms under WiFi load, with occasional spikes of hundreds of µs. The models above average it, but it is still the dominant noise term:

- Rate fit: 0.014 ppm (simulated).
- `pps` timestamps: 1.7 µs rms.
- `ntp` timestamps: each sample carries its own jitter, $\sqrt 2 \cdot 10\ \mu\text{s} / 10\ \text{ms} \approx 1400$ ppm of timescale noise between neighbouring samples at 100 SPS.

### 9.2 Hardware capture

With `capture=mcpwm`, DRDY and the 1 Hz edge are wired to two channels of one MCPWM capture timer. The hardware latches the counter at the edge, and the interrupt only reads the latched value later. Latency drops out, and both edges share one counter, so their difference is exact.

On the ESP32-C5 the capture timer runs at 160 MHz (6.25 ns). Its 32-bit count wraps every

$$ 2^{32} / 160\ \text{MHz} = 26.84\ \text{s} $$

Unwrapping. The full 64-bit count is reconstructed from the 32-bit value $c$ using esp_timer as a coarse guide. With $(C_\text{last}, t_\text{last})$ the newest unwrapped count and the esp time it was seen at, and $k$ counts per µs:

$$ C_\text{exp} = C_\text{last} + (t_\text{now} - t_\text{last})\,k, \qquad C = C_\text{exp} + \text{int32}(c - C_\text{exp} \bmod 2^{32}) $$

This is correct as long as the guide is within half a wrap, ±13.4 s. Interrupt latency is microseconds, so it is always satisfied, whatever the edge order or the time since the last edge. Edges from the two channels may be serviced out of order; only a newer count moves the reference.

Conversion. Counts are converted to esp_timer microseconds, so the rest of the firmware is unchanged:

$$ t = \text{round}(C / k) + t_\text{off} $$

The offset $t_\text{off}$ is set once at start-up by a software-triggered capture read together with esp_timer. It carries that interrupt's latency, a constant few µs, which cancels in every difference the timing models use. Rounding to µs leaves $1/\sqrt{12} = 0.29$ µs rms of white noise, the new floor.

Effect, by the formulas of sections 7.3 and 8.2 with $\sigma_t$ = 0.29 µs:

| Quantity | `isr` (10 µs) | `mcpwm` (0.29 µs) |
|---|---|---|
| Position noise $\sigma_p$ | 0.0141 samples | 0.00041 samples |
| Rate error, $N$ = 300 | 0.0094 ppm | 0.00027 ppm |
| `pps` phase error $2\sigma_p/\sqrt N$ | 1.63 µs | 0.047 µs |
| `ntp` neighbour jitter at 100 SPS | ~1400 ppm | ~40 ppm |

The data values are unaffected. Capture changes only when an edge is said to have happened.

### 9.3 Conditions

- The capture hardware holds one value per channel. Its interrupt must run while flash writes have the cache off (`CONFIG_MCPWM_ISR_CACHE_SAFE`), or DRDY edges during a settings save would go uncounted. The GPIO path runs from IRAM for the same reason.
- Every function and variable the capture interrupt touches is in internal RAM. The 64-bit division it uses is in ROM.

### 9.4 Self-checks

A hardware path that runs but misbehaves (wrong pin routing, wrong edge, a bad calibration) would not show up as an error by itself. Two checks make it visible:

- Per edge: a latched time must land just before its interrupt, between 100 µs after (the calibration bias) and 5 ms before. One outside that window is counted and timed from the interrupt instead, so a faulty capture path falls back to interrupt accuracy, never to wrong times.
- Per second, on both paths: the DRDY count rate must match the nominal rate within 5 %. That catches edges counted twice or missed. The window is wider than the LTC6903's 1 % error and far wider than the 0.4 % count quantisation at the lowest rate.

If the capture timer cannot be set up at all, the station falls back to `isr` and reports which path is in use. `isr` remains the default until `mcpwm` has passed the bench checks.

---

## 10. Error budget

| Mode | Absolute UTC | Between neighbouring samples | Rate |
|---|---|---|---|
| `pps`, `capture=mcpwm` | about 0.05 µs, plus the GPS module and cable | exact grid | 0.0003 ppm |
| `pps`, `capture=isr` | about 1.7 µs rms | exact grid | 0.01 ppm |
| `ntp`, `capture=mcpwm` | SNTP over WiFi, 1 to 10 ms, never steps | 0.3 µs | from a reference if fitted |
| `ntp`, `capture=isr` | same | about 10 µs | same |
| `rtc` | seconds; a DS3231 drifts about 0.17 s per day | as `ntp` | as `ntp` |

For a public seismic network station at 100 SPS (a sample every 10 ms), absolute time to about 1 ms is the working requirement. `ntp` meets it on a good network, and `pps` meets it with three to four orders of margin.

---

## 11. Verification

### 11.1 Host simulation

The timing code was compiled on a host computer, unchanged, against stub system headers. It was driven with:

- an ADC clock 23.4 ppm fast and an ESP crystal 12 ppm fast;
- Gaussian jitter on every DRDY and PPS stamp;
- a system clock that drifts and is stepped by up to ±3 ms at each simulated SNTP sync.

Timestamps were compared with the true sample times.

| Case | Result |
|---|---|
| Rate fit, 10 µs jitter | 0.0136 ppm rms (EMA of differences: 1.43 ppm) |
| Rate fit, 0.29 µs jitter | 0.0003 ppm rms (EMA: 0.055 ppm) |
| `pps` timestamps, 900 s, SNTP steps | 1.67 µs rms, 9.5 µs worst |
| One mis-named PPS edge | ignored |
| System clock 1 s off for good | renamed after 3 edges |
| PPS lost, 30 s holdover | within 1.3 µs |
| Handover to SNTP | no jump: 525 ppm worst between neighbours (500 slew + 12 feed-forward + rounding) |
| No PPS, an hour of SNTP steps, no jitter | 600 ppm worst between neighbours (500 slew + rounding) |
| Capture unwrapping, 101 081 edges, latency to 2 ms, out-of-order, 60 s silence | no wrap error; 0.32 µs worst after the constant calibration bias |
| Capture calibration 1 s off | every edge flagged and timed from the interrupt |
| Output ring: both read orders, behind, wrap of the 32-bit sequence | all checks pass |

The simulation found three faults before they reached hardware, all fixed in the design described above:

- The slew was applied in 1 s chunks, which made 500 µs steps.
- A 1 s step threshold sat exactly at leap-second size.
- The PPS target ignored crystal drift, which left a 350 µs jump at the end of holdover.

### 11.2 Bench checks

1. Boot: PSRAM found and tested, the output ring placed in PSRAM.
2. Converter: STATUS DRDY bits set on new data; backlog drops and lost samples at 0 in steady state; DRDY rate within a few tenths of a percent of nominal; F_RESYNC only after our own SYNC; the sinc³ delay confirmed against a step input.
3. With GPS PPS and `capture=isr`: `pps` quality after 10 edges; note the rate fit's $1\sigma$.
4. With `capture=mcpwm`: both capture channels in the boot log; no capture sanity failures and no rate check failures; lost samples flat across a settings save; the rate fit's $1\sigma$ down by more than ten times. `mcpwm` then becomes the default.
5. Noise floor with shorted inputs, under `isr` and `mcpwm`, to confirm the capture clock adds nothing measurable.

---

## 12. Design decisions

| Item | Decision | Reason |
|---|---|---|
| Converter | ADS131M02 | One modulator per channel, simultaneous sampling |
| Master clock | External 8.192 MHz oscillator | Independent of the processor; TCXO only if holdover without PPS matters |
| Power mode | High resolution | |
| Converter rate | OSR 4096, 1000 SPS | Room for the FIR transition above 50 Hz, negligible sinc³ droop |
| Output | FIR decimation by 10 to 100 SPS, 201 taps | PSN / WinSDR rate; noise equal to OSR 8192 / 5 |
| Rate limits | OSR ≥ 512, decimation ≤ 18 | One task wake per sample; a 255-tap FIR |
| Sample accounting | Count every DRDY, hold and flag bad inputs | Keeps the grid and the rate measurement exact |
| Rate measurement | Least-squares fit over 300 PPS edges | 100 times less noise than differencing |
| Timestamps | Sample-count model with PPS; slewed clock without | No SNTP steps or interrupt jitter in the data |
| Edge timing | `isr` default, `mcpwm` opt-in until bench-proven | Hardware path untested; `isr` loses little once the model averages |
| Error detection | Output CRC, STATUS on every frame, sanity counters | Faults become counters, not silent bias |
| History | 22 min ring in PSRAM, heap allocation only | Catch-up after outages; a PSRAM fault can only touch history |
| Code placement | Firmware runs from flash, not PSRAM (no XIP) | In steady state nothing writes flash, so XIP gains little; it would make a PSRAM timing fault a crash instead of a data error |
| Transport | Separate tasks, lock-free ring | Acquisition never waits on the network |

---

## References

- Texas Instruments, [ADS131M02 datasheet, SBAS853A](https://www.ti.com/lit/ds/symlink/ads131m02.pdf): clocking, digital filter, SPI frame, CRC, STATUS, FIFO, SYNC/RESET, layout and decoupling.
- Texas Instruments, [ADS131M02 application example](https://www.alldatasheet.jp/html-pdf/1279776/TI/ADS131M02/2657/74/ADS131M02.html): an MCU supplying CLKIN.
- Espressif, ESP-IDF v6.1: MCPWM capture driver, SPI flash cache handling, SNTP and `adjtime`.
- Larry Cochrane, Webtronics: EQAMP and PSN-ADC24 schematics and firmware.
- [`doc/API.md`](API.md): the station's HTTP interface, where every quantity above is reported.
