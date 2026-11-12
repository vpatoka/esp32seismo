# SPDX-License-Identifier: MIT
# vseismo bridge: station /api/wave -> miniSEED 2 -> DataLink
FROM python:3.12-slim-bookworm

RUN groupadd -g 10000 seismo && useradd -u 10000 -g 10000 -M -d /data seismo
WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY bridge.py test_bridge.py ./

# Unit tests at build time: a broken image never gets tagged.
RUN PYTHONDONTWRITEBYTECODE=1 python -m unittest -q test_bridge

USER 10000:10000
ENV PYTHONUNBUFFERED=1 DATA_DIR=/data METRICS_PORT=9108
VOLUME /data
EXPOSE 9108
HEALTHCHECK --interval=60s --timeout=5s --start-period=30s \
  CMD python -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:9108/metrics', timeout=3)"
ENTRYPOINT ["python", "/app/bridge.py"]
