#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Vlad Patoka (VE3VPT)
"""vseismo bridge: station /api/wave -> miniSEED 2 -> DataLink (ringserver).

The station keeps 22 minutes of output samples and serves them by seq. We
pull with order=oldest, so after any outage we catch up from where we
stopped. Each reply is one unbroken run (one epoch, one time quality), and
t0_us + i / fs holds for every sample in it.

Order of work per reply:
  1. append the raw reply to the journal (NDJSON, one file per UTC day)
  2. add the samples to the open segment, or close it and open a new one
  3. pack the full 512-byte Steim2 records, send them with ack
  4. persist, per channel, the first seq not yet in an acknowledged record

A partial record waits at most FLUSH_S. On restart we fetch from the
smallest persisted seq and drop, per channel, what was already sent, so
nothing is lost or duplicated while the station still holds it.

A segment is a run of samples with the same epoch, time quality and flag
set, contiguous in seq and time. Its flags go into every record packed
from it: station flags as miniSEED 2 data quality bits, the time quality
as blockette 1001, plus "clock locked" (I/O flags) for pps or "time tag
questionable" (data quality) for rtc. A channel on a bench input (station
flag bit 4 for CH0, 5 for CH1) gets activity bit 0, "calibration signals
present", on its own records only.
"""

import base64
import gzip
import http.server
import json
import logging
import os
import shutil
import signal
import socket
import threading
import time
import urllib.request
from datetime import datetime, timezone

import pymseed
from pymseed import DataEncoding, MS3Record, MS3TraceList

log = logging.getLogger("bridge")

RECLEN = 512
WAVE_MAX = 1000

# Station flag bit -> miniSEED extra header (libmseed maps these to the
# miniSEED 2 data quality bits: 4 missing/padded, 5 telemetry sync error,
# 6 filter charging, 1 digitizer clipping).
FLAG_HEADERS = (
    (0x01, "MissingData"),
    (0x02, "TelemetrySyncError"),
    (0x04, "FilterCharging"),
    (0x08, "DigitizerClipping"),
)

# Time quality -> blockette 1001 timing quality, percent.
TQ_QUALITY = {"pps": 100, "ntp": 70, "rtc": 20}

V2_IO_CLOCK_LOCKED = 0x20       # fixed header byte 37, bit 5
V2_DQ_TIME_QUESTIONABLE = 0x80  # fixed header byte 38, bit 7
V2_ACT_CALIBRATION = 0x01       # fixed header byte 36, bit 0

STATION_FLAGS = 0x3F            # bits 0-3 data quality, 4-5 bench input per channel


def test_flag(c):
    """Station flag: channel c's input was a bench short or test signal."""
    return 0x10 << c


def env(name, default=None):
    v = os.environ.get(name)
    return v if v not in (None, "") else default


# ─── DataLink ──────────────────────────────────────────────────────────────

class DataLinkError(Exception):
    pass


class DataLink:
    """Minimal DataLink 1.0 writer: ID, then WRITE with acknowledgement.

    Packet: "DL", one byte header length, header, then the payload. Times
    in a WRITE header are microseconds since the epoch, as libdali sends
    them, so our records and slink2dali's look the same to ringserver.
    """

    def __init__(self, host, port, timeout=10.0):
        self.host, self.port, self.timeout = host, port, timeout
        self.sock = None

    def connect(self):
        self.close()
        s = socket.create_connection((self.host, self.port), timeout=self.timeout)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        self.sock = s
        self._send(f"ID vseismo-bridge:{os.getpid()}:python")
        hdr, _ = self._recv()
        if not hdr.startswith("ID "):
            raise DataLinkError(f"unexpected ID reply: {hdr!r}")
        log.info("DataLink %s:%d: %s", self.host, self.port, hdr)

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
        self.sock = None

    def write(self, streamid, start_us, end_us, record):
        if not self.sock:
            self.connect()
        self._send(f"WRITE {streamid} {start_us} {end_us} A {len(record)}", record)
        hdr, msg = self._recv()
        if not hdr.startswith("OK"):
            raise DataLinkError(f"WRITE {streamid}: {hdr} {msg.decode(errors='replace')}")

    def _send(self, header, data=b""):
        h = header.encode()
        if len(h) > 255:
            raise DataLinkError("header too long")
        self.sock.sendall(b"DL" + bytes([len(h)]) + h + data)

    def _recv_exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise DataLinkError("connection closed")
            buf += chunk
        return buf

    def _recv(self):
        pre = self._recv_exact(3)
        if pre[:2] != b"DL":
            raise DataLinkError(f"bad preheader {pre!r}")
        hdr = self._recv_exact(pre[2]).decode(errors="replace")
        size = 0
        parts = hdr.split()
        if parts and parts[0] in ("OK", "ERROR") and len(parts) >= 3:
            size = int(parts[2])
        return hdr, self._recv_exact(size) if size else b""


