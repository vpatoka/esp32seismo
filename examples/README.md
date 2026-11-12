# Examples

Other ways to run the [`gateway/`](../gateway/README.md) stack. The reference setup is a small Debian box that serves SeedLink through a router port forward; these are for networks where that doesn't fit.

| Example | What it shows |
|---|---|
| [`qnap-relay/`](qnap-relay/README.md) | The gateway stack on a QNAP NAS (Container Station) in relay mode: nothing connects into the home network |
| [`vps-relay/`](vps-relay/README.md) | The public end for relay mode: ringserver, pf, a restricted SSH tunnel user and StationXML on an OpenBSD VPS |

Addresses and names in these files are placeholders (`192.168.1.x`, `relay.example.org`, `www.example.org`).
