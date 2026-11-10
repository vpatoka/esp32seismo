# ADS131M02 + ESP32-C5: External 8.192 MHz clock — technical review

Your overall approach is sound: using a dedicated external clock for the ADS131M02 is a good design decision for a long-term seismic acquisition system.

However, there is one significant mathematical error in the original document, along with several hardware and firmware details that need correction.

The most important correction is the ODR calculation. The ADS131M02 internally divides the incoming CLKIN frequency by 2 to generate its modulator clock. It does not divide it by 128.

The correct formula is:

\\[ f\_{\text{MOD}}=\frac{f\_{\text{CLKIN}}}{2} \\]

\\[ \boxed{\text{ODR}=\frac{f\_{\text{CLKIN}}}{2\times\text{OSR}}} \\]

This changes all four sample rates in your original document.

## 1. Corrected clock configuration and ODR calculations

For an external clock of exactly 8.192 MHz, the supported settings produce:

| OSR  | Output data rate | Sample interval | Typical application                       |
| ---- | ---------------- | --------------- | ----------------------------------------- |
| 8192 | 500 SPS          | 2 ms            | Low-frequency seismic monitoring          |
| 4096 | 1,000 SPS        | 1 ms            | General seismic acquisition               |
| 2048 | 2,000 SPS        | 0.5 ms          | Local events and higher-frequency signals |
| 1024 | 4,000 SPS        | 0.25 ms         | Higher temporal resolution                |
| 512  | 8,000 SPS        | 125 µs          | High-frequency acquisition                |
| 256  | 16,000 SPS       | 62.5 µs         | Specialized transient capture             |
| 128  | 32,000 SPS       | 31.25 µs        | High-speed acquisition                    |

The device also supports OSR 64 through Turbo Mode and an additional OSR setting of 16256. These are less likely to be useful for your seismic application.

Important: Your original recommendation of 500 SPS for local microseismic detection was actually the correct sample rate for your intended application, but it requires OSR 8192, not OSR 2048.

### Recommended operating point

Recommended starting configuration

- External clock: 8.192 MHz
- Power mode: High-resolution
- OSR: 8192
- Output rate: 500 SPS
- Sample interval: 2 ms
- Both ADC channels enabled

I would start with 500 SPS and evaluate the actual noise floor and frequency response of the complete sensor/preamp/ADC chain before increasing the sample rate.

For example, 500 SPS gives a Nyquist frequency of 250 Hz. However, this does not mean the complete acquisition system has a flat response up to 250 Hz. The preamplifier, sensor, and ADC digital filter all influence the usable bandwidth.

The ADS131M02 uses a sinc-type digital filter. Its bandwidth and settling behavior change with OSR. Higher OSR generally improves noise performance at the expense of bandwidth and temporal resolution.

### How vseismo uses this

The firmware does not output the raw ADC rate. It runs the ADC at 1000 SPS (OSR 4096) and decimates by 10 with a 201-tap FIR to 100 SPS, the PSN / WinSDR rate. With decimation, the in-band noise at 0 to 50 Hz is set by the white thermal noise of the modulator, not by OSR: OSR 4096 / decimate 10 and OSR 8192 / decimate 5 land within a fraction of a dB of each other. The 500 SPS figure above applies when the raw rate is the output.

The sinc³ filter also adds a group delay of about 1.5 data periods (1.5 ms at 1000 SPS). The firmware subtracts it, together with the FIR delay of (taps − 1) / 2 input periods, from every output timestamp.




## 2. External clock: accuracy, stability and jitter

There is an important distinction between using an external clock and having a crystal-accurate clock.

An external oscillator does not automatically eliminate frequency drift or jitter. Its performance depends on the oscillator itself.

For a seismic monitoring station, I recommend:

- A dedicated 8.192 MHz CMOS/LVCMOS oscillator.
- Frequency tolerance of 10 ppm or better.
- Low phase noise and low cycle-to-cycle jitter.
- Good temperature stability.
- A stable supply voltage with appropriate decoupling.

A TCXO (Temperature-Compensated Crystal Oscillator) is worth considering if the station will operate outdoors in changing temperatures.

For comparison:

