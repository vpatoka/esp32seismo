# vseismo gateway

Docker Compose stack that turns the station's HTTP API into a standard SeedLink stream and keeps a miniSEED archive. It runs on any small amd64 or arm64 Linux box with Docker; the reference setup is a fanless mini PC on Debian 13. The bridge always opens the connection to the station; the station never connects out.

There are two ways to get the stream to the outside:

| Mode | Services | Who connects to what |
|---|---|---|
| Gateway (default) | `bridge`, `ringserver`, `sds-prune` | Clients connect to this host's SeedLink port, through a router port forward if they're on the internet |
| Relay (`COMPOSE_PROFILES=relay`) | the above plus `slink2dali`, `autossh` | This host tunnels out to a VPS (see [`../examples/`](../examples/README.md)), and clients connect there. Nothing on the internet connects in |

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

On a Debian (or similar) box with Docker and the compose plugin:

```sh
git clone https://github.com/vpatoka/esp32seismo.git && cd esp32seismo/gateway
cp .env.example .env      # edit: DATA_ROOT, LAN_ADDR, VSEISMO_URL, SEED codes; leave COMPOSE_PROFILES empty
D=$(grep ^DATA_ROOT .env | cut -d= -f2)
sudo mkdir -p "$D"/ring "$D"/sds "$D"/bridge
sudo chown -R 10000:10000 "$D"
docker compose up -d --build
```

Your user must be in the `docker` group (`sudo usermod -aG docker $USER`, then log in again). Without it, `docker compose` stops with `permission denied while trying to connect to the docker API at unix:///var/run/docker.sock`.

The bridge image runs its unit tests while it builds, so a broken build never starts. After a change to `RingSize` in `ringserver/ring.conf`, ringserver rebuilds the ring on its next start and carries over the packets that fit.

To publish SeedLink, forward TCP 18000 on the router to `LAN_ADDR:18000`. Only SeedLink listens there (no DataLink, no HTTP), and only the bridge may write. Once you know the receiving network's collector addresses, limit the forward to them. A box that's reachable from the internet is better on its own router interface or VLAN, with access only to the station's port 80.

### Relay mode

If nothing may connect into your network, set `COMPOSE_PROFILES=relay` and the `RELAY_*` values in `.env`: the stack then also tunnels the stream out to a VPS that serves it. [`../examples/qnap-relay/`](../examples/qnap-relay/README.md) walks through it on a NAS, and [`../examples/vps-relay/`](../examples/vps-relay/README.md) sets up the VPS end.

## Check

```sh
docker compose ps
docker compose logs -f bridge
curl -s http://192.168.1.20:9108/metrics
ls "$D"/sds/$(date +%Y)/XX/VSEIS/
```

Live view on any LAN machine: Swarm, or ObsPy `seedlink` client, at `192.168.1.20:18000`, station `XX.VSEIS`.

### A first start

On a 4-core Atom x5-Z8350 with 4 GB and eMMC (a fanless mini PC on Debian 13), the first `docker compose up -d --build` took about 90 s: 14 s pulling `ringserver` and `busybox`, 70 s building the bridge (base image 19 s, pymseed 16 s, tests 3 s, export 24 s). Later starts take seconds.

```
$ docker compose up -d --build
 ✔ Image earthscope/ringserver:4.5.6 Pulled
 ✔ Image busybox:1.37.0              Pulled
 => [7/7] RUN PYTHONDONTWRITEBYTECODE=1 python -m unittest -q test_bridge     3.3s
 ✔ Image vseismo/bridge:local        Built
 ✔ Network vseismo_seismo            Created
 ✔ Container vseismo-sds-prune-1     Started
 ✔ Container vseismo-ringserver-1    Started
 ✔ Container vseismo-bridge-1        Started

$ docker compose logs bridge ringserver
ringserver-1  | ... - ringserver version: 4.5.6
ringserver-1  | ... - Creating new ring packet buffer file
ringserver-1  | ... -    ringsize: 536870912 (512.0 MiB), pktsize: 632 (512 payload)
ringserver-1  | ... -    server ID: vseismo gateway
ringserver-1  | ... - Listening for connections on port 18000 (IPv4: SeedLink)
ringserver-1  | ... - Listening for connections on port 16000 (IPv4: DataLink)
bridge-1      | ... INFO bridge: bridge: http://192.168.1.50 -> ringserver:16000 as XX_VSEIS_00_EHZ/MSEED,XX_VSEIS_00_EH1/MSEED, since seq 0 (boot None)

$ curl -s http://192.168.1.20:9108/metrics | grep -E 'records_sent|gap|pending'
vseismo_station_pending 0
```

That last output is from a station whose acquisition wasn't running yet: the bridge is up and polling, but there are no samples, so there are no record counters yet. Once the station streams, `vseismo_records_sent_total` appears and grows by about one record per channel every few seconds, a DataLink `Client connected` line from the bridge shows in the ringserver log, and day files appear under `$D/sds/`. If `pending` stays above 0, the bridge is still catching up on the station's backlog.

## Updating

Pull, see what changed, and act only on that:

```sh
cd esp32seismo
git status --short                  # should be empty: .env and data are gitignored
old=$(git rev-parse HEAD)
git pull
git diff --stat "$old" HEAD
```

| Changed | On the gateway |
|---|---|
| `gateway/bridge/` | `docker compose up -d --build bridge` |
| `gateway/ringserver/ring.conf` | `docker compose restart ringserver` |
| `gateway/compose.yaml`, or your `.env` | `docker compose up -d` |
| `gateway/slink2dali/`, `gateway/autossh/` | relay mode only: `docker compose up -d --build` |
| `tools/stationxml/` | `docker build -t vseismo/stationxml tools/stationxml` if you generate StationXML here; the stack is untouched |
| `main/`, `hardware/`, `doc/`, `examples/` | nothing: firmware goes to the station, not here |

Run the `docker compose` lines from `gateway/`. When several rows match, `docker compose up -d --build` covers all of them except `ring.conf`.

`up -d` only recreates containers whose image or compose settings changed. `ring.conf` is a bind mount, so compose doesn't see edits to it: restart ringserver yourself. A changed `RingSize` makes it rebuild the ring and carry over the packets that fit.

Short restarts lose nothing. On SIGTERM the bridge flushes its open records (30 s grace), and on start it resumes from its saved seq, while the station keeps about 22 minutes of samples. While ringserver restarts, the bridge holds its records and retries; SeedLink clients reconnect by themselves.

After an update:

```sh
docker compose ps                                   # all Up, bridge (healthy)
docker compose logs --since 5m bridge ringserver
curl -s http://192.168.1.20:9108/metrics | grep -E 'records_sent|gap_samples|outbox|pending'
```

`gap_samples_total` should not grow, and `outbox_records` should go back to 0.

To roll back, `git checkout "$old"` (or `git revert` the commit) and repeat the same action. Builds leave old image layers behind, which add up on a small eMMC: `docker image prune` removes the unused ones.

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
