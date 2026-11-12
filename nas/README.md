# vseismo gateway

Docker Compose stack that turns the station's HTTP API into a standard SeedLink stream and keeps a miniSEED archive. It runs on a NAS (QNAP Container Station) or on any small amd64 or arm64 Linux box. The bridge always opens the connection to the station; the station never connects out.

There are two ways to get the stream to the outside:

| Mode | Services | Who connects to what |
|---|---|---|
| Gateway (default) | `bridge`, `ringserver`, `sds-prune` | Clients connect to this host's SeedLink port, through a router port forward if they're on the internet |
| Relay (`COMPOSE_PROFILES=relay`) | the above plus `slink2dali`, `autossh` | This host tunnels out to a VPS (see `../relay/`), and clients connect there. Nothing on the internet connects in |

```
station 192.168.1.50 ──HTTP pull──► bridge ──DataLink──► ringserver ──SeedLink :18000──► LAN viewers
  /api/wave, order=oldest           journal              512 MB ring                     public clients (gateway mode)
                                    miniSEED 2           SDS archive
                                                              │ relay mode only
                                                              ▼
                                                         slink2dali ──DataLink── autossh ══SSH out══► relay (relay.example.org)
```

| Service | Image | Job |
|---|---|---|
| `bridge` | built from `bridge/` | Polls `/api/wave`, journals raw replies, packs 512-byte Steim2 miniSEED 2 records, writes them to ringserver with acknowledgement |
| `ringserver` | `earthscope/ringserver:4.5.6` | Ring buffer on disk, SDS archive of every record, SeedLink v3 and v4 on port 18000 |
| `sds-prune` | `busybox:1.37.0` | Once a day, deletes SDS day files older than `SDS_KEEP_DAYS` (365 by default, about 10 GB). 0 keeps everything |
| `slink2dali` | built from `slink2dali/` (EarthScope v0.8) | Relay mode: reads local SeedLink, writes the relay's DataLink through the tunnel, resumes from its state file |
| `autossh` | built from `autossh/` | Relay mode: keeps the SSH tunnel to the relay up. The key may open only that one forward |

## How the data stays complete

- The station keeps 22 minutes of samples. The bridge persists, per channel, the first seq not yet in an acknowledged record (`data/bridge/state.json`). After a restart it fetches from there and drops what was already sent: nothing is lost and nothing is duplicated, provided it's back within 22 minutes.
- A station reboot shows as a new `boot` id. The bridge sends what it holds and starts again from seq 0.
- Samples that left the station's ring before the bridge read them are counted in `vseismo_gap_samples_total`. miniSEED readers see the gap as a gap.
- If ringserver is down, packed records wait in memory and the cursor doesn't move. The 512 MB ring holds about three weeks: a SeedLink client that was cut off (a public collector in gateway mode, slink2dali in relay mode) resumes from where it stopped.
- Relay mode: on first contact slink2dali asks for new data only. The relay starts at that moment, and the local archive keeps everything from before.

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

### Gateway mode

On a Debian (or similar) box with Docker and the compose plugin, or a QNAP with Container Station 3:

```sh
git clone https://github.com/vpatoka/esp32seismo.git && cd esp32seismo/nas
cp .env.example .env      # edit: DATA_ROOT, LAN_ADDR, VSEISMO_URL, SEED codes; leave COMPOSE_PROFILES empty
D=$(grep ^DATA_ROOT .env | cut -d= -f2)
sudo mkdir -p "$D"/ring "$D"/sds "$D"/bridge
sudo chown -R 10000:10000 "$D"
docker compose up -d --build
```

To publish SeedLink, forward TCP 18000 on the router to `LAN_ADDR:18000`. Only SeedLink listens there (no DataLink, no HTTP), and only the bridge may write. Once you know the receiving network's collector addresses, limit the forward to them. A box that's reachable from the internet is better on its own router interface or VLAN, with access only to the station's port 80.

### Relay mode

As above, plus the tunnel key. Set `COMPOSE_PROFILES=relay` and the `RELAY_*` values in `.env` first:

```sh
sudo mkdir -p "$D"/slink2dali "$D"/ssh

# Tunnel key, used only for the relay forward (see relay/README.md, step 4)
ssh-keygen -t ed25519 -N '' -C vseismo-gw -f "$D"/ssh/id_ed25519
ssh-keyscan -t ed25519 relay.example.org > "$D"/ssh/known_hosts
ssh-keygen -lf "$D"/ssh/known_hosts       # compare with the fingerprint on the VPS itself

sudo chown -R 10000:10000 "$D"
sudo chmod 600 "$D"/ssh/id_ed25519
docker compose up -d --build
```

On the QNAP, run the same commands as admin over SSH, without `sudo`, with `DATA_ROOT` under `/share/Container`.

The bridge image runs its unit tests while it builds, so a broken build never starts. After a change to `RingSize` in `ringserver/ring.conf`, ringserver rebuilds the ring on its next start and carries over the packets that fit.

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

## Files

| Path under `DATA_ROOT` | Contents |
|---|---|
| `sds/` | miniSEED archive, pruned after `SDS_KEEP_DAYS`, `YYYY/NET/STA/CHAN.D/NET.STA.LOC.CHAN.D.YYYY.DOY`. Readable by ObsPy, SeisComP, any FDSN tool |
| `bridge/journal/` | Every raw `/api/wave` reply, one NDJSON file per UTC day, gzipped at rollover, kept `JOURNAL_DAYS` |
| `bridge/state.json` | Station boot id and per-channel cursor |
| `ring/` | ringserver packet buffer (512 MB) |
| `slink2dali/` | Relay mode: its SeedLink state file |
| `ssh/` | Relay mode: tunnel key and the relay's host key |

## Codes

`XX` is the FDSN test network code. Keep it until a receiving network assigns one, then change `SEED_NET` and the station's `network` setting together. `EH1` becomes `EHN` or `EHE` once the horizontal geophone's azimuth is measured.
