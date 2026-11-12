# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Vlad Patoka (VE3VPT)
"""Build FDSN StationXML for a vseismo station.

The response chain per channel, ground velocity to counts:

  1. geophone        PZ  M/S -> V   2 zeros at 0, poles from f0 and damping
  2. amplifier       PZ  V -> V     EQAMP: 1-pole high-pass, 8-pole low-pass
  3. divider         PZ  V -> V     resistor divider with its RC pole
  4. ADC             V -> COUNTS    2^23 / (1.2 V / PGA) x gcal
  5. sinc3           COUNTS         modulator rate / OSR, modelled as unity
  6. FIR             COUNTS         the firmware's Blackman windowed sinc

Stage gains come from the station (/api/config: div, amp, sens, pga, gcal)
so the file agrees with the tuning page. Filter shapes come from the site
file (component values). Coordinates are always rounded to 0.01 degree.

The firmware stamps each sample with the time the ground moved: it already
subtracts the sinc3 and FIR group delay. Every decimation stage therefore
carries correction == delay, so readers don't shift the times again.
"""

import argparse
import cmath
import json
import math
import sys
import urllib.request

import yaml
from obspy import UTCDateTime
from obspy.core.inventory import (Channel, Equipment, Inventory, Network, Site,
                                  Station)
from obspy.core.inventory.response import (CoefficientsTypeResponseStage,
                                           FIRResponseStage,
                                           InstrumentSensitivity,
                                           PolesZerosResponseStage, Response)
from obspy.core.inventory.util import Comment

ADC_VREF = 1.2              # ADS131M02 internal reference, volts
ADC_FS_COUNTS = 2 ** 23     # 24-bit two's complement
SINC3_DELAY_PERIODS = 1.5   # ADS131M02 sinc3 group delay, in output periods
FIR_MAX = 255


# ── FIR: a copy of fir_design() in main/acq.c ──────────────────────────────

def fir_taps(taps, decim):
    """Tap count the firmware ends up with (taps 0 = automatic)."""
    if decim <= 1:
        return 1
    if taps <= 0:
        taps = min(20 * decim + 1, FIR_MAX)
    taps = min(taps, FIR_MAX)
    if not taps & 1:
        taps -= 1
    return max(taps, 3)


def fir_coefficients(taps, decim):
    """Unity-DC-gain Blackman windowed sinc, cutoff 0.4 x the output rate."""
    taps = fir_taps(taps, decim)
    if taps == 1:
        return [1.0]
    fc = 0.4 / decim
    m = taps - 1
    h = []
    for n in range(taps):
        x = n - m / 2.0
        sinc = 2.0 * fc if x == 0 else math.sin(2.0 * math.pi * fc * x) / (math.pi * x)
        w = 0.42 - 0.5 * math.cos(2.0 * math.pi * n / m) + 0.08 * math.cos(4.0 * math.pi * n / m)
        h.append(sinc * w)
    s = sum(h)
    # The firmware rounds to Q30; keep the same values so the file matches it.
    return [round(v / s * (1 << 30)) / (1 << 30) for v in h]


# ── Analog stages as poles and zeros (rad/s) ───────────────────────────────

def num(v):
    """YAML 1.1 reads 33.0e3 (no exponent sign) as a string: accept both."""
    return float(v)


def geophone_pz(f0, damping):
    w0 = 2 * math.pi * num(f0)
    damping = num(damping)
    d = cmath.sqrt(damping ** 2 - 1)
    return [0j, 0j], [w0 * (-damping + d), w0 * (-damping - d)]


def sallen_key_poles(r1, r2, c_fb, c_gnd):
    """Unity-gain Sallen-Key low-pass: c_fb from the R junction to the output,
    c_gnd from the + input to ground."""
    w0 = 1.0 / math.sqrt(r1 * r2 * c_fb * c_gnd)
    q = math.sqrt(r1 * r2 * c_fb * c_gnd) / (c_gnd * (r1 + r2))
    d = cmath.sqrt(1 - 4 * q * q)
    return [w0 / (2 * q) * (-1 + d), w0 / (2 * q) * (-1 - d)]


def amplifier_pz(amp):
    zeros, poles = [], []
    for hp in amp.get("highpass", []):
        zeros.append(0j)
        poles.append(complex(-1.0 / (num(hp["r"]) * num(hp["c"])), 0))
    for rc in amp.get("rc_lowpass", []):
        poles.append(complex(-1.0 / (num(rc["r"]) * num(rc["c"])), 0))
    for sk in amp.get("sallen_key", []):
        poles += sallen_key_poles(*(num(sk[k]) for k in ("r1", "r2", "c_fb", "c_gnd")))
    return zeros, poles


def divider_pz(div):
    top, bot = num(div["r_top"]), num(div["r_bottom"])
    return [], [complex(-1.0 / (top * bot / (top + bot) * num(div["c"])), 0)]


def pz_eval(zeros, poles, f):
    s = 2j * math.pi * f
    num = 1 + 0j
    for z in zeros:
        num *= s - z
    den = 1 + 0j
    for p in poles:
        den *= s - p
    return num / den


