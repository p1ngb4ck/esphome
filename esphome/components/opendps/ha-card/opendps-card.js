/**
 * OpenDPS Virtual Lab PSU Card for Home Assistant
 *
 * A skeuomorphic lab power supply card with:
 * - LCD seven-segment displays (voltage, current, power)
 * - Vintage stereo-style horizontal sliders for V/I adjustment
 * - Illuminated power button
 * - CV/CC/CP mode indicator LEDs
 * - Temperature gauge
 * - Brushed-metal chassis styling
 *
 * type: custom:opendps-card
 * name: "DPS5005"
 * entities:
 *   voltage_out: sensor.opendps_output_voltage
 *   current_out: sensor.opendps_output_current
 *   power_out: sensor.opendps_output_power
 *   voltage_in: sensor.opendps_input_voltage
 *   output_enabled: binary_sensor.opendps_output_state
 *   set_voltage: number.opendps_set_voltage
 *   set_current: number.opendps_set_current_limit
 *   output_switch: switch.opendps_output_enable
 *   temp1: sensor.opendps_heatsink_temperature
 *   temp2: sensor.opendps_ambient_temperature
 *   operating_mode: select.opendps_operating_mode
 */

// ── Seven-segment digit paths ──────────────────────────────────────────
// Each segment is a polygon. Segments labeled a-g (standard 7-seg layout):
//   ─a─
//  |   |
//  f   b
//  |   |
//   ─g─
//  |   |
//  e   c
//  |   |
//   ─d─

const SEG_PATHS = {
  a: 'M 3,0 L 17,0 L 15,2 L 5,2 Z',
  b: 'M 18,1 L 20,1 L 20,9 L 18,9 L 16,7 L 16,3 Z',
  c: 'M 18,11 L 20,11 L 20,19 L 18,19 L 16,17 L 16,13 Z',
  d: 'M 3,20 L 17,20 L 15,18 L 5,18 Z',
  e: 'M 0,11 L 2,11 L 4,13 L 4,17 L 2,19 L 0,19 Z',
  f: 'M 0,1 L 2,1 L 4,3 L 4,7 L 2,9 L 0,9 Z',
  g: 'M 3,10 L 5,8 L 15,8 L 17,10 L 15,12 L 5,12 Z',
};

// Which segments are on for each character
const DIGIT_MAP = {
  '0': 'abcdef',
  '1': 'bc',
  '2': 'abdeg',
  '3': 'abcdg',
  '4': 'bcfg',
  '5': 'acdfg',
  '6': 'acdefg',
  '7': 'abc',
  '8': 'abcdefg',
  '9': 'abcdfg',
  '-': 'g',
  ' ': '',
  '.': '',  // dot handled separately
};


class OpenDPSCard extends HTMLElement {
  constructor() {
    super();
    this.attachShadow({ mode: 'open' });
    this._config = {};
    this._hass = null;

    // Slider interaction state
    this._activeSlider = null;
    this._sliderTrackRect = null;
    this._pendingVoltage = null;
    this._pendingCurrent = null;
    this._sliderTimer = null;
    this._sliderSettleUntil = { voltage: 0, current: 0 };

    // Direct input state
    this._editingVoltage = false;
    this._editingCurrent = false;
  }

  setConfig(config) {
    if (!config.entities) {
      throw new Error('Please define entities in the card configuration');
    }
    this._config = {
      name: config.name || 'OpenDPS',
      entities: config.entities,
      show_temperatures: config.show_temperatures !== false,
      show_input_voltage: config.show_input_voltage !== false,
      show_power: config.show_power !== false,
      show_datalog: config.show_datalog !== false,
      lcd_color: config.lcd_color || 'green',   // green, amber, blue, white
      voltage_step: config.voltage_step || 0.1,
      current_step: config.current_step || 0.01,
      decimal_places_voltage: config.decimal_places_voltage ?? 2,
      decimal_places_current: config.decimal_places_current ?? 3,
      decimal_places_power: config.decimal_places_power ?? 2,
    };
    this._render();
  }

  set hass(hass) {
    this._hass = hass;
    this._updateDisplays();
  }

  get hass() {
    return this._hass;
  }

  // ── State helpers ──────────────────────────────────────────────────

  _getState(entityId) {
    if (!this._hass || !entityId) return null;
    const s = this._hass.states[entityId];
    return s ? s.state : null;
  }

  _getNumber(entityId) {
    const s = this._getState(entityId);
    if (s === null || s === 'unavailable' || s === 'unknown') return null;
    const n = parseFloat(s);
    return isNaN(n) ? null : n;
  }

  _isOn(entityId) {
    const s = this._getState(entityId);
    return s === 'on' || s === 'true' || s === '1';
  }

  // ── LCD color theme ────────────────────────────────────────────────

  _getLCDColors() {
    const themes = {
      green:  { on: '#39ff14', dim: '#0a3a05', bg: '#0c1a0c', glow: 'rgba(57,255,20,0.3)' },
      amber:  { on: '#ffbf00', dim: '#3a2a00', bg: '#1a1400', glow: 'rgba(255,191,0,0.3)' },
      blue:   { on: '#00bfff', dim: '#002a3a', bg: '#001520', glow: 'rgba(0,191,255,0.3)' },
      white:  { on: '#e0e0e0', dim: '#1a1a1a', bg: '#0a0a0a', glow: 'rgba(224,224,224,0.2)' },
    };
    return themes[this._config.lcd_color] || themes.green;
  }

  // ── Format number for 7-segment display ────────────────────────────

  _formatForDisplay(value, totalDigits, decimals) {
    if (value === null || value === undefined) {
      return '-'.repeat(totalDigits);
    }
    let str = value.toFixed(decimals);
    // Pad with spaces to fill display
    const charsNeeded = totalDigits + (decimals > 0 ? 1 : 0); // +1 for dot
    while (str.length < charsNeeded) str = ' ' + str;
    return str;
  }

  // ── Build 7-segment SVG for a string ───────────────────────────────

  _buildSegmentDisplay(text, colorOn, colorDim) {
    const charWidth = 24;
    const dotWidth = 8;
    let x = 0;
    let svgContent = '';

    for (let i = 0; i < text.length; i++) {
      const ch = text[i];
      if (ch === '.') {
        // Render dot
        svgContent += `<circle cx="${x + 3}" cy="19" r="2.5" fill="${colorOn}" />`;
        x += dotWidth;
        continue;
      }

      const activeSegs = DIGIT_MAP[ch] || '';
      for (const [seg, path] of Object.entries(SEG_PATHS)) {
        const isActive = activeSegs.includes(seg);
        svgContent += `<g transform="translate(${x}, 0)">
          <path d="${path}" fill="${isActive ? colorOn : colorDim}" />
        </g>`;
      }
      x += charWidth;
    }

    return { svg: svgContent, width: x };
  }

  // ── Horizontal slider (vintage stereo fader style) ─────────────────

