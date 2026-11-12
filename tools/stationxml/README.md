# StationXML generator

Builds the FDSN StationXML file for a vseismo station: where it is, what its channels are, and the full instrument response from ground velocity to counts. Receiving networks and tools such as ObsPy and SeisComP need it to turn counts into m/s.

The script reads the station's live settings (`/api/config`, `/api/acq`), so the gains match the tuning page. A site file supplies the rest: coordinates, channel orientation, and the component values of the analog filters.

## Response chain

| Stage | Model | From |
|---|---|---|
| 1. Geophone | 2 zeros at 0, 2 poles from natural frequency and damping | site file (`sensor`), gain from the station's `chN_sens` |
| 2. Amplifier | EQAMP: 1-pole high-pass at 0.048 Hz, 8-pole low-pass (2 RC poles at 21.9 Hz, 3 Sallen-Key pairs near 20 Hz), -3 dB at about 18 Hz | site file (`amplifier`, from the schematic), gain from `chN_amp` |
| 3. Divider | 9.1k/1.0k with 47 nF: one pole at about 3.8 kHz | site file (`divider`), ratio from `chN_div` |
| 4. ADC | 2^23 / (1.2 V / PGA) x gcal counts per volt | `chN_pga`, `chN_gcal` |
| 5. sinc3 | modulator rate / OSR, unity gain (droop under 0.1 dB below 40 Hz) | `clk_hz`, `osr` |
| 6. FIR | the firmware's 201-tap Blackman windowed sinc, decimate by 10 | same formula as `fir_design()` in `main/acq.c`, Q30-rounded |

The firmware already subtracts the filter group delay from every timestamp (101.5 ms at 100 SPS), so each decimation stage carries `correction = delay`. Readers then leave the times alone.

Coordinates are always rounded to 0.01 degree (about 1 km).

## Run

Build once (the tests run during the build):

```sh
docker build -t vseismo/stationxml tools/stationxml
```

Copy `site.example.yaml` somewhere outside the repo, fill it in, then:

```sh
docker run --rm --network host -v "$PWD":/work vseismo/stationxml \
    --station-url http://192.168.1.50 --site site.yaml -o XX.VSEIS.xml --plot XX.VSEIS.png
```

```
EHZ: 1.926e+10 counts/(m/s) at 10.0 Hz, 100 SPS, filter delay 101.50 ms (already removed from the timestamps)
EH1: 1.926e+10 counts/(m/s) at 10.0 Hz, 100 SPS, filter delay 101.50 ms (already removed from the timestamps)
wrote XX.VSEIS.xml
wrote XX.VSEIS.png
```

The script refuses to run while `chN_amp` or `chN_sens` is 0 on the station, because the file would claim a sensitivity nobody measured. `--draft` uses the site file's `gain_nominal` and `sensitivity_nominal` instead and marks the station "DRAFT" in the file. That's fine for checking the shape, but don't publish it.

Offline, `--config-json` takes a saved `{"config": <GET /api/config>, "acq": <GET /api/acq>}` in place of a live station.

The output is validated against the FDSN StationXML schema before it is written. Check the plot: flat from about 5 to 18 Hz, falling 12 dB per octave below the 4.5 Hz geophone corner, and steeply above 18 Hz.

## When to make a new file

StationXML is versioned by date. Change `site.start` and regenerate when anything in the chain changes: amplifier gain, geophone, orientation, location, sample rate. Keep the old file: data recorded before the change still needs it.

## Files

| File | |
|---|---|
| `make_stationxml.py` | The generator |
| `site.example.yaml` | Site file template, EQAMP geophone-configuration values from the "Chan1-18Hz" schematic |
| `test_make_stationxml.py` | FIR matches the firmware, EQAMP model matches the schematic, sensitivity equals the chain product, corrections equal delays, coordinates rounded, schema-valid output |
| `Dockerfile`, `requirements.txt` | ObsPy 1.4.2 image |