# ─── Station ───────────────────────────────────────────────────────────────

class Station:
    def __init__(self, url, user=None, password=None, timeout=10.0):
        self.url = url.rstrip("/")
        self.timeout = timeout
        self.auth = None
        if user and password:
            self.auth = "Basic " + base64.b64encode(f"{user}:{password}".encode()).decode()

    def get(self, path):
        req = urllib.request.Request(self.url + path)
        if self.auth:
            req.add_header("Authorization", self.auth)
        with urllib.request.urlopen(req, timeout=self.timeout) as r:
            return json.loads(r.read())

    def wave(self, since):
        return self.get(f"/api/wave?since={since}&max={WAVE_MAX}&order=oldest")


# ─── Journal ───────────────────────────────────────────────────────────────

class Journal:
    """Raw replies, one NDJSON file per UTC day, gzipped at rollover.

    Written before anything is packed, so miniSEED can be rebuilt from it
    with different metadata or a fixed converter.
    """

    def __init__(self, path, keep_days):
        self.path, self.keep_days = path, keep_days
        os.makedirs(path, exist_ok=True)
        self.day, self.f = None, None

    def append(self, reply):
        day = datetime.now(timezone.utc).strftime("%Y-%m-%d")
        if day != self.day:
            self._roll(day)
        self.f.write(json.dumps({"rx": time.time(), "wave": reply}, separators=(",", ":")) + "\n")
        self.f.flush()
        os.fsync(self.f.fileno())

    def _roll(self, day):
        if self.f:
            self.f.close()
        self.day = day
        self.f = open(os.path.join(self.path, f"{day}.ndjson"), "a", encoding="utf-8")
        for name in sorted(os.listdir(self.path)):
            full = os.path.join(self.path, name)
            if name.endswith(".ndjson") and name[:10] != day:
                with open(full, "rb") as src, gzip.open(full + ".gz", "wb") as dst:
                    shutil.copyfileobj(src, dst)
                os.remove(full)
        cutoff = time.time() - self.keep_days * 86400
        for name in os.listdir(self.path):
            full = os.path.join(self.path, name)
            if name.endswith(".ndjson.gz") and os.path.getmtime(full) < cutoff:
                os.remove(full)

    def close(self):
        if self.f:
            self.f.close()
            self.f = None


# ─── State ─────────────────────────────────────────────────────────────────

class State:
    """Station boot id and, per channel, the first seq not yet sent."""

    def __init__(self, path, nch):
        self.path = path
        self.boot = None
        self.acked = [0] * nch
        try:
            with open(path, encoding="utf-8") as f:
                d = json.load(f)
            if len(d.get("acked", [])) == nch:
                self.boot, self.acked = d.get("boot"), [int(x) for x in d["acked"]]
        except (OSError, ValueError):
            pass

    def save(self):
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump({"boot": self.boot, "acked": self.acked, "saved": time.time()}, f)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, self.path)


# ─── Metrics ───────────────────────────────────────────────────────────────

class Metrics:
    def __init__(self):
        self.lock = threading.Lock()
        self.v = {}

    def inc(self, name, n=1):
        with self.lock:
            self.v[name] = self.v.get(name, 0) + n

    def set(self, name, value):
        with self.lock:
            self.v[name] = value

    def text(self):
        with self.lock:
            items = sorted(self.v.items())
        out = []
        for k, val in items:
            if val is None:
                continue
            out.append(f"vseismo_{k} {float(val):.6g}")
        return "\n".join(out) + "\n"

    def serve(self, port):
        metrics = self

        class H(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                body = metrics.text().encode()
                self.send_response(200 if self.path in ("/metrics", "/") else 404)
                self.send_header("Content-Type", "text/plain; version=0.0.4")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *a):
                pass

        srv = http.server.ThreadingHTTPServer(("0.0.0.0", port), H)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        return srv