  _buildSlider(id, label, unit) {
    return `
      <div class="slider-group">
        <div class="slider-label">${label}</div>
        <div class="slider-scale" id="slider-scale-${id}">
          <span class="slider-scale-label" id="slider-scale-min-${id}">0</span>
          <span class="slider-scale-label" id="slider-scale-q1-${id}"></span>
          <span class="slider-scale-label" id="slider-scale-mid-${id}"></span>
          <span class="slider-scale-label" id="slider-scale-q3-${id}"></span>
          <span class="slider-scale-label" id="slider-scale-max-${id}">${unit}</span>
        </div>
        <div class="slider-track-container" id="slider-${id}">
          <div class="slider-track">
            <div class="slider-track-fill" id="slider-fill-${id}"></div>
            <div class="slider-tick-marks">
              ${Array(21).fill(0).map((_, i) =>
                `<div class="slider-tick${i % 5 === 0 ? ' slider-tick-major' : ''}"></div>`
              ).join('')}
            </div>
          </div>
          <div class="slider-thumb" id="slider-thumb-${id}">
            <div class="slider-thumb-grip"></div>
            <div class="slider-thumb-grip"></div>
            <div class="slider-thumb-grip"></div>
          </div>
        </div>
      </div>`;
  }

  // ── Power button SVG ───────────────────────────────────────────────

  _buildPowerButton(isOn) {
    const color = isOn ? '#39ff14' : '#555';
    const glow = isOn ? 'drop-shadow(0 0 8px rgba(57,255,20,0.8))' : 'none';
    return `
      <svg class="power-btn-svg" viewBox="0 0 70 70" width="70" height="70">
        <defs>
          <radialGradient id="pwr-grad" cx="40%" cy="35%">
            <stop offset="0%" stop-color="#555"/>
            <stop offset="100%" stop-color="#222"/>
          </radialGradient>
        </defs>
        <!-- Outer ring -->
        <circle cx="35" cy="35" r="33" fill="none"
                stroke="${color}" stroke-width="3"
                style="filter:${glow}; transition: all 0.3s;"/>
        <!-- Button body -->
        <circle cx="35" cy="35" r="28" fill="url(#pwr-grad)"
                stroke="#111" stroke-width="1.5"/>
        <!-- Power icon -->
        <path d="M 35 15 L 35 28" stroke="${color}" stroke-width="3.5"
              stroke-linecap="round" fill="none"
              style="filter:${glow}; transition: all 0.3s;"/>
        <path d="M 24 22 A 16 16 0 1 0 46 22" stroke="${color}" stroke-width="3"
              stroke-linecap="round" fill="none"
              style="filter:${glow}; transition: all 0.3s;"/>
      </svg>`;
  }

  // ── LED indicator ──────────────────────────────────────────────────

  _buildLED(color, isOn, label, clickable) {
    const fill = isOn ? color : '#333';
    const glow = isOn ? `drop-shadow(0 0 6px ${color})` : 'none';
    const cls = clickable ? 'led-indicator led-clickable' : 'led-indicator';
    const dataAttr = clickable ? ` data-mode="${label.toLowerCase()}"` : '';
    return `
      <div class="${cls}"${dataAttr}>
        <svg width="20" height="20" viewBox="0 0 20 20">
          <circle cx="10" cy="10" r="7" fill="${fill}"
                  stroke="#222" stroke-width="1.5"
                  style="filter:${glow}; transition: all 0.3s;"/>
          <circle cx="7.5" cy="7.5" r="2" fill="rgba(255,255,255,${isOn ? 0.4 : 0.1})"/>
        </svg>
        <span class="led-label">${label}</span>
      </div>`;
  }

  // ── Temperature gauge ──────────────────────────────────────────────

  _buildTempGauge(temp, label, maxTemp) {
    maxTemp = maxTemp || 80;
    const pct = temp !== null ? Math.min(100, Math.max(0, (temp / maxTemp) * 100)) : 0;
    const barColor = pct > 80 ? '#ff3333' : pct > 60 ? '#ffaa00' : '#39ff14';
    const displayTemp = temp !== null ? temp.toFixed(1) : '--';
    return `
      <div class="temp-gauge">
        <div class="temp-label">${label}</div>
        <div class="temp-bar-track">
          <div class="temp-bar-fill" style="width:${pct}%;background:${barColor};"></div>
        </div>
        <div class="temp-value">${displayTemp}°C</div>
      </div>`;
  }

  // ── Main render ────────────────────────────────────────────────────

