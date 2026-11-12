# SPDX-License-Identifier: MIT
"""Checks for make_stationxml.py. Run: python -m unittest -v test_make_stationxml"""

import copy
import io
import math
import os
import tempfile
import unittest

import yaml
from obspy import read_inventory

import make_stationxml as mk

HERE = os.path.dirname(os.path.abspath(__file__))

CFG = {
    "ok": True, "api": 1, "station": "VSEIS", "network": "XX",
    "clk_src": "ext", "clk_hz": 8192000, "osr": 4096, "decim": 10, "taps": 0,
    "ch": [
        {"name": "Z", "pga": 1, "phase": 0, "gcal": 1.0, "ocal": 0, "div": 1 / 10.1, "amp": 1000, "sens": 28.8},
        {"name": "H", "pga": 1, "phase": 0, "gcal": 1.0, "ocal": 0, "div": 1 / 10.1, "amp": 1000, "sens": 28.8},
    ],
}
ACQ = {"fs_in": 1000.0, "fs_out": 100.0}


def site():
    with open(os.path.join(HERE, "site.example.yaml")) as f:
        s = yaml.safe_load(f)
    s["site"]["latitude"] = 12.345678
    s["site"]["longitude"] = -98.765432
    return s


def db(x):
    return 20 * math.log10(x)


class Fir(unittest.TestCase):
    def test_taps_follow_the_firmware(self):
        self.assertEqual(mk.fir_taps(0, 10), 201)
        self.assertEqual(mk.fir_taps(0, 18), 255)     # 361 capped at FIR_MAX
        self.assertEqual(mk.fir_taps(200, 10), 199)   # even -> one less
        self.assertEqual(mk.fir_taps(0, 1), 1)

    def test_unity_dc_symmetric(self):
        h = mk.fir_coefficients(0, 10)
        self.assertEqual(len(h), 201)
        self.assertAlmostEqual(sum(h), 1.0, places=6)
        for a, b in zip(h, reversed(h)):
            self.assertEqual(a, b)

    def test_q30_grid(self):
        for v in mk.fir_coefficients(0, 10):
            self.assertEqual(v * (1 << 30), round(v * (1 << 30)))


class Analog(unittest.TestCase):
    def test_eqamp_matches_the_schematic(self):
        z, p = mk.amplifier_pz(site()["amplifier"])
        self.assertEqual((len(z), len(p)), (1, 9))     # 1 high-pass + 8 low-pass poles
        ref = abs(mk.pz_eval(z, p, 1.0))
        rel = lambda f: abs(mk.pz_eval(z, p, f)) / ref
        self.assertAlmostEqual(db(rel(0.0479)), -3.0, delta=0.3)    # 20 s high-pass
        f3 = next(f / 100 for f in range(1000, 3000) if db(rel(f / 100)) < -3.0)
        self.assertTrue(17.5 < f3 < 19.0, f3)                       # the "18 Hz" board
        self.assertLess(db(rel(30.0)), -25.0)

    def test_geophone_flat_above_corner(self):
        z, p = mk.geophone_pz(4.5, 0.6)
        self.assertAlmostEqual(mk.shape_gain(z, p, 100.0, 1000.0), 1.0, places=2)
        self.assertLess(mk.shape_gain(z, p, 1.0, 1000.0), 0.06)


