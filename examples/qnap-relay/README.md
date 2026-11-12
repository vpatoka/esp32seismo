# Gateway on a QNAP NAS, relay mode

Runs the [`gateway/`](../../gateway/README.md) stack on a QNAP with Container Station 3, and sends the stream out to a VPS instead of opening a port at home. Every connection starts inside the LAN; nothing on the internet connects in.

```
station 192.168.1.50 ──HTTP pull──► bridge ──► ringserver (NAS, 192.168.1.20) ──SeedLink──► slink2dali
                                               ring, SDS archive, LAN SeedLink :18000              │ DataLink
                                                                                                   ▼
                                                              autossh ══SSH out══► relay.example.org ringserver :18000 ──► clients
```

Five containers: `bridge`, `ringserver`, `sds-prune` as in gateway mode, plus `slink2dali` and `autossh` from the `relay` profile. Set up the VPS first with [`../vps-relay/`](../vps-relay/README.md).

## Install

On the NAS over SSH, as admin:

```sh
mkdir -p /share/Container/vseismo && cd /share/Container/vseismo
git clone https://github.com/vpatoka/esp32seismo.git && cd esp32seismo/gateway
cp ../examples/qnap-relay/env.example .env    # edit: LAN_ADDR, VSEISMO_URL, SEED codes, RELAY_*
D=$(grep ^DATA_ROOT .env | cut -d= -f2)
mkdir -p "$D"/ring "$D"/sds "$D"/bridge "$D"/slink2dali "$D"/ssh

# Tunnel key, used only for the relay forward (vps-relay README, step 4)
ssh-keygen -t ed25519 -N '' -C vseismo-gw -f "$D"/ssh/id_ed25519
ssh-keyscan -t ed25519 relay.example.org > "$D"/ssh/known_hosts
ssh-keygen -lf "$D"/ssh/known_hosts       # compare with the fingerprint on the VPS itself

chown -R 10000:10000 "$D"
chmod 600 "$D"/ssh/id_ed25519
docker compose up -d --build
```

If the NAS has no `git`, copy the repo there from another machine.

## Check

```sh
docker compose ps                          # five services Up
docker compose logs -f autossh slink2dali  # tunnel up, records flowing
curl -s http://192.168.1.20:9108/metrics
```

From anywhere: `slinktool -L relay.example.org:18000`, or Swarm pointed at `relay.example.org:18000`.

## Notes

- On first contact slink2dali asks the local ringserver for new data only. The relay starts at that moment; the NAS archive keeps everything from before.
- If the VPS or the uplink is down, the local ring holds about three weeks and slink2dali catches up from its state file (`$D/slink2dali/`).
- The NAS's own LAN SeedLink on `192.168.1.20:18000` stays available for Swarm or ObsPy on the LAN.