  _render() {
    if (!this._config) return;

    const lcd = this._getLCDColors();

    this.shadowRoot.innerHTML = `
      <style>${this._getStyles(lcd)}</style>
      <ha-card>
        <div class="psu-chassis">
          <!-- Top panel: model label and input voltage -->
          <div class="top-panel">
            <div class="model-label">${this._config.name}</div>
            <div class="input-voltage-display" id="input-v-display">
              <span class="iv-label">INPUT</span>
              <span class="iv-value" id="iv-value">--.- V</span>
            </div>
          </div>

          <!-- LCD Display area -->
          <div class="lcd-panel" style="background:${lcd.bg};">
            <div class="lcd-row">
              <div class="lcd-display-group">
                <div class="lcd-label" style="color:${lcd.on};">VOLTAGE</div>
                <div class="lcd-digits lcd-digits-readonly" id="lcd-voltage">
                  <!-- filled by JS -->
                </div>
                <div class="lcd-unit" style="color:${lcd.on};">V</div>
              </div>
              <div class="lcd-display-group">
                <div class="lcd-label" style="color:${lcd.on};">CURRENT</div>
                <div class="lcd-digits lcd-digits-readonly" id="lcd-current">
                  <!-- filled by JS -->
                </div>
                <div class="lcd-unit" style="color:${lcd.on};">A</div>
              </div>
            </div>
            ${this._config.show_power ? `
              <div class="lcd-row lcd-row-secondary">
                <div class="lcd-display-group lcd-small">
                  <div class="lcd-label lcd-label-sm" style="color:${lcd.on};">POWER</div>
                  <div class="lcd-digits lcd-digits-sm" id="lcd-power"></div>
                  <div class="lcd-unit lcd-unit-sm" style="color:${lcd.on};">W</div>
                </div>
                <div class="lcd-display-group lcd-small">
                  <div class="lcd-label lcd-label-sm" style="color:${lcd.on};">SET V</div>
                  <div class="lcd-digits lcd-digits-sm" id="lcd-set-voltage" title="Click to edit"></div>
                  <div class="lcd-unit lcd-unit-sm" style="color:${lcd.on};">V</div>
                </div>
                <div class="lcd-display-group lcd-small">
                  <div class="lcd-label lcd-label-sm" style="color:${lcd.on};">SET A</div>
                  <div class="lcd-digits lcd-digits-sm" id="lcd-set-current" title="Click to edit"></div>
                  <div class="lcd-unit lcd-unit-sm" style="color:${lcd.on};">A</div>
                </div>
              </div>
            ` : ''}
          </div>

          <!-- Mode LEDs -->
          <div class="led-row" id="led-row">
            <!-- filled by JS -->
          </div>

          <!-- Controls panel -->
          <div class="controls-panel">
            ${this._buildSlider('voltage', 'VOLTAGE', 'V')}
            <div class="power-button-area" id="power-button">
              ${this._buildPowerButton(false)}
              <div class="pwr-label" id="pwr-label">OUTPUT</div>
            </div>
            ${this._buildSlider('current', 'CURRENT', 'A')}
          </div>

          <!-- Temperature gauges -->
          ${this._config.show_temperatures ? `
            <div class="temp-panel" id="temp-panel">
              <!-- filled by JS -->
            </div>
          ` : ''}

          <!-- Datalog controls -->
          ${this._config.show_datalog && this._config.entities.datalog_start ? `
            <div class="datalog-panel" id="datalog-panel">
              <div class="datalog-header">
                <svg width="14" height="14" viewBox="0 0 14 14" style="vertical-align:middle;">
                  <circle cx="7" cy="7" r="5" fill="#333" stroke="#222" stroke-width="1.5" id="datalog-led"/>
                </svg>
                <span class="datalog-title">DATALOG</span>
              </div>
              ${this._config.entities.datalog_filename ? `
                <div class="datalog-filename-row">
                  <input type="text" id="datalog-filename-input" class="datalog-filename"
                         placeholder="filename.csv" value="">
                </div>
              ` : ''}
              <div class="datalog-buttons">
                <button class="datalog-btn datalog-btn-start" id="datalog-btn-start">REC</button>
                <button class="datalog-btn datalog-btn-stop" id="datalog-btn-stop">STOP</button>
                ${this._config.entities.datalog_flush ? `
                  <button class="datalog-btn datalog-btn-flush" id="datalog-btn-flush">FLUSH</button>
                ` : ''}
              </div>
            </div>
          ` : ''}

          <!-- Bottom trim -->
          <div class="bottom-trim">
            <div class="ventilation">
              ${Array(12).fill('<div class="vent-slot"></div>').join('')}
            </div>
          </div>

          <!-- Hidden inputs for direct value entry (inside psu-chassis for positioning) -->
          <input type="number" id="voltage-edit-input" class="hidden-input"
                 step="${this._config.voltage_step}" min="0">
          <input type="number" id="current-edit-input" class="hidden-input"
                 step="${this._config.current_step}" min="0">
        </div>
      </ha-card>
    `;

    this._attachEventListeners();
    this._updateDisplays();
  }

  // ── Update all displays with current state ─────────────────────────

  _updateDisplays() {
    if (!this._hass || !this._config || !this.shadowRoot) return;

    const ent = this._config.entities;
    const lcd = this._getLCDColors();
    const dp = this._config;

    // Output values
    const vOut = this._getNumber(ent.voltage_out);
    const iOut = this._getNumber(ent.current_out);
    const pOut = this._getNumber(ent.power_out);
    const vIn = this._getNumber(ent.voltage_in);

    // Setpoints
    const setV = this._getNumber(ent.set_voltage);
    const setI = this._getNumber(ent.set_current);

    // Output state
    const isOn = this._isOn(ent.output_enabled) || this._isOn(ent.output_switch);

    // Mode
    const mode = this._getState(ent.operating_mode);

    // Temperatures
    const t1 = this._getNumber(ent.temp1);
    const t2 = this._getNumber(ent.temp2);

    // ── Update LCD displays ──
    this._updateLCD('lcd-voltage', vOut, 4, dp.decimal_places_voltage, lcd);
    this._updateLCD('lcd-current', iOut, 4, dp.decimal_places_current, lcd);

    if (dp.show_power) {
      this._updateLCD('lcd-power', pOut, 4, dp.decimal_places_power, lcd, true);
      // Only update SET displays when not actively dragging or settling
      const now2 = Date.now();
      if (this._activeSlider !== 'voltage' && now2 > this._sliderSettleUntil.voltage) {
        this._updateLCD('lcd-set-voltage', setV, 4, dp.decimal_places_voltage, lcd, true);
      }
      if (this._activeSlider !== 'current' && now2 > this._sliderSettleUntil.current) {
        this._updateLCD('lcd-set-current', setI, 4, dp.decimal_places_current, lcd, true);
      }
    }

    // ── Input voltage ──
    const ivEl = this.shadowRoot.getElementById('iv-value');
    if (ivEl) {
      ivEl.textContent = vIn !== null ? `${vIn.toFixed(1)} V` : '--.- V';
    }

    // ── Power button ──
    // Update color/glow of existing SVG elements in-place (no DOM replacement)
    const pwrBtn = this.shadowRoot.getElementById('power-button');
    if (pwrBtn) {
      const color = isOn ? '#39ff14' : '#555';
      const glow = isOn ? 'drop-shadow(0 0 8px rgba(57,255,20,0.8))' : 'none';
      const svg = pwrBtn.querySelector('.power-btn-svg');
      if (svg) {
        // Outer ring
        const circles = svg.querySelectorAll('circle');
        if (circles[0]) { circles[0].setAttribute('stroke', color); circles[0].style.filter = glow; }
        // Power icon paths
        const paths = svg.querySelectorAll('path');
        for (const p of paths) { p.setAttribute('stroke', color); p.style.filter = glow; }
      }
      // Update label text
      const pwrLabel = pwrBtn.querySelector('.pwr-label');
      if (pwrLabel) {
        pwrLabel.textContent = isOn ? 'OUTPUT ON' : 'OUTPUT';
      }
    }

    // ── Mode LEDs ──
    const ledRow = this.shadowRoot.getElementById('led-row');
    if (ledRow) {
      const modeStr = (mode || '').toLowerCase();
      ledRow.innerHTML =
        this._buildLED('#39ff14', modeStr === 'cv', 'CV', true) +
        this._buildLED('#ffbf00', modeStr === 'cc', 'CC', true) +
        this._buildLED('#ff3333', modeStr === 'cp', 'CP', true) +
        this._buildLED(isOn ? '#39ff14' : '#555', isOn, 'OUT', false);
    }

    // ── Temperature ──
    if (this._config.show_temperatures) {
      const tempPanel = this.shadowRoot.getElementById('temp-panel');
      if (tempPanel) {
        let html = '';
        if (ent.temp1) html += this._buildTempGauge(t1, 'HEATSINK', 80);
        if (ent.temp2) html += this._buildTempGauge(t2, 'AMBIENT', 50);
        tempPanel.innerHTML = html;
      }
    }

    // ── Datalog filename sync ──
    const fnInput = this.shadowRoot.getElementById('datalog-filename-input');
    if (fnInput && this.shadowRoot.activeElement !== fnInput) {
      const fnEntity = ent.datalog_filename;
      const fnVal = this._getState(fnEntity);
      if (fnVal && fnVal !== 'unknown' && fnVal !== 'unavailable') {
        fnInput.value = fnVal;
      }
    }

    // ── Update slider positions (skip during drag or settle period) ──
    const now = Date.now();
    if (setV !== null && this._activeSlider !== 'voltage' && now > this._sliderSettleUntil.voltage) {
      this._updateSliderPosition('voltage', setV);
    }
    if (setI !== null && this._activeSlider !== 'current' && now > this._sliderSettleUntil.current) {
      this._updateSliderPosition('current', setI);
    }

    // ── Update slider scale labels (min/q1/mid/q3/max from entity attributes) ──
    this._updateSliderScale('voltage');
    this._updateSliderScale('current');
  }