| Clock accuracy | Frequency error | Time error accumulated per day |
| -------------- | --------------- | ------------------------------ |
| 10 ppm         | 0.00001         | 0.864 seconds                  |
| 2 ppm          | 0.000002        | 0.173 seconds                  |
| 0.5 ppm        | 0.0000005       | 0.043 seconds                  |
| 0.1 ppm        | 0.0000001       | 0.009 seconds                  |

These are frequency-derived timing errors, assuming constant frequency offset.

A particularly important point for seismic networks: an accurate ADC clock does not automatically give you UTC-synchronized timestamps.

If you eventually want to compare seismic events between multiple stations, you should plan for GPS PPS (Pulse Per Second) synchronization or another disciplined time reference.

The oscillator determines sample-rate stability; the PPS reference determines alignment to absolute time.

## 3. Hardware wiring and signal integrity

The ADS131M02 accepts an external LVCMOS clock directly on its `CLKIN` pin.

There is no separate external-clock selection register, and there are no M0/M1 clock-mode selection pins on this device.

The external oscillator simply drives CLKIN.

### Recommended connections

## Clock and ADC connections

8.192 MHz oscillator

LVCMOS output, compatible with ADC DVDD

CLKIN — ADS131M02

ADS131M02

- DRDY
- CS
- SCLK
- DIN
- DOUT
- SYNC/RESET

ESP32-C5

- GPIO interrupt
- GPIO
- SPI SCLK
- SPI MOSI
- SPI MISO
- GPIO

All digital interfaces must share a common ground and use compatible logic levels.

### Clock electrical requirements

According to the TI datasheet:

| Parameter               | Requirement         |
| ----------------------- | ------------------- |
| Nominal CLKIN           | 8.192 MHz           |
| High-resolution maximum | 8.4 MHz             |
| Minimum frequency       | 0.3 MHz             |
| Duty cycle              | 40–60%              |
| Clock input             | LVCMOS              |
| Input high voltage      | At least 0.8 × DVDD |
| Input low voltage       | At most 0.2 × DVDD  |

The oscillator output must be compatible with the ADC's DVDD supply. Do not assume that a 5 V oscillator module can connect directly to CLKIN.


### PCB layout recommendations

I would make the following changes to your original layout recommendations:

1. Keep the oscillator-to-CLKIN trace short.
2. Route the clock over a continuous ground plane.
3. Avoid routing the clock parallel to high-impedance analog input traces.
4. Avoid unnecessary vias and long clock stubs.
5. Consider a small series termination resistor near the oscillator output. Its value should be selected based on the oscillator output impedance and PCB trace characteristics.
6. Keep the oscillator and its digital return currents away from the sensitive preamplifier input circuitry.

One particularly important consideration is your Larry's Webtronics preamplifier.

Do not run the 8.192 MHz clock alongside the analog sensor signal or preamplifier input traces. Clock harmonics and switching currents can introduce interference that may be particularly problematic when measuring very small seismic signals.

### Power supply decoupling

Your original recommendation of 100 nF capacitors is useful but incomplete.

For the ADS131M02, follow TI's specific requirements:

| Pin  | Recommended capacitor | Connection |
| ---- | --------------------- | ---------- |
| AVDD | 1 µF                  | AGND       |
| DVDD | 1 µF                  | DGND       |
| CAP  | 220 nF                | DGND       |

Place these capacitors close to their respective pins.

Also provide local decoupling for the oscillator supply, typically 100 nF plus an appropriate bulk capacitor.

Do not omit the CAP capacitor: it is required for the internal digital LDO configuration.


## 4. Firmware initialization sequence (ESP32-C5)

I would change your original initialization sequence considerably.

The biggest issue is the suggestion that sending a NULL command clears fault states and synchronizes the SPI interface.

That is not what the NULL command does.

The ADS131M02 defines NULL (`0x0000`) as a no-operation command. It returns the STATUS register but does not reset the device or clear all fault conditions.

### Recommended initialization sequence

1. Initialize the oscillator

   Start the 8.192 MHz oscillator and allow it to stabilize according to its manufacturer's startup specification.

   Do not assume that every oscillator requires exactly 2 ms.
2. Power up the ADS131M02

   Ensure AVDD and DVDD are stable and that CLKIN is already receiving a valid clock, or starts immediately after supply stabilization.

   TI specifies a typical startup time of approximately 0.5 ms from the supply reaching 90% of nominal voltage to the first DRDY falling edge under specified conditions.