# ─── Segments ──────────────────────────────────────────────────────────────

class Segment:
    """Open run of samples on one time grid, with one flag set."""

    def __init__(self, key, fs, start_ns, seq0, sourceids):
        self.key = key                  # (epoch, tq, flags)
        self.fs = fs
        self.period_ns = 1e9 / fs
        self.seq_end = seq0             # seq after the last sample added
        self.t_end_ns = start_ns        # time of the sample at seq_end
        self.opened = time.monotonic()
        self.tl = MS3TraceList()
        self.sourceids = sourceids
        self.added = [0] * len(sourceids)

    def follows(self, key, fs, seq0, t0_ns):
        return (key == self.key and fs == self.fs and seq0 == self.seq_end
                and abs(t0_ns - self.t_end_ns) <= self.period_ns / 2)

    def add(self, cols, seq0, t0_ns, skip):
        """cols[c] = samples from seq0 on; skip[c] = leading samples to drop."""
        n = len(cols[0])
        for c, sid in enumerate(self.sourceids):
            k = min(skip[c], n)
            if k < n:
                self.tl.add_data(sid, cols[c][k:], "i", self.fs,
                                 starttime=int(round(t0_ns + k * self.period_ns)))
                self.added[c] += n - k
        self.seq_end = seq0 + n
        self.t_end_ns = t0_ns + n * self.period_ns

    def unsent(self, c):
        tid = self.tl.get_traceid(self.sourceids[c])
        return sum(s.numsamples for s in tid) if tid is not None else 0

    def extra_headers(self):
        _, tq, flags = self.key
        hdr = {"FDSN": {"Time": {"Quality": TQ_QUALITY.get(tq, 0)}}}
        fl = {name: True for bit, name in FLAG_HEADERS if flags & bit}
        if fl:
            hdr["FDSN"]["Flags"] = fl
        return json.dumps(hdr)

    def pack(self, flush):
        """Yield (channel index, record) for full records, or all with flush."""
        recs = self.tl.generate(max_record_length=RECLEN, encoding=DataEncoding.STEIM2,
                                format_version=2, extra_headers=self.extra_headers(),
                                flush_data=flush, remove_packed=True)
        tq = self.key[1]
        for rec in recs:
            rec = bytearray(rec)
            if tq == "pps":
                rec[37] |= V2_IO_CLOCK_LOCKED
            elif tq == "rtc":
                rec[38] |= V2_DQ_TIME_QUESTIONABLE
            yield bytes(rec)


def flag_runs(flags, n):
    """Split 0..n into runs of equal station flags: (start, end, flags)."""
    if not flags:
        return [(0, n, 0)]
    runs, start = [], 0
    for i in range(1, n + 1):
        if i == n or flags[i] != flags[start]:
            runs.append((start, i, flags[start] & STATION_FLAGS))
            start = i
    return runs


# ─── Bridge ────────────────────────────────────────────────────────────────

SEQ_MASK = 0xFFFFFFFF


def seq_diff(a, b):
    """a - b on the station's 32-bit seq, as a signed number."""
    d = (a - b) & SEQ_MASK
    return d - (1 << 32) if d & 0x80000000 else d