  _updateLCD(id, value, totalDigits, decimals, lcd, small) {
    const el = this.shadowRoot.getElementById(id);
    if (!el) return;

    const text = this._formatForDisplay(value, totalDigits, decimals);
    const { svg, width } = this._buildSegmentDisplay(text, lcd.on, lcd.dim);

    const scale = small ? 1.2 : 1.8;
    const h = small ? 26 : 40;
    el.innerHTML = `<svg viewBox="0 0 ${width} 22" width="${width * scale}" height="${h}"
                        style="filter:drop-shadow(0 0 3px ${lcd.glow});">
                     ${svg}
                   </svg>`;
  }

  _updateSliderPosition(which, value) {
    const { min, max } = this._getSliderRange(which);
    const pct = Math.max(0, Math.min(1, (value - min) / (max - min))) * 100;

    const fill = this.shadowRoot.getElementById(`slider-fill-${which}`);
    const thumb = this.shadowRoot.getElementById(`slider-thumb-${which}`);
    if (fill) fill.style.width = `${pct}%`;
    if (thumb) thumb.style.left = `${pct}%`;
  }

  _updateSliderScale(which) {
    const { min, max } = this._getSliderRange(which);
    const unit = which === 'voltage' ? 'V' : 'A';
    const decimals = which === 'voltage' ? 1 : 2;
    const q1 = min + (max - min) * 0.25;
    const mid = min + (max - min) * 0.5;
    const q3 = min + (max - min) * 0.75;

    const setLabel = (id, text) => {
      const el = this.shadowRoot.getElementById(id);
      if (el) el.textContent = text;
    };

    setLabel(`slider-scale-min-${which}`, min.toFixed(decimals));
    setLabel(`slider-scale-q1-${which}`, q1.toFixed(decimals));
    setLabel(`slider-scale-mid-${which}`, mid.toFixed(decimals));
    setLabel(`slider-scale-q3-${which}`, q3.toFixed(decimals));
    setLabel(`slider-scale-max-${which}`, max.toFixed(decimals) + unit);
  }

  _getSliderRange(which) {
    const ent = this._config.entities;
    let min = 0, max = which === 'voltage' ? 30 : 5;
    if (this._hass) {
      const entityId = which === 'voltage' ? ent.set_voltage : ent.set_current;
      if (entityId && this._hass.states[entityId]) {
        const attrs = this._hass.states[entityId].attributes;
        min = attrs.min || 0;
        max = attrs.max || max;
      }
    }
    return { min, max };
  }

  // ── Event listeners ────────────────────────────────────────────────

  _attachEventListeners() {
    // Attach directly to individual elements for reliability.
    // This avoids issues with ha-card shadow DOM swallowing events
    // and SVG→HTML namespace boundary problems with delegation.

    // ── Power button: direct click on the area container ──
    const pwrBtn = this.shadowRoot.getElementById('power-button');
    if (pwrBtn) {
      pwrBtn.addEventListener('click', () => {
        console.log('[OpenDPS] Power button clicked');
        this._toggleOutput();
      });
    } else {
      console.warn('[OpenDPS] Power button element not found!');
    }

    // ── LCD click-to-edit: only on SET V / SET A displays (actual values are read-only) ──
    const lcdTargets = [
      { id: 'lcd-set-voltage', which: 'voltage' },
      { id: 'lcd-set-current', which: 'current' },
    ];
    for (const { id, which } of lcdTargets) {
      const el = this.shadowRoot.getElementById(id);
      if (el) {
        el.addEventListener('click', () => {
          this._startEdit(which);
        });
      }
    }

    // ── Mode LED clicks (delegated on stable #led-row since children are rebuilt) ──
    const ledRow = this.shadowRoot.getElementById('led-row');
    if (ledRow) {
      ledRow.addEventListener('click', (e) => {
        const led = e.target.closest('.led-clickable');
        if (led && led.dataset.mode) {
          this._setMode(led.dataset.mode);
        }
      });
    }

    // ── Slider interactions ──
    this._setupSlider('voltage');
    this._setupSlider('current');

    // ── Hidden inputs for direct value entry ──
    this._setupEditInput('voltage-edit-input', 'voltage');
    this._setupEditInput('current-edit-input', 'current');

    // ── Datalog buttons ──
    const btnStart = this.shadowRoot.getElementById('datalog-btn-start');
    if (btnStart) {
      btnStart.addEventListener('click', () => this._datalogStart());
    }
    const btnStop = this.shadowRoot.getElementById('datalog-btn-stop');
    if (btnStop) {
      btnStop.addEventListener('click', () => this._datalogStop());
    }
    const btnFlush = this.shadowRoot.getElementById('datalog-btn-flush');
    if (btnFlush) {
      btnFlush.addEventListener('click', () => this._datalogFlush());
    }

    // ── Datalog filename: sync text input → HA text entity ──
    const fnInput = this.shadowRoot.getElementById('datalog-filename-input');
    if (fnInput) {
      fnInput.addEventListener('change', () => {
        this._setDatalogFilename(fnInput.value);
      });
    }
  }