3. Wait for SPI readiness

   Monitor DRDY and wait for its initial low-to-high transition indicating that the device is ready for SPI communication.

   Do not send configuration commands before this point.
4. Configure the SPI interface

   Configure the ESP32-C5 SPI peripheral for the timing mode specified by TI.

   Use an initial conservative SCLK frequency and verify the communication before increasing it.
5. Configure MODE and CLOCK registers

   Set the required operating mode, power mode, OSR and enabled ADC channels.

   For the recommended 500 SPS configuration:
   - PWR = high-resolution
   - OSR[2:0] = `110`
   - TBM = `0`
   - CH0_EN = `1`
   - CH1_EN = `1`
   There is no external clock selection bit to set.
6. Verify register readback

   Read back CLOCK and MODE.

   Check that the configured values match the expected values. Also inspect STATUS for reported faults.
7. Enable DRDY interrupt

   Configure a falling-edge interrupt on the ESP32-C5 GPIO connected to DRDY.

   Start the acquisition task only after configuration and verification.
8. Start continuous acquisition

   On each DRDY event, retrieve the complete SPI data frame, decode both 24-bit channel values and validate the communication status and CRC where enabled.

TI specifically recommends providing a valid clock as soon as possible after supply ramp-up. The ADC requires an initial clock-dependent startup interval, and its sinc³ filter requires three output data periods to settle. The first samples should therefore be treated appropriately rather than automatically considered fully settled measurements.




### CLOCK register configuration

The relevant CLOCK register fields are:

| Bit(s) | Field    | Setting             |
| ------ | -------- | ------------------- |
| 9      | CH1_EN   | 1                   |
| 8      | CH0_EN   | 1                   |
| 7:6    | Reserved | 00                  |
| 5      | TBM      | 0                   |
| 4:2    | OSR      | 110 for 500 SPS     |
| 1:0    | PWR      | 10, high-resolution |

For both channels enabled, high-resolution operation and OSR 8192, the CLOCK register value is:

CLOCK register, 500 SPS:

```
0x031A = 0000 0011 0001 1010
         CH1_EN = 1, CH0_EN = 1, reserved = 00, TBM = 0, OSR = 110, PWR = 10
```

This is the expected register value assuming the stated configuration and reserved bits written as zero. (0x037A, quoted in an earlier draft, sets bit 6, which is reserved, and bit 5, which is TBM, and is wrong.) The firmware computes the value from the fields: 0x031A at OSR 8192, 0x0316 at OSR 4096.

## 5. ESP32-C5 acquisition architecture

For a seismic station, I would strongly recommend separating data acquisition from data processing.

The ESP32-C5 should not perform expensive calculations inside the DRDY interrupt handler.

Instead, use this architecture:

ADS131M02

Continuous dual-channel conversion

DRDY falling-edge interrupt

Minimal ISR — notify acquisition task

High-priority SPI acquisition task

Read complete frame, validate and decode samples

Ring buffer / DMA buffer

Timestamped, sequential sample storage

Background processing

Filtering, packetization, storage, networking and diagnostics

### Why this matters

At 500 SPS, you have 2 ms between samples. That sounds generous, but the system should still be designed for deterministic acquisition.

I would implement:

- A high-priority acquisition task.
- No Wi-Fi, MQTT or network operations inside the acquisition path.
- A ring buffer large enough to accommodate temporary processing delays.
- A counter for missed DRDY events.
- A counter for SPI CRC errors.
- A counter for ADC status faults.
- A monotonic sample counter.
- DMA-based SPI transfers where supported by the ESP32-C5 SPI driver.

The ADS131M02 produces a fixed-length SPI frame containing status, two channel results and CRC. Your firmware should decode the frame according to the configured word length and CRC settings.


## 6. Seismic timing and timestamp architecture

This is an area I would extend beyond the original design.

For seismic monitoring, it is not sufficient to know that a sample arrived at a particular ESP32 timestamp.

You need to know when the ADC actually acquired that sample.

I recommend maintaining three separate timing concepts:

| Timing source         | Purpose                                           |
| --------------------- | ------------------------------------------------- |
| ADC sample counter    | Exact sample ordering                             |
| ESP32 monotonic timer | Detecting timing anomalies and acquisition delays |
| GPS PPS               | Absolute UTC synchronization                      |

