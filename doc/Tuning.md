# EQAMP Tuning

How to set the two trimmers on each channel of the Webtronics EQAMP, and how to get the measured gain into the station (`chN_amp`) and from there into StationXML.

Values here are for the 10 Hz build with JP1 in, as fitted on our board. See the [README](../README.md#signal-chain) for the signal chain.

## The two trimmers

Each channel has two 50k trimmers:

| Silkscreen | Ref | Next to | Sets |
|---|---|---|---|
| Gain | R9 | LT1007 (U1) | LF411 (U2) stage gain, 1 + R9 / 5.1k = x1 to x10.8 |
| Offset | R24 | NE5534 (U4) | DC level at the channel output |

Channel gain with JP1 in is 33.5 x (1 + R9 / 5.1k) x 6.1, about 204 to 2208. Fully counter-clockwise is minimum gain. With JP1 out the first stage is x158 and the range is about 964 to 10400; a geophone doesn't need that.

### Order: gain first, then offset

```
input ── U1 LT1007 ── U2 LF411 (Gain) ── RC + 3 Sallen-Key ── C16/R22 high-pass ── U3D ── RC ── U4 NE5534 (Offset) ── out
          x33.5        x1..x10.8           U3A..U3C              138 s, tau 22 s                    x6.1
```

C16 blocks every DC offset ahead of it. Turning the gain pot can't move the output DC for good, but it does throw a step through the high-pass that takes about 2 minutes (5 tau) to die away. Set the gain, wait, then set the offset. Touching the gain pot again means waiting and checking the offset again.

### Two offset layers

| Layer | Removes | How |
|---|---|---|
| R24, hardware | Amplifier offset (U3D, U4) | Trimmer, inputs quiet |
| `chN_ocal`, firmware | ADC offset only | `cmd=zero` with the channel on `mux=short` |

Run `zero` on the shorted ADC input, not on the amplifier signal. Otherwise OCAL also absorbs the amplifier offset, and its temperature drift stops being visible in the data.

## Tools

- DMM with a DC range good to 1 mV
- Function generator that goes down to 1 Hz (a DDS unit). Phone and sound-card generators are AC coupled and don't reach 1 Hz
- Resistors for an input divider: 100k and 100 ohm, 1 % or better
- A shorting link for the amplifier inputs
- The station running, its web page or `curl`

Disconnect the geophones for all of this. A connected geophone is part of the input network and records the bench being bumped.

## Plain English walkthrough (with a generator)

The short version of Steps B and C. Do Step A (ADC zero) once beforehand. One channel at a time.

### Before you start

- Unplug the geophones from the amplifier.
- Power everything on and let it warm up for 5 minutes.
- Open the station's web page so you can see the numbers.

### 1. Build a small divider

The generator can't make a signal small enough on its own, so two resistors shrink it about 1000 times:

- a 100k resistor from the generator output to the amplifier input
- a 100 ohm resistor from the amplifier input to ground

1 volt from the generator becomes about 1 millivolt at the amplifier, roughly what a geophone produces.

### 2. Set the generator

1. Sine wave, 50 Hz.
2. Multimeter on AC volts across the generator output (before the resistors). Turn the amplitude until it reads 1.000 V.
3. Change the frequency to 1 Hz. Leave the amplitude alone.

A multimeter can't read a 1 Hz signal properly, so set the level where the meter is accurate, then change the frequency. The generator keeps the same level.

### 3. Set the gain

1. Turn the Gain trimmer (near the LT1007) all the way counter-clockwise. That's the lowest gain.
2. Watch "mV at the amplifier" for that channel on the web page.
3. Turn the Gain trimmer slowly clockwise until it reads about 989 mV. That's a gain of 1000.
4. Let it sit for a few seconds and note the average reading.

### 4. Tell the station the gain

Divide the reading by 0.989. If the page settles at 1003 mV, the gain is 1003 / 0.989 = 1014. Enter it as the channel's gain in the settings panel: `ch0_amp` for the vertical, `ch1_amp` for the horizontal.

### 5. Set the offset

1. Disconnect the generator and short the amplifier input to ground with a wire.
2. Wait 2 to 3 minutes. The amplifier needs that long to settle after the gain trimmer moved.
3. Multimeter on DC volts at the channel output. Turn the Offset trimmer (near the NE5534) until it reads close to 0, within 5 millivolts either way.

### 6. Other channel, then write it down

Repeat for the second channel and keep a record: trimmer position, reading, gain, offset, date and temperature.

### Rules to remember

- Gain first, then offset. Moving the gain trimmer upsets the offset for a couple of minutes.
- Touched a trimmer later? Measure again and update the station's gain number.
- Every gain number change means a new StationXML file.

## Step A: ADC zero

Per channel, N = 0 and 1:

```
curl -d cmd=mux -d ch=N -d mode=short http://vseismo/api/acq/cmd
# wait a few seconds, check /api/acq: mux "short", mean steady
curl -d cmd=zero -d ch=N http://vseismo/api/acq/cmd
curl -d cmd=mux -d ch=N -d mode=normal http://vseismo/api/acq/cmd
```

After `zero` the shorted mean should be near 0. If it doubles instead, the OCAL sign is wrong; see [API.md](API.md#bench-commands).

## Step B: gain

The site file defines the amplifier gain at 1 Hz (`passband_hz: 1.0`). At 1 Hz the 10 Hz build is flat to well under 0.1 dB, and one period fills exactly one of the station's 1 s statistics windows.

1. Divider at the channel input: 100k in series from the generator, 100 ohm from the input to ground. With the board's own 10k input in parallel the shunt is 99.0 ohm, so the ratio is 99.0 / 100099 = 1 / 1011.
2. Set the generator to 1.000 V rms. A DMM can't read 1 Hz AC reliably: set the level at 50 Hz on the meter, then switch the generator to 1 Hz. A DDS generator's amplitude doesn't change with frequency. Input is now 0.989 mV rms.
3. Start with the Gain pot fully counter-clockwise (about x204), then turn it until `amp_mv` in `/api/acq` (rms at the EQAMP output, AC) reads the target. For gain 1000 that's 989 mV.
4. Read `amp_mv` over several seconds and average. Gain = `amp_mv` / 0.989.
5. Set it: `curl -d ch0_amp=<gain> http://vseismo/api/config` (scaling only, takes effect immediately).
6. Repeat on the other channel. The same tone into both channels at once also shows their phase match.

`amp_mv` uses `chN_div`, default 1 / 10.1 from the 0.1 % divider resistors. At the levels above the ADC sees about 98 mV rms, far from clipping.

### Without a generator: DC step

Rougher, but needs only an AA cell and the same divider. Measure the cell with the DMM (DC is accurate), switch it onto the divider, and read the step height in the output samples (`/api/wave`) 0.5 to 1 s after the edge. The 10 Hz filter has settled by then. The high-pass has drooped by e^(-t / 22 s), 2 to 4 %: multiply the reading by e^(t / 22). Average the on and the off edges. Expect a few percent; use the tone for the final value.

## Step C: offset

1. Wait 2 to 3 minutes after power-on or the last touch of the Gain pot.
2. Short the channel input (generator and divider off).
3. Turn the Offset pot until the output reads 0 within about 5 mV. Either:
   - a DMM on the channel output, or
   - the station: offset at the output in mV = `mean` x `lsb_uv` / `div` / 1000, from `/api/acq`. With PGA 1 one count is 0.143 uV at the ADC, so 5 mV at the output is about 3500 counts.

5 mV out of +-10 V costs nothing in headroom. Don't chase the last millivolt; it drifts with temperature.

## Choosing the gain

Clip points with the L-15B at 28.8 V/(m/s) and +-10 V out, in the passband:

| Gain | Input clip, peak | Ground velocity clip, peak |
|---|---|---|
| 204 (min) | 49 mV | 1.7 mm/s |
| 500 | 20 mV | 0.69 mm/s |
| 1000 | 10 mV | 0.35 mm/s |
| 2208 (max) | 4.5 mV | 0.16 mm/s |

Start at 1000. After the noise survey at the site, raise it only if the quiet-night background at the ADC sits less than about 10 dB above the shorted-input floor (Step A, noise spectrum on the web page). Lower it if local events you care about clip.

Keep `chN_pga` at 1. Any PGA gain makes the ADC clip before the amplifier does: PGA 2 caps the amplifier output at about 6 V.

## When to recheck

- Any touch of a trimmer: new gain measurement, new `chN_amp`, new StationXML (change `site.start`, keep the old file)
- Offset in an unheated enclosure: at the turn of each season. The gain doesn't drift measurably; the offset does
- After replacing an op-amp or any filter part