  _setupEditInput(inputId, which) {
    const input = this.shadowRoot.getElementById(inputId);
    if (!input) return;

    const commit = () => {
      const val = parseFloat(input.value);
      if (!isNaN(val)) {
        if (which === 'voltage') this._setVoltage(val);
        else this._setCurrent(val);
      }
      this._endEdit(which);
    };

    input.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') {
        e.preventDefault();
        commit();
      } else if (e.key === 'Escape') {
        this._endEdit(which);
      }
    });
    input.addEventListener('blur', () => this._endEdit(which));
  }

  _setupSlider(which) {
    const container = this.shadowRoot.getElementById(`slider-${which}`);
    if (!container) return;

    const thumb = this.shadowRoot.getElementById(`slider-thumb-${which}`);
    if (!thumb) return;

    // Pointer down on thumb to start drag
    thumb.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      e.stopPropagation();
      this._activeSlider = which;
      const track = container.querySelector('.slider-track');
      this._sliderTrackRect = track.getBoundingClientRect();

      thumb.setPointerCapture(e.pointerId);
      thumb.classList.add('slider-thumb-active');

      const onMove = (ev) => this._onSliderMove(ev, which);
      const onUp = () => {
        this._onSliderUp(which);
        thumb.classList.remove('slider-thumb-active');
        thumb.removeEventListener('pointermove', onMove);
        thumb.removeEventListener('pointerup', onUp);
      };
      thumb.addEventListener('pointermove', onMove);
      thumb.addEventListener('pointerup', onUp);
    });

    // Click on track to jump to position
    container.addEventListener('pointerdown', (e) => {
      if (e.target.closest('.slider-thumb')) return;
      const track = container.querySelector('.slider-track');
      const rect = track.getBoundingClientRect();
      const pct = Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width));
      this._setSliderValue(which, pct);
      this._sliderSettleUntil[which] = Date.now() + 1500;
    });

    // Mouse wheel for fine adjustment
    container.addEventListener('wheel', (e) => {
      e.preventDefault();
      const ent = this._config.entities;
      const entityId = which === 'voltage' ? ent.set_voltage : ent.set_current;
      const current = this._getNumber(entityId) || 0;
      const step = which === 'voltage' ? this._config.voltage_step : this._config.current_step;
      const delta = e.deltaY < 0 ? step : -step;
      const { min, max } = this._getSliderRange(which);
      const newVal = Math.max(min, Math.min(max, current + delta));

      if (which === 'voltage') {
        this._setVoltage(newVal);
      } else {
        this._setCurrent(newVal);
      }
      this._updateSliderPosition(which, newVal);
      // Update SET display immediately for wheel
      const lcd = this._getLCDColors();
      const dp = this._config;
      if (which === 'voltage') {
        this._updateLCD('lcd-set-voltage', newVal, 4, dp.decimal_places_voltage, lcd, true);
      } else {
        this._updateLCD('lcd-set-current', newVal, 4, dp.decimal_places_current, lcd, true);
      }
      this._sliderSettleUntil[which] = Date.now() + 1500;
    }, { passive: false });
  }

  _onSliderMove(e, which) {
    if (!this._activeSlider || !this._sliderTrackRect) return;

    const rect = this._sliderTrackRect;
    const pct = Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width));
    this._setSliderValue(which, pct, true);
  }

  _onSliderUp(which) {
    // Commit any pending value
    if (this._pendingVoltage !== null) {
      this._setVoltage(this._pendingVoltage);
      this._pendingVoltage = null;
    }
    if (this._pendingCurrent !== null) {
      this._setCurrent(this._pendingCurrent);
      this._pendingCurrent = null;
    }
    // Prevent HA state from snapping slider back while the new value propagates
    this._sliderSettleUntil[which] = Date.now() + 1500;
    this._activeSlider = null;
    this._sliderTrackRect = null;
  }

  _setSliderValue(which, pct, debounce) {
    const { min, max } = this._getSliderRange(which);
    const step = which === 'voltage' ? this._config.voltage_step : this._config.current_step;
    let newVal = min + pct * (max - min);
    newVal = Math.round(newVal / step) * step;
    newVal = Math.max(min, Math.min(max, newVal));

    // Update visual immediately
    this._updateSliderPosition(which, newVal);

    // Update SET V / SET A LCD display live during drag
    const lcd = this._getLCDColors();
    const dp = this._config;
    if (which === 'voltage') {
      this._updateLCD('lcd-set-voltage', newVal, 4, dp.decimal_places_voltage, lcd, true);
    } else {
      this._updateLCD('lcd-set-current', newVal, 4, dp.decimal_places_current, lcd, true);
    }

    if (debounce) {
      // Debounce during drag
      if (which === 'voltage') {
        this._pendingVoltage = newVal;
      } else {
        this._pendingCurrent = newVal;
      }
      clearTimeout(this._sliderTimer);
      this._sliderTimer = setTimeout(() => {
        if (this._pendingVoltage !== null) {
          this._setVoltage(this._pendingVoltage);
          this._pendingVoltage = null;
        }
        if (this._pendingCurrent !== null) {
          this._setCurrent(this._pendingCurrent);
          this._pendingCurrent = null;
        }
      }, 100);
    } else {
      // Immediate (click on track)
      if (which === 'voltage') {
        this._setVoltage(newVal);
      } else {
        this._setCurrent(newVal);
      }
    }
  }

  // ── Direct edit mode ───────────────────────────────────────────────

  _startEdit(which) {
    console.log('[OpenDPS] Starting edit for:', which);
    const ent = this._config.entities;
    const entityId = which === 'voltage' ? ent.set_voltage : ent.set_current;
    const currentVal = this._getNumber(entityId);

    const inputId = which === 'voltage' ? 'voltage-edit-input' : 'current-edit-input';
    const input = this.shadowRoot.getElementById(inputId);
    if (!input) {
      console.warn('[OpenDPS] Edit input not found:', inputId);
      return;
    }

    // Position over the SET V / SET A display (not the actual value display)
    const lcdEl = this.shadowRoot.getElementById(
      which === 'voltage' ? 'lcd-set-voltage' : 'lcd-set-current'
    );
    if (!lcdEl) return;

    const rect = lcdEl.getBoundingClientRect();
    const cardRect = this.shadowRoot.querySelector('.psu-chassis').getBoundingClientRect();

    input.style.position = 'absolute';
    input.style.left = `${rect.left - cardRect.left}px`;
    input.style.top = `${rect.top - cardRect.top}px`;
    input.style.width = `${rect.width}px`;
    input.style.height = `${rect.height}px`;
    input.style.display = 'block';
    input.value = currentVal !== null ? currentVal.toFixed(
      which === 'voltage' ? this._config.decimal_places_voltage : this._config.decimal_places_current
    ) : '';

    // Set min/max from entity attributes
    if (this._hass && entityId && this._hass.states[entityId]) {
      const attrs = this._hass.states[entityId].attributes;
      if (attrs.min !== undefined) input.min = attrs.min;
      if (attrs.max !== undefined) input.max = attrs.max;
      if (attrs.step !== undefined) input.step = attrs.step;
    }

    setTimeout(() => {
      input.focus();
      input.select();
    }, 50);
  }

  _endEdit(which) {
    const inputId = which === 'voltage' ? 'voltage-edit-input' : 'current-edit-input';
    const input = this.shadowRoot.getElementById(inputId);
    if (input) {
      input.style.display = 'none';
    }
  }

  // ── HA service calls ───────────────────────────────────────────────

  _toggleOutput() {
    const entityId = this._config.entities.output_switch;
    if (!entityId || !this._hass) {
      console.warn('[OpenDPS] toggleOutput: missing entity or hass', { entityId, hass: !!this._hass });
      return;
    }
    const isOn = this._isOn(entityId);
    console.log('[OpenDPS] Calling switch service:', isOn ? 'turn_off' : 'turn_on', entityId);
    this._hass.callService('switch', isOn ? 'turn_off' : 'turn_on', {
      entity_id: entityId
    });
  }

  _setMode(mode) {
    const entityId = this._config.entities.operating_mode;
    if (!entityId || !this._hass) return;
    console.log('[OpenDPS] Setting mode:', mode, entityId);
    this._hass.callService('select', 'select_option', {
      entity_id: entityId,
      option: mode
    });
  }

  _setVoltage(value) {
    const entityId = this._config.entities.set_voltage;
    if (!entityId || !this._hass || isNaN(value)) return;
    console.log('[OpenDPS] Setting voltage:', value, entityId);
    this._hass.callService('number', 'set_value', {
      entity_id: entityId,
      value: parseFloat(value.toFixed(this._config.decimal_places_voltage))
    });
  }

  _setCurrent(value) {
    const entityId = this._config.entities.set_current;
    if (!entityId || !this._hass || isNaN(value)) return;
    console.log('[OpenDPS] Setting current:', value, entityId);
    this._hass.callService('number', 'set_value', {
      entity_id: entityId,
      value: parseFloat(value.toFixed(this._config.decimal_places_current))
    });
  }

  // ── Datalog controls ─────────────────────────────────────────────

  _datalogStart() {
    const entityId = this._config.entities.datalog_start;
    if (!entityId || !this._hass) return;
    console.log('[OpenDPS] Pressing datalog start:', entityId);
    this._hass.callService('button', 'press', { entity_id: entityId });
  }

  _datalogStop() {
    const entityId = this._config.entities.datalog_stop;
    if (!entityId || !this._hass) return;
    console.log('[OpenDPS] Pressing datalog stop:', entityId);
    this._hass.callService('button', 'press', { entity_id: entityId });
  }

  _datalogFlush() {
    const entityId = this._config.entities.datalog_flush;
    if (!entityId || !this._hass) return;
    console.log('[OpenDPS] Pressing datalog flush:', entityId);
    this._hass.callService('button', 'press', { entity_id: entityId });
  }

  _setDatalogFilename(value) {
    const entityId = this._config.entities.datalog_filename;
    if (!entityId || !this._hass) return;
    console.log('[OpenDPS] Setting datalog filename:', value, entityId);
    this._hass.callService('text', 'set_value', {
      entity_id: entityId,
      value: value
    });
  }

  // ── Styles ─────────────────────────────────────────────────────────

  _getStyles(lcd) {
    return `
      :host {
        display: block;
      }

      ha-card {
        overflow: visible;
        background: transparent !important;
        box-shadow: none !important;
        pointer-events: auto;
      }

      .psu-chassis {
        position: relative;
        background: linear-gradient(170deg, #3a3a3a 0%, #2a2a2a 40%, #222 100%);
        border-radius: 12px;
        border: 1px solid #555;
        box-shadow:
          0 4px 20px rgba(0,0,0,0.6),
          inset 0 1px 0 rgba(255,255,255,0.08),
          inset 0 -1px 0 rgba(0,0,0,0.3);
        padding: 16px;
        font-family: 'Roboto', 'Segoe UI', Arial, sans-serif;
        /* Brushed metal texture */
        background-image:
          linear-gradient(170deg, #3a3a3a 0%, #2a2a2a 40%, #222 100%),
          repeating-linear-gradient(
            90deg,
            transparent,
            transparent 1px,
            rgba(255,255,255,0.015) 1px,
            rgba(255,255,255,0.015) 2px
          );
      }

      /* ── Top panel ── */
      .top-panel {
        display: flex;
        justify-content: space-between;
        align-items: center;
        margin-bottom: 12px;
        padding: 0 4px;
      }

      .model-label {
        font-size: 16px;
        font-weight: 700;
        color: #ccc;
        text-transform: uppercase;
        letter-spacing: 2px;
        text-shadow: 0 1px 2px rgba(0,0,0,0.5);
      }

      .input-voltage-display {
        display: flex;
        align-items: center;
        gap: 6px;
        background: #1a1a1a;
        border: 1px solid #444;
        border-radius: 4px;
        padding: 4px 10px;
      }

      .iv-label {
        font-size: 9px;
        color: #777;
        text-transform: uppercase;
        letter-spacing: 1px;
      }

      .iv-value {
        font-size: 13px;
        font-family: 'Courier New', monospace;
        color: ${lcd.on};
        font-weight: bold;
        text-shadow: 0 0 4px ${lcd.glow};
      }

      /* ── LCD Panel ── */
      .lcd-panel {
        border: 2px solid #111;
        border-radius: 8px;
        padding: 16px;
        margin-bottom: 12px;
        box-shadow:
          inset 0 2px 8px rgba(0,0,0,0.8),
          0 1px 0 rgba(255,255,255,0.05);
      }

      .lcd-row {
        display: flex;
        justify-content: space-around;
        align-items: flex-end;
        gap: 16px;
      }

      .lcd-row-secondary {
        margin-top: 12px;
        padding-top: 10px;
        border-top: 1px solid rgba(255,255,255,0.05);
      }

      .lcd-display-group {
        display: flex;
        flex-direction: column;
        align-items: center;
        gap: 4px;
        flex: 1;
      }

      .lcd-label {
        font-size: 10px;
        font-weight: 600;
        letter-spacing: 2px;
        text-transform: uppercase;
        opacity: 0.7;
      }

      .lcd-label-sm {
        font-size: 8px;
        letter-spacing: 1px;
      }

      .lcd-digits {
        transition: opacity 0.2s;
        min-height: 40px;
        display: flex;
        align-items: center;
      }

      .lcd-digits-readonly {
        cursor: default;
      }

      .lcd-digits svg {
        pointer-events: none;
      }

      .lcd-digits-sm {
        min-height: 26px;
        cursor: pointer;
      }

      .lcd-digits-sm:hover {
        opacity: 0.85;
      }

      .lcd-unit {
        font-size: 14px;
        font-weight: 700;
        opacity: 0.6;
        letter-spacing: 1px;
      }

      .lcd-unit-sm {
        font-size: 10px;
      }

      .lcd-small {
        flex: 1;
      }

      /* ── LED row ── */
      .led-row {
        display: flex;
        justify-content: center;
        gap: 20px;
        margin-bottom: 14px;
        padding: 6px 0;
      }

      .led-indicator {
        display: flex;
        flex-direction: column;
        align-items: center;
        gap: 3px;
      }

      .led-clickable {
        cursor: pointer;
        -webkit-tap-highlight-color: transparent;
      }

      .led-clickable:hover svg circle:first-child {
        filter: brightness(1.3);
      }

      .led-clickable svg {
        pointer-events: none;
      }

      .led-label {
        font-size: 9px;
        color: #888;
        letter-spacing: 1px;
        font-weight: 600;
      }

      /* ── Controls panel ── */
      .controls-panel {
        display: flex;
        justify-content: space-between;
        align-items: center;
        padding: 8px 0;
        margin-bottom: 8px;
      }

      /* ── Slider (vintage fader) ── */
      .slider-group {
        flex: 1;
        display: flex;
        flex-direction: column;
        align-items: center;
        gap: 6px;
        user-select: none;
        touch-action: none;
        min-width: 0;
      }

      .slider-label {
        font-size: 9px;
        color: #888;
        letter-spacing: 2px;
        font-weight: 600;
        text-transform: uppercase;
      }

      .slider-track-container {
        position: relative;
        width: 100%;
        height: 40px;
        display: flex;
        align-items: center;
        cursor: pointer;
        padding: 0 14px;
        box-sizing: border-box;
      }

      .slider-track {
        position: relative;
        width: 100%;
        height: 6px;
        background: linear-gradient(to bottom, #111, #222);
        border-radius: 3px;
        border: 1px solid #444;
        box-shadow: inset 0 1px 3px rgba(0,0,0,0.6);
        overflow: hidden;
      }

      .slider-track-fill {
        position: absolute;
        top: 0;
        left: 0;
        height: 100%;
        background: linear-gradient(to right, ${lcd.on}44, ${lcd.on}aa);
        border-radius: 2px;
        transition: width 0.1s ease-out;
      }

      .slider-tick-marks {
        position: absolute;
        top: 0;
        left: 0;
        right: 0;
        bottom: 0;
        display: flex;
        justify-content: space-between;
        align-items: center;
        padding: 0 1px;
        pointer-events: none;
      }

      .slider-tick {
        width: 1px;
        height: 3px;
        background: rgba(255,255,255,0.15);
        border-radius: 0.5px;
      }

      .slider-tick-major {
        height: 5px;
        background: rgba(255,255,255,0.25);
      }

      .slider-scale {
        display: flex;
        justify-content: space-between;
        padding: 0 14px;
        margin-bottom: 2px;
      }

      .slider-scale-label {
        font-size: 8px;
        color: #666;
        font-family: 'Courier New', monospace;
        font-weight: 600;
        min-width: 0;
        text-align: center;
      }

      .slider-scale-label:first-child {
        text-align: left;
      }

      .slider-scale-label:last-child {
        text-align: right;
      }

      .slider-thumb {
        position: absolute;
        top: 50%;
        left: 0%;
        transform: translate(-50%, -50%);
        width: 28px;
        height: 34px;
        background: linear-gradient(170deg, #555 0%, #333 50%, #2a2a2a 100%);
        border: 1px solid #666;
        border-radius: 4px;
        box-shadow:
          0 2px 6px rgba(0,0,0,0.5),
          inset 0 1px 0 rgba(255,255,255,0.15),
          inset 0 -1px 0 rgba(0,0,0,0.2);
        cursor: grab;
        display: flex;
        flex-direction: column;
        align-items: center;
        justify-content: center;
        gap: 2px;
        transition: left 0.1s ease-out, box-shadow 0.15s;
        touch-action: none;
      }

      .slider-thumb:hover {
        border-color: #888;
        box-shadow:
          0 2px 8px rgba(0,0,0,0.6),
          inset 0 1px 0 rgba(255,255,255,0.2),
          0 0 6px ${lcd.glow};
      }

      .slider-thumb-active {
        cursor: grabbing !important;
        border-color: ${lcd.on} !important;
        box-shadow:
          0 2px 10px rgba(0,0,0,0.6),
          0 0 10px ${lcd.glow} !important;
      }

      .slider-thumb-grip {
        width: 14px;
        height: 1px;
        background: rgba(255,255,255,0.2);
        border-radius: 0.5px;
      }

      .power-button-area {
        display: flex;
        flex-direction: column;
        align-items: center;
        gap: 6px;
        cursor: pointer;
        user-select: none;
        -webkit-tap-highlight-color: transparent;
      }

      .power-button-area:active .power-btn-svg {
        transform: scale(0.95);
      }

      .power-btn-svg {
        transition: transform 0.15s;
        pointer-events: none;
      }

      .pwr-label {
        font-size: 10px;
        color: #999;
        letter-spacing: 2px;
        font-weight: 600;
        text-transform: uppercase;
      }

      /* ── Temperature panel ── */
      .temp-panel {
        display: flex;
        gap: 16px;
        padding: 8px 0;
        margin-bottom: 8px;
      }

      .temp-gauge {
        flex: 1;
        display: flex;
        align-items: center;
        gap: 8px;
      }

      .temp-label {
        font-size: 8px;
        color: #777;
        letter-spacing: 1px;
        font-weight: 600;
        min-width: 56px;
      }

      .temp-bar-track {
        flex: 1;
        height: 6px;
        background: #1a1a1a;
        border-radius: 3px;
        border: 1px solid #333;
        overflow: hidden;
      }

      .temp-bar-fill {
        height: 100%;
        border-radius: 2px;
        transition: width 0.5s ease, background 0.5s ease;
      }

      .temp-value {
        font-size: 11px;
        color: #aaa;
        font-family: 'Courier New', monospace;
        min-width: 48px;
        text-align: right;
      }

      /* ── Datalog panel ── */
      .datalog-panel {
        background: #1a1a1a;
        border: 1px solid #333;
        border-radius: 6px;
        padding: 10px 12px;
        margin-bottom: 8px;
      }

      .datalog-header {
        display: flex;
        align-items: center;
        gap: 6px;
        margin-bottom: 8px;
      }

      .datalog-title {
        font-size: 9px;
        color: #888;
        letter-spacing: 2px;
        font-weight: 600;
        text-transform: uppercase;
      }

      .datalog-filename-row {
        margin-bottom: 8px;
      }

      .datalog-filename {
        width: 100%;
        background: #111;
        color: #ccc;
        border: 1px solid #444;
        border-radius: 3px;
        padding: 5px 8px;
        font-family: 'Courier New', monospace;
        font-size: 12px;
        box-sizing: border-box;
        outline: none;
      }

      .datalog-filename:focus {
        border-color: #666;
      }

      .datalog-buttons {
        display: flex;
        gap: 8px;
      }

      .datalog-btn {
        flex: 1;
        padding: 6px 0;
        border: 1px solid #444;
        border-radius: 4px;
        background: #2a2a2a;
        color: #aaa;
        font-size: 10px;
        font-weight: 700;
        letter-spacing: 1px;
        cursor: pointer;
        transition: all 0.15s;
        text-transform: uppercase;
        -webkit-tap-highlight-color: transparent;
      }

      .datalog-btn:hover {
        background: #333;
        color: #ddd;
      }

      .datalog-btn:active {
        transform: scale(0.97);
      }

      .datalog-btn-start {
        border-color: #a33;
        color: #f55;
      }

      .datalog-btn-start:hover {
        background: #3a1a1a;
        color: #ff6666;
      }

      .datalog-btn-stop {
        border-color: #555;
      }

      .datalog-btn-flush {
        border-color: #555;
      }

      /* ── Bottom trim ── */
      .bottom-trim {
        margin-top: 4px;
      }

      .ventilation {
        display: flex;
        justify-content: center;
        gap: 6px;
        padding: 4px 0;
      }

      .vent-slot {
        width: 20px;
        height: 3px;
        background: #1a1a1a;
        border-radius: 1px;
        box-shadow: inset 0 1px 1px rgba(0,0,0,0.5);
      }

      /* ── Hidden edit inputs ── */
      .hidden-input {
        display: none;
        position: absolute;
        z-index: 100;
        background: ${lcd.bg};
        color: ${lcd.on};
        border: 2px solid ${lcd.on};
        border-radius: 4px;
        font-family: 'Courier New', monospace;
        font-size: 18px;
        font-weight: bold;
        text-align: center;
        padding: 4px;
        box-sizing: border-box;
        outline: none;
      }

      .hidden-input:focus {
        box-shadow: 0 0 8px ${lcd.glow};
      }

      /* Remove number input spinners */
      .hidden-input::-webkit-outer-spin-button,
      .hidden-input::-webkit-inner-spin-button {
        -webkit-appearance: none;
        margin: 0;
      }
      .hidden-input[type=number] {
        -moz-appearance: textfield;
      }

      /* ── Responsive ── */
      @media (max-width: 350px) {
        .psu-chassis {
          padding: 10px;
        }
        .lcd-panel {
          padding: 10px;
        }
        .controls-panel {
          gap: 4px;
        }
      }
    `;
  }

  // ── Card size for HA layout ────────────────────────────────────────

  getCardSize() {
    return 6;
  }

  static getConfigElement() {
    return document.createElement('opendps-card-editor');
  }

  static getStubConfig() {
    return {
      name: 'OpenDPS',
      entities: {
        voltage_out: '',
        current_out: '',
        output_switch: '',
      },
    };
  }
}