def shape_gain(zeros, poles, f, f_hi):
    """|H(f)| / |H(f_hi)|: the stage's gain at f relative to its passband
    (for the geophone, the high-frequency asymptote)."""
    return abs(pz_eval(zeros, poles, f)) / abs(pz_eval(zeros, poles, f_hi))


def pz_stage(seq, name, zeros, poles, passband_gain, fn, f_hi, in_u, out_u, in_d, out_d):
    """passband_gain is the plain gain (V/(m/s), V/V); the stage gain is
    quoted at fn, where the shape may already droop."""
    a0 = 1.0 / abs(pz_eval(zeros, poles, fn))
    return PolesZerosResponseStage(
        stage_sequence_number=seq, name=name,
        stage_gain=passband_gain * shape_gain(zeros, poles, fn, f_hi), stage_gain_frequency=fn,
        input_units=in_u, output_units=out_u,
        input_units_description=in_d, output_units_description=out_d,
        pz_transfer_function_type="LAPLACE (RADIANS/SECOND)",
        normalization_frequency=fn, normalization_factor=a0,
        zeros=zeros, poles=poles)


# ── Inputs ─────────────────────────────────────────────────────────────────

def fetch_json(url):
    with urllib.request.urlopen(url, timeout=10) as r:
        return json.load(r)


def load_station(args):
    if args.config_json:
        with open(args.config_json) as f:
            data = json.load(f)
        return data.get("config", data), data.get("acq")
    base = args.station_url.rstrip("/")
    return fetch_json(base + "/api/config"), fetch_json(base + "/api/acq")


def rates(cfg, acq):
    fs_in = cfg["clk_hz"] / 2.0 / cfg["osr"]
    if acq and acq.get("fs_in"):
        fs_in = float(acq["fs_in"])        # the clock actually set (LEDC may differ)
    decim = max(1, int(cfg["decim"]))
    return fs_in * cfg["osr"], fs_in, fs_in / decim, decim


# ── Build ──────────────────────────────────────────────────────────────────

class NotCalibrated(Exception):
    pass


def channel_response(cfg_ch, cfg, acq, site, draft):
    sensor, amp, div = site["sensor"], site["amplifier"], site["divider"]
    fn = float(site.get("normalization_hz", 10.0))
    sens = float(cfg_ch.get("sens") or 0)
    gain = float(cfg_ch.get("amp") or 0)
    if sens <= 0 or gain <= 0:
        if not draft:
            raise NotCalibrated(f"channel {cfg_ch.get('name')}: amp={gain} sens={sens} on the station")
        sens = sens if sens > 0 else float(sensor["sensitivity_nominal"])
        gain = gain if gain > 0 else float(amp["gain_nominal"])
    ratio = float(cfg_ch.get("div") or num(div["r_bottom"]) / (num(div["r_top"]) + num(div["r_bottom"])))
    pga = int(cfg_ch.get("pga") or 1)
    gcal = float(cfg_ch.get("gcal") or 1.0)

    fmod, fs_in, fs_out, decim = rates(cfg, acq)
    stages = []

    # Each stage's plain gain holds where its shape is flat: the geophone far
    # above its corner, the amplifier mid-band, the divider at DC.
    z, p = geophone_pz(sensor["natural_hz"], sensor["damping"])
    stages.append(pz_stage(1, "geophone", z, p, sens, fn, 1000.0,
                           "M/S", "V", "Velocity in meters per second", "Volts"))

    z, p = amplifier_pz(amp)
    stages.append(pz_stage(2, "amplifier", z, p, gain, fn, float(amp.get("passband_hz", 1.0)),
                           "V", "V", "Volts", "Volts"))

    z, p = divider_pz(div)
    stages.append(pz_stage(3, "divider", z, p, ratio, fn, 0.0001,
                           "V", "V", "Volts", "Volts"))

    counts_per_v = ADC_FS_COUNTS / (ADC_VREF / pga) * gcal
    stages.append(CoefficientsTypeResponseStage(
        stage_sequence_number=4, name="ADC", stage_gain=counts_per_v, stage_gain_frequency=fn,
        input_units="V", output_units="COUNTS",
        input_units_description="Volts", output_units_description="Digital counts",
        cf_transfer_function_type="DIGITAL", numerator=[], denominator=[],
        decimation_input_sample_rate=fmod, decimation_factor=1,
        decimation_offset=0, decimation_delay=0.0, decimation_correction=0.0))

    sinc_delay = SINC3_DELAY_PERIODS / fs_in
    stages.append(CoefficientsTypeResponseStage(
        stage_sequence_number=5, name="sinc3",
        stage_gain=1.0, stage_gain_frequency=fn,
        input_units="COUNTS", output_units="COUNTS",
        input_units_description="Digital counts", output_units_description="Digital counts",
        cf_transfer_function_type="DIGITAL", numerator=[1.0], denominator=[],
        decimation_input_sample_rate=fmod, decimation_factor=int(cfg["osr"]),
        decimation_offset=0, decimation_delay=sinc_delay, decimation_correction=sinc_delay))

    coef = fir_coefficients(int(cfg.get("taps") or 0), decim)
    fir_delay = (len(coef) - 1) / 2.0 / fs_in
    stages.append(FIRResponseStage(
        stage_sequence_number=6, name="FIR",
        stage_gain=1.0, stage_gain_frequency=fn,
        input_units="COUNTS", output_units="COUNTS",
        input_units_description="Digital counts", output_units_description="Digital counts",
        symmetry="NONE", coefficients=coef,
        decimation_input_sample_rate=fs_in, decimation_factor=decim,
        decimation_offset=0, decimation_delay=fir_delay, decimation_correction=fir_delay))

    resp = Response(
        instrument_sensitivity=InstrumentSensitivity(
            value=1.0, frequency=fn, input_units="M/S", output_units="COUNTS",
            input_units_description="Velocity in meters per second",
            output_units_description="Digital counts"),
        response_stages=stages)
    resp.recalculate_overall_sensitivity(fn)
    return resp, fs_out, sinc_delay + fir_delay


