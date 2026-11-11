#!/bin/sh
# SPDX-License-Identifier: MIT
# Keys and known_hosts are mounted read-only at /keys:
#   /keys/id_ed25519     private key, restricted on the relay to one forward
#   /keys/known_hosts    the relay's host key, so a changed key stops the tunnel
set -eu
: "${RELAY_HOST:?set RELAY_HOST}"
: "${RELAY_USER:?set RELAY_USER}"
RELAY_SSH_PORT="${RELAY_SSH_PORT:-22}"
RELAY_DL_PORT="${RELAY_DL_PORT:-16000}"
LOCAL_PORT="${LOCAL_PORT:-16000}"

for f in /keys/id_ed25519 /keys/known_hosts; do
    [ -r "$f" ] || { echo "missing $f" >&2; exit 1; }
done

# autossh restarts ssh when it exits; ssh itself detects a dead link with
# ServerAlive (45 s) and exits, since ExitOnForwardFailure makes a forward
# that cannot bind fatal. -M 0: no autossh monitor port.
export AUTOSSH_GATETIME=0
exec autossh -M 0 -N \
    -o BatchMode=yes \
    -o ExitOnForwardFailure=yes \
    -o ServerAliveInterval=15 -o ServerAliveCountMax=3 \
    -o StrictHostKeyChecking=yes -o UserKnownHostsFile=/keys/known_hosts \
    -o IdentitiesOnly=yes -i /keys/id_ed25519 \
    -L "0.0.0.0:${LOCAL_PORT}:127.0.0.1:${RELAY_DL_PORT}" \
    -p "$RELAY_SSH_PORT" "${RELAY_USER}@${RELAY_HOST}"