class Bridge:
    def __init__(self, station, dl, net, sta, loc, chans, data_dir,
                 flush_s=10.0, journal_days=60, metrics=None):
        self.station, self.dl = station, dl
        self.nch = len(chans)
        self.sourceids = [pymseed.nslc2sourceid(net, sta, loc, ch) for ch in chans]
        self.streamids = [f"{net}_{sta}_{loc}_{ch}/MSEED" for ch in chans]
        self.flush_s = flush_s
        self.metrics = metrics or Metrics()
        os.makedirs(data_dir, exist_ok=True)
        self.state = State(os.path.join(data_dir, "state.json"), self.nch)
        self.journal = Journal(os.path.join(data_dir, "journal"), journal_days)
        self.seg = None
        self.outbox = []                # (channel, record, start_us, end_us), not yet acknowledged
        # Per channel: first seq not in any packed record. Becomes the
        # persisted state.acked once the outbox has drained.
        self.sent_upto = list(self.state.acked)
        self.since = min(self.state.acked, key=lambda a: seq_diff(a, self.state.acked[0]))
        self.station_ok = self.dl_ok = True
        # A cold start (no saved boot) takes whatever the ring holds; after
        # that a seq jump means samples left the station's ring unread.
        self.have_data = self.state.boot is not None

    # One poll. Returns the station's pending count (poll again at once if > 0).
    def step(self):
        try:
            w = self.station.wave(self.since)
        except (OSError, ValueError) as e:
            self.metrics.inc("poll_errors_total")
            self.metrics.set("station_up", 0)
            if self.station_ok:
                log.warning("station unreachable: %s", e)
            self.station_ok = False
            self._age_flush()
            self._drain()
            return 0
        if not self.station_ok:
            log.info("station back")
        self.station_ok = True
        self.metrics.inc("polls_total")
        self.metrics.set("station_pending", w.get("pending", 0))

        boot = w.get("boot")
        if boot is not None and boot != self.state.boot:
            known = self.state.boot is not None
            self.state.boot = boot
            if known:
                # This reply answered an old-boot cursor and may start
                # anywhere in the new ring: send what we hold, reset, and
                # refetch from the new boot's oldest sample.
                log.warning("station rebooted (boot id changed): its seq restarts at 0")
                self.metrics.inc("station_reboots_total")
                self._close_segment()
                self._drain()
                self.sent_upto = [0] * self.nch
                self.state.acked = [0] * self.nch
                self.since = 0
                self.have_data = False
                self.state.save()
                return 1
            self.state.save()

        n, seq0 = w["n"], w["seq0"]
        if n > 0:
            self.journal.append(w)
            lost = seq_diff(seq0, self.since)
            if lost > 0 and self.have_data:
                log.warning("gap: wanted seq %d, station sent %d: %d samples left its ring",
                            self.since, seq0, lost)
                self.metrics.inc("gap_samples_total", lost)
                self._close_segment()
            self.have_data = True
            if w.get("tq") == "none" or not w.get("t0_us"):
                self.metrics.inc("untimed_samples_total", n)
                self._close_segment()
                self._advance_all(seq0 + n)
            else:
                self._ingest(w)
        self.since = w["next"]
        self._age_flush()
        self._drain()
        return w.get("pending", 0)

    def _advance_all(self, seq):
        self.sent_upto = [seq if seq_diff(seq, s) > 0 else s for s in self.sent_upto]

    def _ingest(self, w):
        n, seq0, fs = w["n"], w["seq0"], float(w["fs"])
        t0_ns = int(w["t0_us"]) * 1000
        period_ns = 1e9 / fs
        cols = [w[f"ch{c}"] for c in range(self.nch)]
        for a, b, flags in flag_runs(w.get("flags"), n):
            key = (w.get("epoch"), w["tq"], flags)
            s0, t_ns = (seq0 + a) & SEQ_MASK, t0_ns + a * period_ns
            if not (self.seg and self.seg.follows(key, fs, s0, t_ns)):
                self._close_segment()
                self.seg = Segment(key, fs, t_ns, s0, self.sourceids)
            # Drop what an earlier run of the bridge already sent.
            skip = [max(0, seq_diff(self.sent_upto[c], s0)) for c in range(self.nch)]
            self.seg.add([col[a:b] for col in cols], s0, t_ns, skip)
            self._pack(flush=False)
        self.metrics.inc("samples_total", n)
        last_ns = t0_ns + (n - 1) * period_ns
        self.metrics.set("last_sample_time_seconds", last_ns / 1e9)
        self.metrics.set("last_sample_age_seconds", time.time() - last_ns / 1e9)
        self.metrics.set("time_quality_percent", TQ_QUALITY.get(w["tq"], 0))

    def _pack(self, flush):
        seg = self.seg
        if not seg:
            return
        bench = seg.key[2]
        for rec in seg.pack(flush):
            msr = next(MS3Record.from_buffer(rec))
            c = self.sourceids.index(msr.sourceid)
            if bench & test_flag(c):
                rec = bytearray(rec)
                rec[36] |= V2_ACT_CALIBRATION
                rec = bytes(rec)
            self.outbox.append((c, rec, msr.starttime // 1000, msr.endtime // 1000))
        for c in range(self.nch):
            upto = (seg.seq_end - seg.unsent(c)) & SEQ_MASK
            if seq_diff(upto, self.sent_upto[c]) > 0:
                self.sent_upto[c] = upto

    def _close_segment(self):
        if self.seg:
            self._pack(flush=True)
            self.seg = None

    def _age_flush(self):
        """Send a partial record once the segment has held data FLUSH_S."""
        if self.seg and time.monotonic() - self.seg.opened >= self.flush_s:
            self._pack(flush=True)
            self.seg.opened = time.monotonic()

    def _drain(self):
        while self.outbox:
            c, rec, start_us, end_us = self.outbox[0]
            try:
                self.dl.write(self.streamids[c], start_us, end_us, rec)
            except (OSError, DataLinkError) as e:
                self.metrics.inc("datalink_errors_total")
                if self.dl_ok:
                    log.warning("DataLink: %s (retrying every poll)", e)
                self.dl_ok = False
                self.dl.close()
                self.metrics.set("outbox_records", len(self.outbox))
                return False
            self.outbox.pop(0)
            self.metrics.inc("records_sent_total")
        if not self.dl_ok:
            log.info("DataLink back")
        self.dl_ok = True
        self.metrics.set("outbox_records", 0)
        if self.state.acked != self.sent_upto:
            self.state.acked = list(self.sent_upto)
            self.state.save()
        return True

    def shutdown(self):
        self._close_segment()
        self._drain()
        self.journal.close()

    def poll_status(self):
        try:
            s = self.station.get("/api/status")
        except (OSError, ValueError):
            return
        m = self.metrics
        hw = s.get("hw") or {}
        radio, mem = hw.get("radio") or {}, hw.get("mem") or {}
        ref, tm = s.get("ref") or {}, s.get("time") or {}
        m.set("station_up", 1)
        m.set("station_uptime_seconds", s.get("uptime"))
        m.set("station_rssi_dbm", radio.get("rssi"))
        m.set("station_wifi_disconnects", radio.get("disconnects"))
        m.set("station_int_ram_min_bytes", mem.get("int_min"))
        m.set("station_rate_sigma_ppm", ref.get("rate_sigma_ppm"))
        m.set("station_adc_ppm", ref.get("adc_ppm"))
        m.set("station_ntp_age_seconds", tm.get("ntp_age"))
        m.set("station_slew_ms", tm.get("slew_ms"))


def main():
    logging.basicConfig(level=env("LOG_LEVEL", "INFO"),
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    station = Station(env("VSEISMO_URL", "http://vseismo"),
                      env("VSEISMO_USER"), env("VSEISMO_PASS"))
    net, sta = env("SEED_NET"), env("SEED_STA")
    if not (net and sta):
        cfg = station.get("/api/config")
        net, sta = net or cfg["network"], sta or cfg["station"]
    chans = env("SEED_CHAN", "EHZ,EH1").split(",")
    dl = DataLink(env("DL_HOST", "ringserver"), int(env("DL_PORT", "16000")))
    metrics = Metrics()
    metrics.serve(int(env("METRICS_PORT", "9108")))
    b = Bridge(station, dl, net, sta, env("SEED_LOC", "00"), chans, env("DATA_DIR", "/data"),
               flush_s=float(env("FLUSH_S", "10")), journal_days=int(env("JOURNAL_DAYS", "60")),
               metrics=metrics)
    log.info("bridge: %s -> %s:%d as %s, since seq %d (boot %s)",
             station.url, dl.host, dl.port, ",".join(b.streamids), b.since, b.state.boot)

    stop = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    poll_s = float(env("POLL_S", "1.0"))
    status_s = float(env("STATUS_S", "60"))
    next_status = 0.0
    while not stop.is_set():
        if time.monotonic() >= next_status:
            b.poll_status()
            next_status = time.monotonic() + status_s
        pending = b.step()
        if pending <= 0:
            stop.wait(poll_s)
    log.info("stopping: flushing open records")
    b.shutdown()


if __name__ == "__main__":
    main()