// ── Card Editor ────────────────────────────────────────────────────────

class OpenDPSCardEditor extends HTMLElement {
  constructor() {
    super();
    this.attachShadow({ mode: 'open' });
    this._config = {};
  }

  setConfig(config) {
    this._config = config;
    this._render();
  }

  _render() {
    const ent = this._config.entities || {};
    this.shadowRoot.innerHTML = `
      <style>
        .editor { padding: 16px; font-family: Arial, sans-serif; }
        .section { margin-bottom: 16px; }
        .section-title {
          font-weight: 600; font-size: 14px; margin-bottom: 8px;
          padding-bottom: 4px; border-bottom: 1px solid #ddd;
        }
        .row { margin-bottom: 10px; }
        label { display: block; font-size: 12px; color: #666; margin-bottom: 3px; }
        input, select {
          width: 100%; padding: 8px; border: 1px solid #ccc; border-radius: 4px;
          box-sizing: border-box; font-size: 13px;
        }
        .row-2col { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }
      </style>
      <div class="editor">
        <div class="section">
          <div class="section-title">General</div>
          <div class="row">
            <label>Card Name</label>
            <input type="text" id="name" value="${this._config.name || 'OpenDPS'}">
          </div>
          <div class="row-2col">
            <div class="row">
              <label>LCD Color</label>
              <select id="lcd_color">
                ${['green','amber','blue','white'].map(c =>
                  `<option value="${c}" ${(this._config.lcd_color || 'green') === c ? 'selected' : ''}>${c}</option>`
                ).join('')}
              </select>
            </div>
          </div>
        </div>

        <div class="section">
          <div class="section-title">Sensor Entities</div>
          ${this._entityRow('voltage_out', 'Output Voltage', ent)}
          ${this._entityRow('current_out', 'Output Current', ent)}
          ${this._entityRow('power_out', 'Output Power', ent)}
          ${this._entityRow('voltage_in', 'Input Voltage', ent)}
          ${this._entityRow('temp1', 'Heatsink Temp', ent)}
          ${this._entityRow('temp2', 'Ambient Temp', ent)}
        </div>

        <div class="section">
          <div class="section-title">Control Entities</div>
          ${this._entityRow('output_switch', 'Output Switch', ent)}
          ${this._entityRow('output_enabled', 'Output Binary Sensor', ent)}
          ${this._entityRow('set_voltage', 'Set Voltage Number', ent)}
          ${this._entityRow('set_current', 'Set Current Number', ent)}
          ${this._entityRow('operating_mode', 'Operating Mode Select', ent)}
        </div>

        <div class="section">
          <div class="section-title">Datalog Entities</div>
          ${this._entityRow('datalog_start', 'Start Datalog Button', ent)}
          ${this._entityRow('datalog_stop', 'Stop Datalog Button', ent)}
          ${this._entityRow('datalog_flush', 'Flush Datalog Button', ent)}
          ${this._entityRow('datalog_filename', 'Filename Text Input', ent)}
        </div>
      </div>
    `;

    // Attach listeners
    const fields = ['name', 'lcd_color',
      'voltage_out', 'current_out', 'power_out', 'voltage_in',
      'temp1', 'temp2', 'output_switch', 'output_enabled',
      'set_voltage', 'set_current', 'operating_mode',
      'datalog_start', 'datalog_stop', 'datalog_flush', 'datalog_filename'];

    fields.forEach(id => {
      const el = this.shadowRoot.getElementById(id);
      if (el) {
        el.addEventListener('change', () => this._fireChanged());
        el.addEventListener('input', () => this._fireChanged());
      }
    });
  }

