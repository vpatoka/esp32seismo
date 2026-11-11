# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Vlad Patoka (VE3VPT)
"""Bridge tests against a model of the station ring and a recording DataLink.

Run: python -m unittest -v test_bridge
"""

import json
import os
import random
import shutil
import tempfile
import unittest

from pymseed import MS3Record

import bridge

FS = 100.0
T0_US = 1_794_240_000_000_000          # 2026-11-09, whole second
PERIOD_US = 10_000


class FakeStation:
    """The firmware's /api/wave rules: order=oldest, at most 1000 samples,
    one reply ends at an epoch, tq or time-grid break."""

    def __init__(self, ring_len=131072):
        self.boot = 1111
        self.ring_len = ring_len
        self.s = []                     # dicts with seq, t_us, epoch, tq, flags, v
        self.base = 0                   # seq of s[0]
        self.t_us = T0_US
        self.epoch = 1
        self.tq = "pps"
        self.down = False

    def push(self, n, flags=0, jump_us=0):
        self.t_us += jump_us
        for _ in range(n):
            seq = self.base + len(self.s)
            self.s.append({"seq": seq, "t_us": self.t_us, "epoch": self.epoch, "tq": self.tq,
                           "flags": flags,
                           "v": (random.randint(-5000, 5000), random.randint(-5000, 5000))})
            self.t_us += PERIOD_US
        while len(self.s) > self.ring_len:
            self.s.pop(0)
            self.base += 1

    def reboot(self):
        self.boot += 1
        self.s, self.base, self.epoch = [], 0, 1

    def get(self, path):
        if self.down:
            raise OSError("station down")
        return {"hw": {}, "ref": {}, "time": {}}

    def wave(self, since):
        if self.down:
            raise OSError("station down")
        head = self.base + len(self.s)
        if since < self.base or since > head:
            since = self.base
        end = min(head, since + bridge.WAVE_MAX)
        win = self.s[since - self.base:end - self.base]
        for i in range(1, len(win)):
            a, b = win[i - 1], win[i]
            if (a["epoch"], a["tq"]) != (b["epoch"], b["tq"]) or \
                    abs(b["t_us"] - a["t_us"] - PERIOD_US) > PERIOD_US // 2:
                win = win[:i]
                break
        nxt = since + len(win)
        first = win[0] if win else None
        no_time = first is None or first["tq"] == "none"
        return {"ok": True, "next": nxt, "seq0": since, "n": len(win), "pending": head - nxt,
                "fs": FS, "t0": 0 if no_time else first["t_us"] // 1000,
                "tq": first["tq"] if first else "none",
                "t0_us": 0 if no_time else first["t_us"],
                "epoch": first["epoch"] if first else self.epoch, "boot": self.boot,
                "ch0": [x["v"][0] for x in win], "ch1": [x["v"][1] for x in win],
                "flags": [x["flags"] for x in win]}


class FakeDataLink:
    def __init__(self):
        self.recs = []
        self.fail = 0                   # fail this many writes

    def write(self, streamid, start_us, end_us, record):
        if self.fail:
            self.fail -= 1
            raise OSError("ringserver down")
        self.recs.append((streamid, start_us, end_us, record))

    def close(self):
        pass


def read_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def decode(recs, streamid):
    out = []
    for sid, start_us, end_us, raw in recs:
        if sid != streamid:
            continue
        m = next(MS3Record.from_buffer(raw, unpack_data=True))
        assert len(raw) == 512
        assert start_us == m.starttime // 1000 and end_us == m.endtime // 1000
        out.append({"t_us": m.starttime // 1000, "v": list(m.datasamples),
                    "extra": json.loads(m.extra) if m.extra else {},
                    "act": raw[36], "io": raw[37], "dq": raw[38]})
    return out


