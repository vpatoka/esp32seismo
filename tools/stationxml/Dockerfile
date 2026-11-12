# SPDX-License-Identifier: MIT
# StationXML generator. Build: docker build -t vseismo/stationxml tools/stationxml
FROM python:3.12-slim-bookworm
WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY make_stationxml.py test_make_stationxml.py site.example.yaml ./
RUN PYTHONDONTWRITEBYTECODE=1 python -m unittest -q test_make_stationxml
WORKDIR /work
ENTRYPOINT ["python", "/app/make_stationxml.py"]
