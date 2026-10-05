// Dashboard/graph/settings app for the sidecar's embedded web UI. Talks to
// this same device's own REST API (GET /api/reading, /api/settings,
// /api/events, /api/history; POST /api/settings, /api/event) -- no
// WebSocket here, REST polling is fine for a UI a human is looking at (see
// the project plan: WS is reserved for the flu-display link, which needs
// push-driven low latency this doesn't).

// --- Pulse/color math, ported from flu-display/main/led_display.c so the
// glow's *rhythm* (pulse speed scales with rate, peak color swaps toward
// the neighboring zone when trending) matches the physical LED ring
// exactly. The brightness treatment is deliberately NOT ported 1:1, though:
// the LED math dims an actual light source to ~8% at the trough, which
// reads as "breathing" on a physical diffuser but would just look like the
// circle going nearly black on a flat screen against this design's light
// cream background. Here the *halo* (glow-outer's box-shadow) breathes in
// opacity/size instead, while the inner circle's color stays fully
// saturated throughout -- same signals (speed, trend direction, zone),
// adapted for a screen instead of a light source.
// Pulse-tuning constants (idle/fast period, rate deadband, color-transition
// exponent) live in this.settings now, fetched from GET /api/settings (same
// live-fetch mechanism already used for zone_cold_max_c/zone_optimal_max_c/
// fast_rise_c_per_min) -- previously hardcoded here AND in
// flu-display/main/config.h independently, which had already drifted out of
// sync with flu-display/CLAUDE.md's own documented on-hardware tuning
// history. The literals below are only the pre-first-fetch fallback.
const DEFAULT_IDLE_PULSE_PERIOD_MS = 8000;
const DEFAULT_FAST_PULSE_PERIOD_MS = 1400;
const DEFAULT_RATE_DEADBAND_C_PER_MIN = 3.0;
const DEFAULT_COLOR_TRANSITION_EXPONENT = 3.0;
const DEFAULT_BREATHING_EXPONENT = 1.0;

// Colours are saved as #RRGGBB and shared with the physical LED ring.
const ZONE_ORDER = ['cold', 'optimal', 'hot'];
const DEFAULT_ZONE_COLORS = { cold: '#003cff', optimal: '#ff3700', hot: '#ff0000' };
// Presets only fill the existing draft colour fields; no extra saved setting.
const ZONE_THEMES = [
  { id: 'classic', name: 'Classic', colors: ['#003cff', '#ff3700', '#ff0000'] },
  { id: 'fire', name: 'Fire', colors: ['#f29900', '#f66d00', '#ff0000'] },
  { id: 'ember', name: 'Ember', colors: ['#547ca6', '#f3b54a', '#e85a24'] },
  { id: 'forest', name: 'Forest', colors: ['#526780', '#32845a', '#cb542e'] },
  { id: 'contrast', name: 'High contrast', colors: ['#0072b2', '#f0e442', '#d55e00'] },
];
function zoneColor(zoneName, settings = {}) {
  const zone = ZONE_ORDER.includes(zoneName) ? zoneName : 'cold';
  const value = settings[`zone_${zone}_color`];
  const hex = /^#[0-9a-f]{6}$/i.test(value || '') ? value : DEFAULT_ZONE_COLORS[zone];
  const bg = [1, 3, 5].map((offset) => parseInt(hex.slice(offset, offset + 2), 16));
  // Pick the higher-contrast text colour using relative luminance.
  const linear = bg.map((v) => { const c = v / 255; return c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4; });
  const luminance = linear[0] * 0.2126 + linear[1] * 0.7152 + linear[2] * 0.0722;
  return { bg, text: luminance > 0.179 ? [0, 0, 0] : [255, 255, 255] };
}
function peakZoneColor(zoneName, rate, settings) {
  const i = Math.max(0, ZONE_ORDER.indexOf(zoneName));
  if (rate > 0) return zoneColor(ZONE_ORDER[Math.min(2, i + 1)], settings);
  if (rate < 0) return zoneColor(ZONE_ORDER[Math.max(0, i - 1)], settings);
  return zoneColor(zoneName, settings);
}
// History graph range picker (see the graph screen's .range-picker) -- all
// four just slice the same 24h/4min-cadence ring buffer GET /api/history
// already returns in full (SENSORS_HISTORY_CAPACITY in sensors.h), so this
// is a pure client-side view filter, no separate backend request per range.
const HISTORY_RANGE_KEYS = ['1h', '3h', '6h', '24h'];
const HISTORY_RANGES_S = { '1h': 3600, '3h': 3 * 3600, '6h': 6 * 3600, '24h': 24 * 3600 };
const HISTORY_RANGE_LABELS = { '1h': 'hour', '3h': '3 hours', '6h': '6 hours', '24h': '24 hours' };

