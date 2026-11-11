# SPDX-License-Identifier: MIT
# Outbound SSH tunnel: <container>:16000 -> relay 127.0.0.1:16000 (DataLink).
# Nothing listens on the home network; the relay key may only open this one
# forward (see relay/README.md).
FROM alpine:3.20
RUN apk add --no-cache openssh-client autossh \
 && addgroup -g 10000 seismo && adduser -u 10000 -G seismo -D -H -h /keys seismo
COPY entrypoint.sh /entrypoint.sh
USER 10000:10000
ENTRYPOINT ["/bin/sh", "/entrypoint.sh"]
