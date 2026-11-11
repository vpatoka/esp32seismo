# vseismo on the NAS

Four containers on the QNAP (Container Station, Docker Compose) turn the station's HTTP API into a standard SeedLink stream, keep a miniSEED archive, and forward the stream to the public relay. Every connection is opened from inside the LAN. Nothing on the internet connects in.

```
station 192.168.1.50 ──HTTP pull──► bridge ──DataLink──► ringserver ──SeedLink──► slink2dali
  /api/wave, order=oldest           journal              1 GB ring                 state file
                                    miniSEED 2           SDS archive                   │ DataLink
                                                         SeedLink :18000 (LAN)         ▼
                                                                         autossh ══SSH out══► relay (relay.example.org)
```

| Service | Image | Job |
|---|---|---|
| `bridge` | built from `bridge/` | Polls `/api/wave`, journals raw replies, packs 512-byte Steim2 miniSEED 2 records, writes them to ringserver with acknowledgement |
| `ringserver` | `earthscope/ringserver:4.5.6` | Ring buffer on disk, SDS archive of every record, SeedLink v3 and v4 on the LAN |
| `slink2dali` | built from `slink2dali/` (EarthScope v0.8) | Reads local SeedLink, writes the relay's DataLink through the tunnel, resumes from its state file |
| `autossh` | built from `autossh/` | Keeps the SSH tunnel to the relay up. The key may open only that one forward |

## How the data stays complete

- The station keeps 22 minutes of samples. The bridge persists, per channel, the first seq not yet in an acknowledged record (`data/bridge/state.json`). After a restart it fetches from there and drops what was already sent: nothing is lost and nothing is duplicated, provided it's back within 22 minutes.
- A station reboot shows as a new `boot` id. The bridge sends what it holds and starts again from seq 0.
- Samples that left the station's ring before the bridge read them are counted in `vseismo_gap_samples_total`. miniSEED readers see the gap as a gap.
- If ringserver is down, packed records wait in memory and the cursor doesn't move. If the relay or the uplink is down, the local 1 GB ring holds the stream for weeks, and slink2dali catches up from its state file.
- On first contact slink2dali asks for new data only. The relay starts at that moment, and the NAS archive keeps everything from before.

## Records

One record never mixes time quality, epoch (a station restart moves the time grid) or flags. A partial record is sent after `FLUSH_S` (10 s), which bounds the latency.

| Station | miniSEED 2 header |
|---|---|
| Flag bit 0, gap (input held) | Data quality bit 4, missing/padded |
| Flag bit 1, CRC (input held) | Data quality bit 5, telemetry sync error |
| Flag bit 2, filter settling | Data quality bit 6, filter may be charging |
| Flag bit 3, clip | Data quality bit 1, digitizer clipping |
| `tq` `pps` | I/O bit 5, clock locked. Blockette 1001 timing quality 100 |
| `tq` `ntp` | Timing quality 70 |
| `tq` `rtc` | Data quality bit 7, time tag questionable. Timing quality 20 |
| `tq` `none` | Not archived. Counted in `vseismo_untimed_samples_total` |
| Flag bit 4 or 5, bench input on CH0 or CH1 (`mux` short or test signal) | Activity bit 0, calibration signals present, on that channel's records only |

Bench tests are archived and streamed, but marked: receivers that honour the calibration bit skip them. A record never mixes bench and real samples.

## Install

On the NAS over SSH, with Container Station 3 installed:

```sh
mkdir -p /share/Container/vseismo && cd /share/Container/vseismo
# copy this nas/ directory here as app/
cd app
cp .env.example .env                      # edit: station URL, SEED codes, relay user
D=$(grep ^DATA_ROOT .env | cut -d= -f2)
mkdir -p "$D"/ring "$D"/sds "$D"/bridge "$D"/slink2dali "$D"/ssh

# Tunnel key, used only for the relay forward (see relay/README.md, step 4)
ssh-keygen -t ed25519 -N '' -C vseismo-nas -f "$D"/ssh/id_ed25519
ssh-keyscan -t ed25519 relay.example.org > "$D"/ssh/known_hosts
ssh-keygen -lf "$D"/ssh/known_hosts       # compare with the fingerprint on the VPS itself

chown -R 10000:10000 "$D"
chmod 600 "$D"/ssh/id_ed25519

docker compose up -d --build
```

The bridge image runs its unit tests while it builds, so a broken build never starts.

## Check

```sh
docker compose ps
docker compose logs -f bridge
curl -s http://192.168.1.20:9108/metrics
ls "$D"/sds/$(date +%Y)/XX/VSEIS/
```

Live view on any LAN machine: Swarm, or ObsPy `seedlink` client, at `192.168.1.20:18000`, station `XX.VSEIS`.

## Metrics (`:9108/metrics`)

| Metric | Meaning |
|---|---|
| `vseismo_samples_total`, `vseismo_records_sent_total` | Samples taken in, records acknowledged by ringserver |
| `vseismo_last_sample_age_seconds` | How far behind real time the newest sample is |
| `vseismo_station_pending` | Samples the station still holds for us. Above 0 only while catching up |
| `vseismo_gap_samples_total`, `vseismo_untimed_samples_total` | Samples lost to the ring, samples without wall-clock time |
| `vseismo_station_reboots_total`, `vseismo_poll_errors_total`, `vseismo_datalink_errors_total` | Faults |
| `vseismo_outbox_records` | Records waiting for ringserver |
| `vseismo_time_quality_percent` | 100 pps, 70 ntp, 20 rtc |
| `vseismo_station_*` | From `/api/status` every 60 s: RSSI, WiFi disconnects, lowest internal RAM, rate fit sigma, ADC ppm, SNTP age, slew |

## Files on the NAS

| Path under `DATA_ROOT` | Contents |
|---|---|
| `sds/` | miniSEED archive, `YYYY/NET/STA/CHAN.D/NET.STA.LOC.CHAN.D.YYYY.DOY`. Readable by ObsPy, SeisComP, any FDSN tool |
| `bridge/journal/` | Every raw `/api/wave` reply, one NDJSON file per UTC day, gzipped at rollover, kept `JOURNAL_DAYS` |
| `bridge/state.json` | Station boot id and per-channel cursor |
| `ring/` | ringserver packet buffer (1 GB) |
| `slink2dali/` | Its SeedLink state file |
| `ssh/` | Tunnel key and the relay's host key |

## Codes

`XX` is the FDSN test network code. Keep it until a receiving network assigns one, then change `SEED_NET` and the station's `network` setting together. `EH1` becomes `EHN` or `EHE` once the horizontal geophone's azimuth is measured.