class Build(unittest.TestCase):
    def test_sensitivity_is_the_chain_product(self):
        s = site()
        inv, report = mk.build(CFG, ACQ, s, created="2026-01-01")
        ch = inv[0][0][0]
        fn = s["normalization_hz"]
        zg, pg = mk.geophone_pz(4.5, 0.6)
        za, pa = mk.amplifier_pz(s["amplifier"])
        zd, pd = mk.divider_pz(s["divider"])
        want = (28.8 * mk.shape_gain(zg, pg, fn, 1000.0)
                * 1000 * mk.shape_gain(za, pa, fn, 1.0)
                * (1 / 10.1) * mk.shape_gain(zd, pd, fn, 0.0001)
                * 2 ** 23 / 1.2)
        got = ch.response.instrument_sensitivity.value
        self.assertAlmostEqual(got / want, 1.0, places=3)
        # and ObsPy's own evaluation of the full response agrees
        self.assertAlmostEqual(abs(ch.response.get_evalresp_response_for_frequencies([fn], output="VEL")[0]) / want,
                               1.0, places=2)

    def test_corrections_equal_delays(self):
        inv, report = mk.build(CFG, ACQ, site(), created="2026-01-01")
        total = 0.0
        for st in inv[0][0][0].response.response_stages:
            if st.decimation_factor is not None:
                self.assertEqual(st.decimation_delay, st.decimation_correction)
                total += st.decimation_delay
        self.assertAlmostEqual(total, 0.1015, places=6)    # matches the firmware's group delay
        self.assertAlmostEqual(report[0][3], 0.1015, places=6)

    def test_rates_follow_the_clock(self):
        inv, _ = mk.build(CFG, ACQ, site(), created="2026-01-01")
        self.assertEqual(inv[0][0][0].sample_rate, 100.0)
        ledc = dict(CFG, clk_hz=8000000)
        inv, _ = mk.build(ledc, {"fs_in": 976.5625}, site(), created="2026-01-01")
        self.assertAlmostEqual(inv[0][0][0].sample_rate, 97.65625)

    def test_coordinates_rounded(self):
        inv, _ = mk.build(CFG, ACQ, site(), created="2026-01-01")
        sta = inv[0][0]
        self.assertEqual((sta.latitude, sta.longitude), (12.35, -98.77))
        for ch in sta:
            self.assertEqual((ch.latitude, ch.longitude), (12.35, -98.77))

    def test_uncalibrated_refused_unless_draft(self):
        raw = copy.deepcopy(CFG)
        for c in raw["ch"]:
            c["amp"] = 0
            c["sens"] = 0
        with self.assertRaises(mk.NotCalibrated):
            mk.build(raw, ACQ, site(), created="2026-01-01")
        inv, _ = mk.build(raw, ACQ, site(), draft=True, created="2026-01-01")
        self.assertTrue(any("DRAFT" in c.value for c in inv[0][0].comments))

    def test_unmeasured_azimuth_is_marked(self):
        inv, _ = mk.build(CFG, ACQ, site(), created="2026-01-01")
        eh1 = inv[0][0].select(channel="EH1")[0]
        self.assertTrue(any("Azimuth" in c.value for c in eh1.comments))
        self.assertIsNone(eh1.azimuth)
        self.assertEqual(inv[0][0].select(channel="EHZ")[0].azimuth, 0.0)

    def test_unmeasured_azimuth_absent_from_xml(self):
        inv, _ = mk.build(CFG, ACQ, site(), created="2026-01-01")
        buf = io.BytesIO()
        inv.write(buf, format="STATIONXML", validate=True)
        back = read_inventory(io.BytesIO(buf.getvalue()))
        self.assertIsNone(back.select(channel="EH1")[0][0][0].azimuth)
        self.assertEqual(back.select(channel="EHZ")[0][0][0].azimuth, 0.0)

    def test_no_operator_by_default(self):
        inv, _ = mk.build(CFG, ACQ, site(), created="2026-01-01")
        self.assertEqual(inv[0][0].operators, [])

    def test_operator_and_website_survive_xml(self):
        s = site()
        s["operator"] = {"agency": "Test Operator", "website": "https://www.example.org",
                         "contact": {"name": "A. Person"}}
        inv, _ = mk.build(CFG, ACQ, s, created="2026-01-01")
        buf = io.BytesIO()
        inv.write(buf, format="STATIONXML", validate=True)
        op = read_inventory(io.BytesIO(buf.getvalue()))[0][0].operators[0]
        self.assertEqual((op.agency, op.website), ("Test Operator", "https://www.example.org"))
        self.assertEqual(op.contacts[0].names, ["A. Person"])
        self.assertEqual(op.contacts[0].emails, [])

    def test_operator_needs_agency(self):
        s = site()
        s["operator"] = {"website": "https://www.example.org"}
        with self.assertRaises(ValueError):
            mk.build(CFG, ACQ, s, created="2026-01-01")

    def test_writes_valid_stationxml(self):
        inv, _ = mk.build(CFG, ACQ, site(), created="2026-01-01")
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "x.xml")
            inv.write(path, format="STATIONXML", validate=True)
            back = read_inventory(path)
        self.assertEqual(sorted(back.get_contents()["channels"]), ["XX.VSEIS.00.EH1", "XX.VSEIS.00.EHZ"])


def closed_port():
    """A local TCP port nothing listens on: connecting fails at once."""
    import socket
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Errors(unittest.TestCase):
    def run_main(self, argv):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(SystemExit) as cm:
                mk.main(argv + ["-o", os.path.join(d, "x.xml")])
        return str(cm.exception.code)

    def test_unreachable_station_is_a_message(self):
        url = f"http://127.0.0.1:{closed_port()}"
        msg = self.run_main(["--station-url", url, "--site", os.path.join(HERE, "site.example.yaml")])
        self.assertIn(f"station {url} unreachable (/api/config)", msg)
        self.assertIn("/api/status", msg)

    def test_missing_site_file_is_a_message(self):
        msg = self.run_main(["--config-json", "unused.json", "--site", "/nonexistent/site.yaml"])
        self.assertIn("cannot read site file /nonexistent/site.yaml", msg)

    def test_missing_config_json_is_a_message(self):
        msg = self.run_main(["--config-json", "/nonexistent/cfg.json",
                             "--site", os.path.join(HERE, "site.example.yaml")])
        self.assertIn("cannot read station config /nonexistent/cfg.json", msg)


if __name__ == "__main__":
    unittest.main()