Do not timestamp samples using the time at which the SPI transaction finishes. SPI latency and interrupt scheduling introduce variable delays.

Instead, associate each DRDY event with the corresponding sample period and maintain the ADC sample sequence.

For a 500 SPS configuration:

\\[ T_s = \frac{1}{500}=0.002\text{ seconds} \\]

A sample timestamp can then be reconstructed from a known synchronization epoch:

\\[ t_n=t_0+nT_s \\]

For better long-term accuracy, periodically estimate the actual sampling frequency against the GPS PPS reference rather than assuming that the nominal oscillator frequency is exact.

One further distinction: the GPS PPS can discipline or calibrate your sample-time model, but it does not automatically synchronize the ADC's internal sampling phase. For precise phase alignment, use the ADS131M02 SYNC/RESET synchronization functionality appropriately.


## 7. Analog signal quality and anti-aliasing

There is another important consideration for your seismic station: increasing the ADC sample rate does not automatically improve the measurement.

The complete signal chain matters:

Seismic sensor

Larry's Webtronics preamplifier

Analog anti-aliasing filter

ADS131M02

24-bit conversion + digital filtering

ESP32-C5 acquisition

The ADS131M02 digital filter attenuates out-of-band modulator noise, but it does not replace an appropriately designed analog anti-aliasing filter.

For a 500 SPS seismic station, I would investigate:

- The frequency response of the preamplifier.
- The analog filter cutoff and attenuation.
- The ADC sinc³ filter response.
- The actual sensor bandwidth.
- Noise introduced by the oscillator and digital circuitry.

In particular, do not assume that soil damping makes signals above 200 Hz irrelevant. Instrument response, sensor characteristics and the type of seismic event all influence the useful bandwidth.

## 8. Recommended engineering decisions

Here is how I would finalize the initial hardware and firmware design.

| Component            | Recommendation                                                        |
| -------------------- | --------------------------------------------------------------------- |
| ADC                  | ADS131M02                                                             |
| Master clock         | External 8.192 MHz oscillator                                         |
| Oscillator type      | Low-jitter CMOS oscillator; TCXO if temperature stability warrants it |
| ADC power mode       | High-resolution                                                       |
| Initial OSR          | 8192                                                                  |
| Initial sample rate  | 500 SPS                                                               |
| ADC channels         | Both enabled                                                          |
| Data-ready mechanism | DRDY falling-edge interrupt                                           |
| Processor            | ESP32-C5                                                              |
| SPI                  | Dedicated acquisition task                                            |
| Buffering            | Ring buffer                                                           |
| Error detection      | SPI CRC + ADC STATUS monitoring                                       |
| Time synchronization | GPS PPS                                                               |
| Data transport       | Separate background task                                              |

### TI documentation

The authoritative reference for implementing this is:

- [Texas Instruments ADS131M02 datasheet (SBAS853A)](https://www.ti.com/lit/ds/symlink/ads131m02.pdf) — especially sections 5, 6, 8.3, 8.4, 8.5, 8.6, 9.1 and 11.
- [TI ADS131M02 application example](https://www.alldatasheet.jp/html-pdf/1279776/TI/ADS131M02/2657/74/ADS131M02.html) — useful because it demonstrates an MCU supplying an external 8.192 MHz clock to the ADC.

## Final recommendation

Design recommendation

I would keep the external 8.192 MHz oscillator.

It gives you a well-defined clock source independent of ESP32-C5 firmware scheduling, Wi-Fi activity and other processor workloads.

However, I would make three design decisions before producing the PCB:

1. Use 500 SPS initially. It is a sensible starting point for continuous seismic monitoring, with a 2 ms sample interval.
2. Design for GPS PPS synchronization from the beginning. Retrofitting precise station-to-station timing later can be unnecessarily difficult.
3. Keep the analog and digital sections physically separated. The ADC may offer excellent resolution, but poor PCB layout or clock coupling can compromise the actual noise floor.

One final architectural suggestion: make the firmware support selectable OSR settings (500, 1,000, 2,000 and 4,000 SPS) without requiring a hardware modification. This will let you experiment with actual seismic recordings and choose the optimal acquisition rate after evaluating the complete sensor chain.
