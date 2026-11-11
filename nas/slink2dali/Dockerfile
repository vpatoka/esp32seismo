# SPDX-License-Identifier: MIT
# slink2dali (EarthScope): read SeedLink, write DataLink. Forwards the local
# ringserver to the relay through the SSH tunnel, resuming from a state file.
FROM debian:bookworm-slim AS build
ARG VERSION=0.8
ARG SHA256=b07a8064d39a808b0f20f1c47052649e5a27c21ecaf0aeff96dfe9657ee25920
RUN apt-get update && apt-get install -y --no-install-recommends build-essential curl ca-certificates \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
RUN curl -fsSL -o s2d.tar.gz https://github.com/EarthScope/slink2dali/archive/refs/tags/v${VERSION}.tar.gz \
 && echo "${SHA256}  s2d.tar.gz" | sha256sum -c - \
 && tar xzf s2d.tar.gz --strip-components=1 \
 && make -j"$(nproc)" \
 && ./slink2dali -V

FROM debian:bookworm-slim
RUN groupadd -g 10000 seismo && useradd -u 10000 -g 10000 -M -d /state seismo
COPY --from=build /src/slink2dali /usr/local/bin/slink2dali
USER 10000:10000
VOLUME /state
ENTRYPOINT ["/usr/local/bin/slink2dali"]
