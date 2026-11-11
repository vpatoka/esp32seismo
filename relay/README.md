# vseismo relay on an OpenBSD VPS

The public face of the station. The NAS keeps an outbound SSH tunnel here and writes miniSEED over DataLink to `127.0.0.1:16000`. ringserver serves it to anyone as SeedLink on port 18000. The home network accepts nothing from outside.

```
NAS slink2dali ──DataLink──► autossh ══SSH :22══► sshd ──► 127.0.0.1:16000 ringserver :18000 SeedLink ──► clients
```

| File | Goes to |
|---|---|
| `ringserver.conf` | `/etc/ringserver.conf` |
| `rc.d/ringserver` | `/etc/rc.d/ringserver` |
| `pf.conf.snippet` | merged into `/etc/pf.conf` |
| `sshd_config.snippet` | appended to `/etc/ssh/sshd_config` |
| `authorized_keys.example` | `/home/seismo/.ssh/authorized_keys`, with the NAS public key |
| `httpd.conf.snippet` | inside the `www.example.org` server block of `/etc/httpd.conf` |

What was checked on Linux before this went in: ringserver 4.5.6 accepts `ringserver.conf` as is; `sshd -t` accepts the Match block; the tunnel carried data end to end with the `authorized_keys` line; with that line the key gets no shell and no other forward; with a bare key, the Match block alone does the same. What was not checked: building ringserver on OpenBSD (the source has no Linux-only calls and builds on macOS, but it hasn't been built here), and the `rc.d` logger.

## 1. Build ringserver

As your normal user:

```sh
doas pkg_add gmake
ftp -o ringserver-4.5.6.tar.gz https://github.com/EarthScope/ringserver/archive/refs/tags/v4.5.6.tar.gz
sha256 ringserver-4.5.6.tar.gz
# expect 56de465a95b7ade27cca71a9b8e832e7430b418416e5e4af9f4dff93d885bfbf
tar xzf ringserver-4.5.6.tar.gz && cd ringserver-4.5.6
gmake
./ringserver -V
doas install -o root -g bin -m 555 ringserver /usr/local/bin/ringserver
```

## 2. ringserver service

```sh
doas useradd -s /sbin/nologin -d /var/ringserver -c "vseismo ringserver" _ringserver
doas mkdir -p /var/ringserver/ring /var/ringserver/log
doas chown -R _ringserver:_ringserver /var/ringserver
doas install -o root -g wheel -m 644 ringserver.conf /etc/ringserver.conf
doas install -o root -g wheel -m 555 rc.d/ringserver /etc/rc.d/ringserver
doas rcctl enable ringserver
doas rcctl set ringserver logger daemon.info    # its output to syslog: /var/log/daemon
doas rcctl start ringserver
ftp -o - http://127.0.0.1:18080/id              # answers once it runs
```

`RingSize 512M` holds two to four weeks. Check free space on `/var` first (`df -h /var`).

## 3. Firewall

Merge `pf.conf.snippet` into `/etc/pf.conf` with your existing rules, then:

```sh
doas pfctl -nf /etc/pf.conf && doas pfctl -f /etc/pf.conf
```

Port 18000 opens to the world with connection-rate limits. 16000 and 18080 are blocked on egress and only reachable over `lo0`. ringserver also refuses writes from anything but localhost, so a pf mistake still leaves DataLink read-only to outsiders.

## 4. Tunnel user

```sh
doas useradd -m -s /sbin/nologin -c "vseismo tunnel" seismo
doas install -d -o seismo -g seismo -m 700 /home/seismo/.ssh
# the NAS public key: DATA_ROOT/ssh/id_ed25519.pub on the NAS
doas sh -c 'printf "%s %s\n" "restrict,port-forwarding,permitopen=\"127.0.0.1:16000\",command=\"/sbin/nologin\"" "$(cat id_ed25519.pub)" > /home/seismo/.ssh/authorized_keys'
doas chown seismo:seismo /home/seismo/.ssh/authorized_keys && doas chmod 600 /home/seismo/.ssh/authorized_keys
```

Append `sshd_config.snippet` to `/etc/ssh/sshd_config` (Match blocks must come last), then `doas sshd -t && doas rcctl reload sshd`.

Give the NAS the host key fingerprint to compare: `ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub`.

## 5. StationXML

```sh
doas mkdir -p /var/www/seismo
# copy XX.VSEIS.xml there
```

Add `httpd.conf.snippet` to the `www.example.org` server block, then `doas httpd -n && doas rcctl reload httpd`. Receivers fetch `https://www.example.org/seismo/XX.VSEIS.xml`.

## 6. DNS (optional)

`seedlink.example.org` as a CNAME of `relay.example.org` lets the relay move later without telling receivers.

## Check

```sh
ftp -o - http://127.0.0.1:18080/streams         # streams and their latest times
doas pfctl -t seedlink_abuse -T show            # clients cut off for connecting too fast
ls /var/ringserver/log                          # who connected and what they took
tail -f /var/log/daemon | grep ringserver
```

From anywhere: `slinktool -L relay.example.org:18000` lists the station, or point Swarm at `relay.example.org:18000`.

## TLS (later)

ringserver can serve SeedLink over TLS on 18500. acme-client's key is root-only, so copy the chain and key into `/var/ringserver/tls/` owned by `_ringserver` after each renewal, then enable the commented lines in `ringserver.conf`. Few SeedLink clients speak TLS yet, so plain 18000 stays the main port.