  _entityRow(id, label, ent) {
    return `
      <div class="row">
        <label>${label}</label>
        <input type="text" id="${id}" value="${ent[id] || ''}"
               placeholder="sensor.opendps_...">
      </div>`;
  }

  _fireChanged() {
    const g = (id) => {
      const el = this.shadowRoot.getElementById(id);
      return el ? el.value : '';
    };

    const entities = {};
    ['voltage_out', 'current_out', 'power_out', 'voltage_in',
     'temp1', 'temp2', 'output_switch', 'output_enabled',
     'set_voltage', 'set_current', 'operating_mode',
     'datalog_start', 'datalog_stop', 'datalog_flush', 'datalog_filename'].forEach(id => {
      const v = g(id);
      if (v) entities[id] = v;
    });

    const config = {
      ...this._config,
      name: g('name') || 'OpenDPS',
      lcd_color: g('lcd_color') || 'green',
      entities,
    };

    this.dispatchEvent(new CustomEvent('config-changed', {
      detail: { config },
      bubbles: true,
      composed: true,
    }));
  }
}


// ── Register ─────────────────────────────────────────────────────────

customElements.define('opendps-card', OpenDPSCard);
customElements.define('opendps-card-editor', OpenDPSCardEditor);

window.customCards = window.customCards || [];
window.customCards.push({
  type: 'opendps-card',
  name: 'OpenDPS Virtual Lab PSU',
  description: 'A skeuomorphic lab power supply card for OpenDPS devices via ESPHome',
  preview: true,
  documentationURL: 'https://github.com/p1ngb4ck/esphome',
});

console.info(
  '%c OPENDPS-CARD %c v2.1.0 %c Virtual Lab PSU ',
  'color: white; background: #39ff14; font-weight: bold;',
  'color: #39ff14; background: #222; font-weight: bold;',
  'color: #888; background: #222;'
);
