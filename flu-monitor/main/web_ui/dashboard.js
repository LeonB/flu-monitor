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

// Only changes consumed by the display need its delivery acknowledgement.
const DISPLAY_SETTING_KEYS = [
  'zone_cold_max_c', 'zone_optimal_max_c', 'fast_rise_c_per_min',
  'rate_deadband_c_per_min', 'idle_pulse_period_ms', 'fast_pulse_period_ms',
  'color_transition_exponent', 'breathing_exponent', 'minimum_brightness', 'maximum_brightness',
  'zone_cold_color', 'zone_optimal_color', 'zone_hot_color',
];

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
  let historyRequest = null;
  return {
    view: 'dashboard',
    WIFI_DEVICES: [{ key: 'monitor', label: 'Monitor' }, { key: 'display', label: 'Display' }],
    deviceWifi: {},
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
    historyReceivedMs: 0,
    historyError: '',
    saving: false,
    savingDisplaySettings: false,
    displaySyncError: '',

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
      setInterval(() => this.fetchReading(), 5000);
      setInterval(() => this.fetchDeviceWifi(), 10000);
      requestAnimationFrame((t) => this.runGlowFrame(t));
      await Promise.all([this.fetchReading(), this.fetchSettings(), this.fetchEvents(), this.fetchDeviceWifi()]);
      this.fetchHistory(); // Warm the graph while the dashboard is visible.
    },

    async fetchDeviceWifi() {
      try {
        const res = await fetch('/api/devices/wifi', { cache: 'no-store', signal: AbortSignal.timeout(3000) });
        if (!res.ok) throw new Error('unavailable');
        this.deviceWifi = await res.json();
      } catch (e) {
        this.deviceWifi = {}; // Never show a cached signal as a live connection.
      }
    },

    wifiSignal(key) {
      const device = this.deviceWifi[key];
      if (!device) return { label: 'Unavailable', bars: 0, tone: 'muted', value: '—' };
      if (!device.connected || !Number.isFinite(device.rssi)) return { label: 'Offline', bars: 0, tone: 'muted', value: '—' };
      const rssi = device.rssi;
      const bars = rssi >= -60 ? 4 : rssi >= -70 ? 3 : rssi >= -80 ? 2 : 1;
      return { label: ['Weak', 'Fair', 'Good', 'Strong'][bars - 1], bars,
        tone: bars <= 2 ? 'weak' : 'good', value: `${rssi} dBm` };
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
      if (historyRequest) return historyRequest;
      if (this.historyReceivedMs && Date.now()-this.historyReceivedMs<30000) return;
      historyRequest = (async () => {
        try {
          const res = await fetch('/api/history', { signal: AbortSignal.timeout(10000) });
          if (!res.ok) throw new Error('unavailable');
          const history = await res.json();
          this.historyReceivedMs = Date.now();
          this.history = history;
          this.historyError = '';
        } catch (e) {
          this.historyError = this.history.samples.length ? '' : "Couldn't load history. Reopen the graph to retry.";
          // Preserve previously loaded data while the monitor is unavailable.
        } finally {
          this.historyLoaded = true;
          historyRequest = null;
        }
      })();
      return historyRequest;
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
        this.historyReceivedMs = 0; // A new event invalidates prefetched history.
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

    openGraph() {
      this.view = 'graph';
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
      if (this.saving) return;
      const submitted = { ...this.draft };
      const needsDisplay = DISPLAY_SETTING_KEYS.some(key => submitted[key] !== this.settings[key]);
      this.savingDisplaySettings = needsDisplay;
      this.saving = true;
      this.displaySyncError = '';
      let saved = false;
      try {
        const res = await fetch('/api/settings', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(submitted),
        });
        if (!res.ok) throw new Error('rejected');
        saved = true;
        const result = await res.json();
        submitted.settings_revision = result.settings_revision;
        this.draft.settings_revision = result.settings_revision;
        this.settings = submitted;
        if (needsDisplay) {
          const applied = await this.waitForDisplaySettings(result.settings_revision);
          if (applied) this.showToast('Settings saved and applied by display');
          else this.reportDisplaySyncError('Settings saved, but the display has not confirmed applying them. It may be disconnected or unable to fetch settings.');
        } else this.showToast('Settings saved');
      } catch (e) {
        if (saved && needsDisplay) this.reportDisplaySyncError('Settings saved, but display delivery could not be verified. Check the display connection.');
        else if (saved) this.showToast('Settings saved, but confirmation could not be read. Reload to check saved values.');
        else this.showToast('Save failed -- check the values or connection');
      }
      this.saving = false;
      this.savingDisplaySettings = false;
    },

    reportDisplaySyncError(message) {
      this.displaySyncError = message;
    },

    async waitForDisplaySettings(revision) {
      if (!Number.isInteger(revision)) return false;
      const deadline = Date.now() + 20000;
      while (Date.now() < deadline) {
        try {
          const res = await fetch('/api/settings/status', { signal: AbortSignal.timeout(3000), cache: 'no-store' });
          if (res.ok) {
            const status = await res.json();
            if (status.settings_revision !== revision) return false;
            if (status.display_applied === true) return true;
          }
        } catch (e) { /* keep checking until the acknowledgement deadline */ }
        await new Promise(resolve => setTimeout(resolve, 500));
      }
      return false;
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

  };
}