function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }
function lerp(a, b, t) { return a + (b - a) * t; }
function lerpRgb(a, b, t) { return [lerp(a[0], b[0], t), lerp(a[1], b[1], t), lerp(a[2], b[2], t)]; }
function rgbCss(c) { return `rgb(${Math.round(c[0])},${Math.round(c[1])},${Math.round(c[2])})`; }

// The "Apple sleep-LED" breathing curve: exp(sin(phase)), normalized from
// its natural range [1/e, e] back to [0, 1] -- ported verbatim from
// led_display.c's breath_envelope().
function breathEnvelope(cyclePos) {
  const kMin = 1 / Math.E, kMax = Math.E;
  const phase = 2 * Math.PI * cyclePos - Math.PI / 2;
  const raw = Math.exp(Math.sin(phase));
  return (raw - kMin) / (kMax - kMin);
}
function ratePulsePeriodMs(rateCPerMin, fastRiseCPerMin, idlePeriodMs, fastPeriodMs) {
  const t = fastRiseCPerMin > 0 ? clamp(rateCPerMin, 0, fastRiseCPerMin) / fastRiseCPerMin : 0;
  return lerp(idlePeriodMs, fastPeriodMs, t);
}

function app() {
  return {
    view: 'dashboard',
    sheetOpen: false,
    themeMenuOpen: false,
    ZONE_THEMES,
    ZONE_ORDER,
    toast: '',
    toastTimer: null,

    reading: { thermocouple_ok: false, thermocouple_c: 0, thermocouple_rate_c_per_min: 0, thermocouple_zone: 'cold' },
    lastReadingMs: 0,
    settings: {},
    draft: {},
    events: [],
    history: { now_s: 0, samples: [], events: [] },
    historyLoaded: false,
    historyRange: '24h',
    HISTORY_RANGE_KEYS,
    saving: false,

    // Glow animation state, updated ~30fps by a requestAnimationFrame loop
    // -- see runGlowFrame() below.
    cyclePos: 0,
    lastFrameMs: 0,
    glowOuterStyle: '',
    glowInnerStyle: '',

    get isStale() {
      // No real-time clock on the device, so this is "no reading fetched
      // in the last 90s" (3x the sensor's own ~30s cadence), not a true
      // reading-age check.
      return this.lastReadingMs === 0 || (Date.now() - this.lastReadingMs) > 90000;
    },

    get rateLabel() {
      const r = this.reading.thermocouple_rate_c_per_min || 0;
      const deadband = this.settings.rate_deadband_c_per_min ?? DEFAULT_RATE_DEADBAND_C_PER_MIN;
      if (Math.abs(r) < deadband) return 'Holding steady';
      return (r > 0 ? '+' : '') + r.toFixed(1) + ' °C/min';
    },

    get currentZoneCss() {
      return rgbCss(zoneColor(this.reading.thermocouple_zone, this.settings).bg);
    },

    async init() {
      await Promise.all([this.fetchReading(), this.fetchSettings(), this.fetchEvents()]);
      setInterval(() => this.fetchReading(), 5000);
      requestAnimationFrame((t) => this.runGlowFrame(t));
    },

    async fetchReading() {
      try {
        const res = await fetch('/api/reading');
        this.reading = await res.json();
        this.lastReadingMs = Date.now();
      } catch (e) { /* leaves the last-known reading up, isStale reflects the gap */ }
    },

    async fetchSettings() {
      try {
        const res = await fetch('/api/settings');
        this.settings = { minimum_brightness: 20, maximum_brightness: 255, breathing_exponent: DEFAULT_BREATHING_EXPONENT, ...Object.fromEntries(ZONE_ORDER.map((zone) => [`zone_${zone}_color`, DEFAULT_ZONE_COLORS[zone]])), ...await res.json() };
        this.draft = { ...this.settings };
      } catch (e) { /* keeps whatever was loaded before, if anything */ }
    },

    async fetchEvents() {
      try {
        const res = await fetch('/api/events');
        this.events = await res.json();
      } catch (e) {
        this.events = [];
      }
    },

    async fetchHistory() {
      this.historyLoaded = false;
      try {
        const res = await fetch('/api/history');
        this.history = await res.json();
      } catch (e) {
        this.history = { now_s: 0, samples: [], events: [] };
      }
      this.historyLoaded = true;
    },

    openSheet() { this.sheetOpen = true; },

    async logEvent(ev) {
      this.sheetOpen = false;
      try {
        const res = await fetch('/api/event', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ event: ev.slug }),
        });
        if (!res.ok) throw new Error('failed');
        // POST /api/event always records the tap locally and returns 200 --
        // sheets_queued:false means the 4-deep Sheets-delivery queue was
        // full at the moment of this exact tap, so THIS event was dropped
        // for Sheets specifically -- draining the queue only helps *future*
        // events, it can't retroactively deliver this one (there's nothing
        // held anywhere for it). Earlier wording here ("Sheets catching up")
        // wrongly implied this event would still show up in Sheets
        // eventually; it won't. Deliberately still not phrased as a
        // failure/retry prompt though: retrying would just log a second,
        // duplicate local annotation for the same tap, since the tap itself
        // already succeeded (see REVIEW.md finding #5/C5/D3).
        const data = await res.json().catch(() => ({}));
        this.showToast(data.sheets_queued === false ? 'Logged: ' + ev.label + ' (not sent to Sheets)' : 'Logged: ' + ev.label);
      } catch (e) {
        this.showToast("Couldn't log " + ev.label + ' -- check the connection');
      }
    },

    showToast(msg) {
      this.toast = msg;
      clearTimeout(this.toastTimer);
      this.toastTimer = setTimeout(() => { this.toast = ''; }, 4000);
    },

    get historyRangeLabel() {
      return HISTORY_RANGE_LABELS[this.historyRange] || HISTORY_RANGE_LABELS['24h'];
    },

    openGraph() {
      this.view = 'graph';
      this.historyRange = '24h';
      this.fetchHistory();
    },

    openSettings() {
      this.draft = { ...this.settings };
      this.themeMenuOpen = false;
      this.view = 'settings';
    },

    closeSettings() {
      this.themeMenuOpen = false;
      this.view = 'dashboard';
    },

    get zoneTheme() {
      return ZONE_THEMES.find(theme => ZONE_ORDER.every((zone, i) =>
        (this.draft[`zone_${zone}_color`] || '').toLowerCase() === theme.colors[i])) || null;
    },

    applyZoneTheme(id) {
      const theme = ZONE_THEMES.find(theme => theme.id === id);
      if (!theme) return;
      ZONE_ORDER.forEach((zone, i) => { this.draft[`zone_${zone}_color`] = theme.colors[i]; });
      this.themeMenuOpen = false;
    },

    changed(key) {
      return this.draft[key] !== this.settings[key];
    },

    get changeCount() {
      return Object.keys(this.draft).filter((k) => this.changed(k)).length;
    },

    step(key, delta) {
      this.draft[key] = (Number(this.draft[key]) || 0) + delta;
    },

    stepBounded(key, delta, min, max) {
      this.draft[key] = clamp((Number(this.draft[key]) || 0) + delta, min, max);
    },

    stepBreathingShape(delta) {
      this.draft.breathing_exponent = Number(clamp((this.draft.breathing_exponent ?? DEFAULT_BREATHING_EXPONENT) + delta, 0.3, 3.0).toFixed(1));
    },

    stepPulsePeriod(key, delta) {
      const isIdlePeriod = key === 'idle_pulse_period_ms';
      const min = isIdlePeriod ? this.draft.fast_pulse_period_ms : 500;
      const max = isIdlePeriod ? 30000 : this.draft.idle_pulse_period_ms;
      this.stepBounded(key, delta, min, max);
    },

    revertDraft() {
      this.draft = { ...this.settings };
    },

    async saveSettings() {
      this.saving = true;
      try {
        const res = await fetch('/api/settings', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(this.draft),
        });
        if (!res.ok) throw new Error('rejected');
        this.settings = { ...this.draft };
        this.showToast('Settings saved');
      } catch (e) {
        this.showToast('Save failed -- check the values');
      }
      this.saving = false;
    },

    // --- Glow animation ---
    runGlowFrame(nowMs) {
      const tickMs = this.lastFrameMs ? Math.min(200, nowMs - this.lastFrameMs) : 33;
      this.lastFrameMs = nowMs;

      const idlePeriodMs = this.settings.idle_pulse_period_ms ?? DEFAULT_IDLE_PULSE_PERIOD_MS;
      const fastPeriodMs = this.settings.fast_pulse_period_ms ?? DEFAULT_FAST_PULSE_PERIOD_MS;
      const deadband = this.settings.rate_deadband_c_per_min ?? DEFAULT_RATE_DEADBAND_C_PER_MIN;
      const colorExponent = this.settings.color_transition_exponent ?? DEFAULT_COLOR_TRANSITION_EXPONENT;

      const valid = this.reading.thermocouple_ok;
      let rate = this.reading.thermocouple_rate_c_per_min || 0;
      if (Math.abs(rate) < deadband) rate = 0;

      const periodMs = valid
        ? ratePulsePeriodMs(rate, this.settings.fast_rise_c_per_min || 20, idlePeriodMs, fastPeriodMs)
        : idlePeriodMs;
      this.cyclePos += tickMs / periodMs;
      if (this.cyclePos > 1) this.cyclePos -= 1;
      const envelope = clamp(breathEnvelope(this.cyclePos), 0, 1);
      const breathingEnvelope = Math.pow(envelope, this.settings.breathing_exponent ?? DEFAULT_BREATHING_EXPONENT);

      if (valid) {
        const zone = this.reading.thermocouple_zone;
        const trough = zoneColor(zone, this.settings);
        const peak = peakZoneColor(zone, rate, this.settings);
        const colorT = Math.pow(envelope, colorExponent);
        const bg = lerpRgb(trough.bg, peak.bg, colorT);
        const text = lerpRgb(trough.text, peak.text, colorT);
        const haloAlpha = lerp(0.25, 0.7, breathingEnvelope);
        const haloSpread = Math.round(lerp(14, 26, breathingEnvelope));
        this.glowInnerStyle = `background-color:${rgbCss(bg)};color:${rgbCss(text)}`;
        this.glowOuterStyle = `box-shadow:0 0 0 ${haloSpread}px rgba(${Math.round(bg[0])},${Math.round(bg[1])},${Math.round(bg[2])},${haloAlpha.toFixed(2)})`;
      } else {
        const alpha = lerp(0.15, 0.4, breathingEnvelope);
        this.glowInnerStyle = 'background-color:var(--color-neutral-300);color:var(--color-neutral-700)';
        this.glowOuterStyle = `box-shadow:0 0 0 18px rgba(160,150,134,${alpha.toFixed(2)})`;
      }

      requestAnimationFrame((t) => this.runGlowFrame(t));
    },

    // --- History graph SVG (range-filtered, see HISTORY_RANGES_S above) ---
    get graphHeight() { return 320; },

    get graphSvg() {
      const samples = this.history.samples || [];
      if (samples.length === 0) return '';

      // Samples are ordered oldest-first (largest age_s first), so this
      // keeps the most-recent contiguous slice matching the picked range --
      // a pure view filter over the same full 24h buffer GET /api/history
      // already returned (see HISTORY_RANGES_S's own comment above).
      const maxAgeS = HISTORY_RANGES_S[this.historyRange] || HISTORY_RANGES_S['24h'];
      const rangedSamples = samples.filter((s) => s[0] <= maxAgeS);

      // Historical samples land every ~4min (see sensors.c's
      // HISTORY_PUSH_EVERY_N), so the last one can be several minutes
      // stale by the time this renders -- appending the live /api/reading
      // value as an age-0 point extends the curve all the way to "now"
      // instead of stopping short of it. Without this, a very recently
      // logged event (also timestamped with second-level precision, unlike
      // the coarse samples) could end up plotted to the right of the
      // curve's last point -- visually reading as "in the future" even
      // though its own age is never actually negative.
      const plotSamples = this.reading.thermocouple_ok ? [...rangedSamples, [0, this.reading.thermocouple_c]] : rangedSamples;
      if (plotSamples.length === 0) return '';

      const W = 342, H = this.graphHeight;
      const temps = plotSamples.map((s) => s[1]);
      const coldMax = this.settings.zone_cold_max_c || 150;
      const optimalMax = this.settings.zone_optimal_max_c || 280;
      const dataMin = Math.min(...temps, coldMax);
      const dataMax = Math.max(...temps, optimalMax);
      const padBottom = Math.max(10, (dataMax - dataMin) * 0.1);
      // The "hot" band's top edge is just whatever headroom the plotted
      // data happens to leave above optimalMax -- a symmetric 10% pad made
      // it a barely-visible sliver whenever the burn stayed under the hot
      // threshold (the common case, since there's no real overfire data --
      // see the repo root CLAUDE.md's "Current phase is data-gathering"
      // section). A larger, fixed-minimum top pad keeps the red band
      // visually present instead of flattening away.
      const padTop = Math.max(30, (dataMax - dataMin) * 0.15);
      const yMin = dataMin - padBottom, yMax = dataMax + padTop;
      const yOf = (t) => H - ((t - yMin) / (yMax - yMin)) * H;
      const xOf = (ageS) => W - (clamp(ageS, 0, maxAgeS) / maxAgeS) * W;

      const ZONE_NAME = { 1: 'cold', 2: 'optimal', 3: 'hot' };
      const ZONE_FILL = Object.fromEntries([1, 2, 3].map((n) => [n, rgbCss(lerpRgb(zoneColor(ZONE_NAME[n], this.settings).bg, [255, 255, 255], 0.85))]));
      let svg = '';
      // Zone bands, cold at the bottom
      const bandTop = [yOf(yMax), yOf(optimalMax), yOf(coldMax)];
      const bandBottom = [yOf(optimalMax), yOf(coldMax), yOf(yMin)];
      const bandZone = [3, 2, 1];
      const bandLabel = {
        3: `Hot above ${Math.round(optimalMax)}°C`,
        2: `Optimal ${Math.round(coldMax)}–${Math.round(optimalMax)}°C`,
        1: `Cold below ${Math.round(coldMax)}°C`,
      };
      for (let i = 0; i < 3; i++) {
        const top = Math.min(bandTop[i], bandBottom[i]);
        const h = Math.abs(bandBottom[i] - bandTop[i]);
        if (h <= 0) continue;
        svg += `<rect x="0" y="${top.toFixed(1)}" width="${W}" height="${h.toFixed(1)}" fill="${ZONE_FILL[bandZone[i]]}"></rect>`;
        // Skip the label if the band's too thin to hold it legibly.
        if (h >= 16) {
          svg += `<text x="8" y="${(top + 13).toFixed(1)}" font-size="10" font-weight="600" fill="var(--color-neutral-700)" font-family="Figtree">${escapeXml(bandLabel[bandZone[i]])}</text>`;
        }
      }

      const points = plotSamples.map((s) => `${xOf(s[0]).toFixed(1)},${yOf(s[1]).toFixed(1)}`).join(' ');
      svg += `<polyline points="${points}" fill="none" stroke="var(--color-text)" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"></polyline>`;

      const last = plotSamples[plotSamples.length - 1];
      const lastZoneName = this.reading.thermocouple_ok ? this.reading.thermocouple_zone : ZONE_NAME[last[3]];
      const lastColor = rgbCss(zoneColor(lastZoneName, this.settings).bg);
      svg += `<circle cx="${xOf(last[0]).toFixed(1)}" cy="${yOf(last[1]).toFixed(1)}" r="6" fill="${lastColor}" stroke="var(--color-bg)" stroke-width="2.5"></circle>`;

      for (const ev of (this.history.events || [])) {
        if (ev[0] > maxAgeS) continue;
        const x = xOf(ev[0]);
        // Nearest sample's temperature, so the marker sits on the curve.
        let nearest = plotSamples[0];
        for (const s of plotSamples) { if (Math.abs(s[0] - ev[0]) < Math.abs(nearest[0] - ev[0])) nearest = s; }
        const y = yOf(nearest[1]);
        svg += `<circle cx="${x.toFixed(1)}" cy="${y.toFixed(1)}" r="6" fill="var(--color-bg)" stroke="var(--color-text)" stroke-width="2.5"></circle>`;
        svg += `<text x="${x.toFixed(1)}" y="${(y - 12).toFixed(1)}" font-size="10" font-weight="600" fill="var(--color-text)" font-family="Figtree" text-anchor="middle">${escapeXml(ev[1])}</text>`;
      }

      svg += `<text x="4" y="${H - 6}" font-size="10" fill="var(--color-neutral-700)" font-family="Figtree">${Math.round(maxAgeS / 3600)}h ago</text>`;
      svg += `<text x="${W - 4}" y="${H - 6}" font-size="10" fill="var(--color-neutral-700)" font-family="Figtree" text-anchor="end">now</text>`;
      return svg;
    },
  };
}

function escapeXml(s) {
  return String(s).replace(/[<>&"']/g, (c) => ({ '<': '&lt;', '>': '&gt;', '&': '&amp;', '"': '&quot;', "'": '&apos;' }[c]));
}