def build(cfg, acq, site, draft=False, created=None):
    net_code = site.get("network") or cfg.get("network") or "XX"
    sta_code = site.get("station") or cfg.get("station") or "VSEIS"
    loc = site.get("location", "00")
    s = site["site"]
    lat = round(float(s["latitude"]), 2)
    lon = round(float(s["longitude"]), 2)
    elev = round(float(s.get("elevation_m", 0.0)))
    start = UTCDateTime(s["start"])

    channels, report = [], []
    for ch in site["channels"]:
        cfg_ch = cfg["ch"][ch["adc_channel"]]
        resp, fs_out, delay = channel_response(cfg_ch, cfg, acq, site, draft)
        az = ch.get("azimuth")
        channels.append(Channel(
            code=ch["code"], location_code=loc, latitude=lat, longitude=lon,
            elevation=elev, depth=float(s.get("depth_m", 0.0)),
            azimuth=float(az) if az is not None else 0.0, dip=float(ch["dip"]),
            sample_rate=fs_out, start_date=start,
            types=["CONTINUOUS", "GEOPHYSICAL"],
            sensor=Equipment(type="Geophone", description=site["sensor"]["description"]),
            data_logger=Equipment(type="Digitizer", description=site.get("datalogger", "vseismo ESP32-C5 + ADS131M02")),
            pre_amplifier=Equipment(type="Amplifier", description=site["amplifier"]["description"]),
            response=resp,
            comments=[]))
        if az is None:
            channels[-1].comments.append(Comment("Azimuth not yet measured; 0 is a placeholder"))
        report.append((ch["code"], resp.instrument_sensitivity.value, fs_out, delay))

    station = Station(
        code=sta_code, latitude=lat, longitude=lon, elevation=elev,
        site=Site(name=s["name"]), start_date=start, creation_date=start,
        channels=channels)
    if draft:
        station.comments.append(Comment(
            "DRAFT: nominal amplifier gain and/or geophone sensitivity, not measured"))
    inv = Inventory(
        networks=[Network(code=net_code, stations=[station], start_date=start)],
        source=site.get("source", "vseismo"),
        sender=site.get("sender"),
        module="vseismo make_stationxml.py",
        created=UTCDateTime(created) if created else UTCDateTime())
    return inv, report


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--station-url", help="e.g. http://192.168.1.50")
    src.add_argument("--config-json", help="saved {'config': /api/config, 'acq': /api/acq} instead of a live station")
    ap.add_argument("--site", required=True, help="site YAML (see site.example.yaml)")
    ap.add_argument("-o", "--output", required=True, help="StationXML file to write")
    ap.add_argument("--plot", help="also write a response plot (PNG)")
    ap.add_argument("--draft", action="store_true",
                    help="use the site file's nominal gain and sensitivity where the station has 0")
    args = ap.parse_args(argv)

    with open(args.site) as f:
        site = yaml.safe_load(f)
    for k in ("latitude", "longitude"):
        v = str(site["site"][k])
        if "." in v and len(v.split(".")[1]) > 2:
            print(f"note: {k} rounded to 0.01 degree ({float(v):.2f})", file=sys.stderr)

    cfg, acq = load_station(args)
    try:
        inv, report = build(cfg, acq, site, draft=args.draft)
    except NotCalibrated as e:
        sys.exit(f"{e}. Set chN_amp and chN_sens on the station, or pass --draft.")

    inv.write(args.output, format="STATIONXML", validate=True)
    for code, sens, fs, delay in report:
        print(f"{code}: {sens:.4g} counts/(m/s) at {site.get('normalization_hz', 10.0)} Hz, "
              f"{fs:g} SPS, filter delay {delay * 1e3:.2f} ms (already removed from the timestamps)")
    print(f"wrote {args.output}{' (DRAFT)' if args.draft else ''}")
    if args.plot:
        import matplotlib
        matplotlib.use("Agg")
        inv.plot_response(min_freq=0.005, output="VEL", outfile=args.plot)
        print(f"wrote {args.plot}")


if __name__ == "__main__":
    main()