class BridgeTest(unittest.TestCase):
    def setUp(self):
        random.seed(7)
        self.dir = tempfile.mkdtemp()
        self.st = FakeStation()
        self.dl = FakeDataLink()

    def tearDown(self):
        shutil.rmtree(self.dir)

    def bridge(self, flush_s=3600.0):
        b = bridge.Bridge(self.st, self.dl, "XX", "VSEIS", "00", ["EHZ", "EH1"], self.dir,
                          flush_s=flush_s)
        self.addCleanup(b.journal.close)
        return b


    def run_until_idle(self, b, limit=1000):
        for _ in range(limit):
            if b.step() <= 0:
                return
        self.fail("bridge never caught up")

    def check_channel(self, recs, c, expect):
        """Records of channel c hold exactly the samples in expect, in order, on time."""
        got = decode(recs, ["XX_VSEIS_00_EHZ/MSEED", "XX_VSEIS_00_EH1/MSEED"][c])
        samples, times = [], []
        for r in got:
            for i, v in enumerate(r["v"]):
                samples.append(v)
                times.append(r["t_us"] + i * PERIOD_US)
        self.assertEqual(samples, [x["v"][c] for x in expect])
        for t, x in zip(times, expect):
            self.assertLessEqual(abs(t - x["t_us"]), 1)
        return got

    def test_continuous_catch_up(self):
        self.st.push(5000)
        b = self.bridge()
        self.run_until_idle(b)
        b.shutdown()
        for c in (0, 1):
            got = self.check_channel(self.dl.recs, c, self.st.s)
            for r in got:
                self.assertEqual(r["extra"]["FDSN"]["Time"]["Quality"], 100)
                self.assertTrue(r["io"] & 0x20, "clock locked with pps")
                self.assertEqual(r["dq"], 0)

    def test_partial_records_wait_for_more(self):
        self.st.push(150)
        b = self.bridge()
        self.run_until_idle(b)
        self.assertEqual(self.dl.recs, [], "150 quiet samples do not fill a record")
        self.st.push(5000)
        self.run_until_idle(b)
        b.shutdown()
        self.check_channel(self.dl.recs, 0, self.st.s)

    def test_age_flush(self):
        self.st.push(50)
        b = self.bridge(flush_s=0.0)
        self.run_until_idle(b)
        self.check_channel(self.dl.recs, 0, self.st.s)
        self.assertEqual(read_json(os.path.join(self.dir, "state.json"))["acked"], [50, 50])

    def test_flags_split_records(self):
        self.st.push(1000)
        self.st.push(30, flags=0x01 | 0x08)
        self.st.push(1000)
        b = self.bridge()
        self.run_until_idle(b)
        b.shutdown()
        got = self.check_channel(self.dl.recs, 0, self.st.s)
        flagged = [r for r in got if r["extra"]["FDSN"].get("Flags")]
        self.assertEqual(sum(len(r["v"]) for r in flagged), 30)
        for r in flagged:
            self.assertEqual(r["extra"]["FDSN"]["Flags"], {"MissingData": True, "DigitizerClipping": True})
            self.assertEqual(r["dq"], 0x10 | 0x02)

    def test_epoch_break_and_time_step(self):
        self.st.push(1200)
        self.st.epoch = 2
        self.st.push(800, jump_us=2_345_678)      # restart: grid moved
        self.st.push(700, jump_us=600_000)        # sample clock step, same epoch
        b = self.bridge()
        self.run_until_idle(b)
        b.shutdown()
        got = self.check_channel(self.dl.recs, 0, self.st.s)
        starts = {r["t_us"] for r in got}
        self.assertIn(self.st.s[1200]["t_us"], starts)
        self.assertIn(self.st.s[2000]["t_us"], starts)

    def test_bench_input_marks_only_its_channel(self):
        self.st.push(1500)
        self.st.push(400, flags=0x10)             # CH0 shorted on the bench
        self.st.push(1500)
        b = self.bridge()
        self.run_until_idle(b)
        b.shutdown()
        ch0 = self.check_channel(self.dl.recs, 0, self.st.s)
        ch1 = self.check_channel(self.dl.recs, 1, self.st.s)
        marked = [r for r in ch0 if r["act"] & 0x01]
        self.assertEqual(sum(len(r["v"]) for r in marked), 400)
        self.assertFalse(any(r["act"] & 0x01 for r in ch1), "CH1 carried real data")
        for r in marked:
            self.assertEqual(r["dq"], 0, "a bench input is not a data quality fault")

    def test_rtc_marks_time_questionable(self):
        self.st.tq = "rtc"
        self.st.push(2000)
        b = self.bridge()
        self.run_until_idle(b)
        b.shutdown()
        for r in self.check_channel(self.dl.recs, 1, self.st.s):
            self.assertTrue(r["dq"] & 0x80)
            self.assertFalse(r["io"] & 0x20)
            self.assertEqual(r["extra"]["FDSN"]["Time"]["Quality"], 20)

    def test_untimed_samples_skipped(self):
        self.st.tq = "none"
        self.st.push(300)
        self.st.tq = "ntp"
        self.st.push(2000, jump_us=5_000_000)
        b = self.bridge()
        self.run_until_idle(b)
        b.shutdown()
        self.check_channel(self.dl.recs, 0, self.st.s[300:])
        self.assertEqual(b.metrics.v["untimed_samples_total"], 300)

    def test_restart_without_loss_or_duplicates(self):
        self.st.push(3000)
        b = self.bridge()
        for _ in range(2):
            b.step()                    # 2000 samples in, a partial record per channel held
        b.journal.close()
        del b                           # crash: no shutdown, the held samples were never sent
        self.st.push(2000)
        b2 = self.bridge()
        self.assertGreater(b2.since, 0)
        self.run_until_idle(b2)
        b2.shutdown()
        for c in (0, 1):
            self.check_channel(self.dl.recs, c, self.st.s)

    def test_station_reboot(self):
        self.st.push(1500)
        b = self.bridge()
        self.run_until_idle(b)
        b.shutdown()
        old = list(self.st.s)
        self.st.reboot()
        self.st.push(1800, jump_us=30_000_000)
        b = self.bridge()
        self.run_until_idle(b)
        b.shutdown()
        self.assertEqual(b.metrics.v["station_reboots_total"], 1)
        self.check_channel(self.dl.recs, 0, old + self.st.s)

    def test_ring_overrun_counts_gap(self):
        self.st.push(1000)
        b = self.bridge()
        self.run_until_idle(b)
        first = list(self.st.s)
        self.st.ring_len = 2000
        self.st.push(5000)              # bridge "away": 3000 samples leave the ring
        self.run_until_idle(b)
        b.shutdown()
        self.assertEqual(b.metrics.v["gap_samples_total"], 3000)
        self.check_channel(self.dl.recs, 0, first + self.st.s)

    def test_datalink_outage(self):
        self.st.push(3000)
        b = self.bridge()
        self.dl.fail = 4
        self.run_until_idle(b)
        saved = read_json(os.path.join(self.dir, "state.json"))["acked"]
        self.assertEqual(saved, [0, 0], "nothing acknowledged yet")
        self.st.push(10)
        self.run_until_idle(b)          # writes succeed again
        b.shutdown()
        self.check_channel(self.dl.recs, 0, self.st.s)

    def test_station_down(self):
        self.st.down = True
        b = self.bridge()
        self.assertEqual(b.step(), 0)
        self.assertEqual(b.metrics.v["poll_errors_total"], 1)
        self.st.down = False
        self.st.push(2000)
        self.run_until_idle(b)
        b.shutdown()
        self.check_channel(self.dl.recs, 1, self.st.s)

    def test_journal(self):
        self.st.push(1500)
        b = self.bridge()
        self.run_until_idle(b)
        b.shutdown()
        jdir = os.path.join(self.dir, "journal")
        with open(os.path.join(jdir, os.listdir(jdir)[0]), encoding="utf-8") as f:
            lines = f.read().splitlines()
        self.assertEqual(sum(json.loads(l)["wave"]["n"] for l in lines), 1500)


class FlagRunsTest(unittest.TestCase):
    def test_runs(self):
        self.assertEqual(bridge.flag_runs([0, 0, 1, 1, 0], 5), [(0, 2, 0), (2, 4, 1), (4, 5, 0)])
        self.assertEqual(bridge.flag_runs([], 3), [(0, 3, 0)])

    def test_seq_diff_wraps(self):
        self.assertEqual(bridge.seq_diff(5, 0xFFFFFFFE), 7)
        self.assertEqual(bridge.seq_diff(0xFFFFFFFE, 5), -7)


if __name__ == "__main__":
    unittest.main()
