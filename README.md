# flu-monitor (project)

A DIY wood stove flue temperature monitor, made of two separate firmwares.
See `CLAUDE.md` for the full project goal and architecture.

- **[`flu-monitor/`](flu-monitor/)** — ESPHome sidecar that reads the
  stovepipe thermocouple and logs it. See `flu-monitor/README.md`.
- **[`flu-display/`](flu-display/)** — plain ESP-IDF ambient light display
  that polls the sidecar and shows the reading as a color/pulse gradient.
  See `flu-display/README.md`.
