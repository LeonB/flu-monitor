# Flue Monitor (project)

A DIY wood stove flue temperature monitor, made of two separate firmwares.
See `CLAUDE.md` for the full project goal and architecture.

- **[`flue-monitor/`](flue-monitor/)** — plain ESP-IDF sidecar that reads the
  stovepipe thermocouple and logs it. See `flue-monitor/README.md`.
- **[`flue-display/`](flue-display/)** — plain ESP-IDF ambient light display
  that polls the sidecar and shows the reading as a color/pulse gradient.
  See `flue-display/README.md`.
