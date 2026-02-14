/**
 * OpenDPS Virtual Lab PSU Card for Home Assistant
 *
 * A skeuomorphic lab power supply card with:
 * - LCD seven-segment displays (voltage, current, power)
 * - Rotary encoder knobs for V/I adjustment
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

    // Knob interaction state
    this._voltageKnobAngle = 0;
    this._currentKnobAngle = 0;
    this._activeKnob = null;
    this._knobStartAngle = 0;
    this._knobStartValue = 0;
    this._pendingVoltage = null;
    this._pendingCurrent = null;
    this._knobTimer = null;

    // Direct input state
    this._editingVoltage = false;
    this._editingCurrent = false;

    // Bind handlers once
    this._onPointerMove = this._onPointerMove.bind(this);
    this._onPointerUp = this._onPointerUp.bind(this);
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

  // ── Rotary knob SVG ────────────────────────────────────────────────

  _buildKnobSVG(id, label, size) {
    const r = size / 2;
    const cx = r;
    const cy = r;
    return `
      <svg class="knob-svg" id="knob-${id}" viewBox="0 0 ${size} ${size}"
           width="${size}" height="${size}" style="cursor:grab;">
        <defs>
          <radialGradient id="knob-grad-${id}" cx="40%" cy="35%">
            <stop offset="0%" stop-color="#888"/>
            <stop offset="50%" stop-color="#555"/>
            <stop offset="100%" stop-color="#222"/>
          </radialGradient>
          <radialGradient id="knob-ring-${id}" cx="50%" cy="50%">
            <stop offset="85%" stop-color="transparent"/>
            <stop offset="90%" stop-color="#333"/>
            <stop offset="95%" stop-color="#666"/>
            <stop offset="100%" stop-color="#333"/>
          </radialGradient>
          <filter id="knob-shadow-${id}">
            <feDropShadow dx="0" dy="2" stdDeviation="3" flood-opacity="0.5"/>
          </filter>
        </defs>
        <!-- Outer ring with tick marks -->
        <circle cx="${cx}" cy="${cy}" r="${r - 2}" fill="#1a1a1a" stroke="#444" stroke-width="1"/>
        ${this._buildTickMarks(cx, cy, r - 4, 30)}
        <!-- Knob body -->
        <circle cx="${cx}" cy="${cy}" r="${r - 14}" fill="url(#knob-grad-${id})"
                stroke="#222" stroke-width="1.5" filter="url(#knob-shadow-${id})"/>
        <!-- Grip lines -->
        ${this._buildGripLines(cx, cy, r - 18)}
        <!-- Indicator line (rotated by JS) -->
        <line class="knob-indicator" id="knob-indicator-${id}"
              x1="${cx}" y1="${cy - r + 20}" x2="${cx}" y2="${cy - r + 30}"
              stroke="#ff3333" stroke-width="2.5" stroke-linecap="round"
              transform="rotate(0, ${cx}, ${cy})"/>
        <!-- Label -->
        <text x="${cx}" y="${size + 16}" text-anchor="middle"
              font-size="11" fill="#999" font-family="Arial, sans-serif">${label}</text>
      </svg>`;
  }

  _buildTickMarks(cx, cy, r, count) {
    let marks = '';
    for (let i = 0; i < count; i++) {
      const angle = (i / count) * 360 - 135;
      const rad = (angle * Math.PI) / 180;
      const isMajor = i % 5 === 0;
      const innerR = r - (isMajor ? 8 : 4);
      const x1 = cx + innerR * Math.cos(rad);
      const y1 = cy + innerR * Math.sin(rad);
      const x2 = cx + r * Math.cos(rad);
      const y2 = cy + r * Math.sin(rad);
      marks += `<line x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}"
                      stroke="${isMajor ? '#888' : '#555'}"
                      stroke-width="${isMajor ? 1.5 : 0.8}"/>`;
    }
    return marks;
  }

  _buildGripLines(cx, cy, r) {
    let lines = '';
    for (let i = 0; i < 12; i++) {
      const angle = (i / 12) * 360;
      const rad = (angle * Math.PI) / 180;
      const x1 = cx + (r - 8) * Math.cos(rad);
      const y1 = cy + (r - 8) * Math.sin(rad);
      const x2 = cx + r * Math.cos(rad);
      const y2 = cy + r * Math.sin(rad);
      lines += `<line x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}"
                      stroke="#2a2a2a" stroke-width="1.5" stroke-linecap="round"/>`;
    }
    return lines;
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

  _buildLED(color, isOn, label) {
    const fill = isOn ? color : '#333';
    const glow = isOn ? `drop-shadow(0 0 4px ${color})` : 'none';
    return `
      <div class="led-indicator">
        <svg width="14" height="14" viewBox="0 0 14 14">
          <circle cx="7" cy="7" r="5" fill="${fill}"
                  stroke="#222" stroke-width="1.5"
                  style="filter:${glow}; transition: all 0.3s;"/>
          <circle cx="5" cy="5" r="1.5" fill="rgba(255,255,255,${isOn ? 0.4 : 0.1})"/>
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
                <div class="lcd-digits" id="lcd-voltage" title="Click to edit">
                  <!-- filled by JS -->
                </div>
                <div class="lcd-unit" style="color:${lcd.on};">V</div>
              </div>
              <div class="lcd-display-group">
                <div class="lcd-label" style="color:${lcd.on};">CURRENT</div>
                <div class="lcd-digits" id="lcd-current" title="Click to edit">
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
                  <div class="lcd-digits lcd-digits-sm" id="lcd-set-voltage"></div>
                  <div class="lcd-unit lcd-unit-sm" style="color:${lcd.on};">V</div>
                </div>
                <div class="lcd-display-group lcd-small">
                  <div class="lcd-label lcd-label-sm" style="color:${lcd.on};">SET A</div>
                  <div class="lcd-digits lcd-digits-sm" id="lcd-set-current"></div>
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
            <div class="knob-area" id="knob-voltage-area">
              ${this._buildKnobSVG('voltage', 'VOLTAGE', 90)}
            </div>
            <div class="power-button-area" id="power-button">
              ${this._buildPowerButton(false)}
              <div class="pwr-label" id="pwr-label">OUTPUT</div>
            </div>
            <div class="knob-area" id="knob-current-area">
              ${this._buildKnobSVG('current', 'CURRENT', 90)}
            </div>
          </div>

          <!-- Temperature gauges -->
          ${this._config.show_temperatures ? `
            <div class="temp-panel" id="temp-panel">
              <!-- filled by JS -->
            </div>
          ` : ''}

          <!-- Bottom trim -->
          <div class="bottom-trim">
            <div class="ventilation">
              ${Array(12).fill('<div class="vent-slot"></div>').join('')}
            </div>
          </div>
        </div>

        <!-- Hidden inputs for direct value entry -->
        <input type="number" id="voltage-edit-input" class="hidden-input"
               step="${this._config.voltage_step}" min="0">
        <input type="number" id="current-edit-input" class="hidden-input"
               step="${this._config.current_step}" min="0">
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
      this._updateLCD('lcd-set-voltage', setV, 4, dp.decimal_places_voltage, lcd, true);
      this._updateLCD('lcd-set-current', setI, 4, dp.decimal_places_current, lcd, true);
    }

    // ── Input voltage ──
    const ivEl = this.shadowRoot.getElementById('iv-value');
    if (ivEl) {
      ivEl.textContent = vIn !== null ? `${vIn.toFixed(1)} V` : '--.- V';
    }

    // ── Power button ──
    // Update SVG and label inside the stable #power-button container
    // without replacing its innerHTML (which would break event delegation targets)
    const pwrBtn = this.shadowRoot.getElementById('power-button');
    if (pwrBtn) {
      // Update SVG: replace only the SVG element
      const oldSvg = pwrBtn.querySelector('.power-btn-svg');
      if (oldSvg) {
        const temp = document.createElement('div');
        temp.innerHTML = this._buildPowerButton(isOn);
        const newSvg = temp.firstElementChild;
        oldSvg.replaceWith(newSvg);
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
        this._buildLED('#39ff14', modeStr === 'cv', 'CV') +
        this._buildLED('#ffbf00', modeStr === 'cc', 'CC') +
        this._buildLED('#ff3333', modeStr === 'cp', 'CP') +
        this._buildLED(isOn ? '#39ff14' : '#555', isOn, 'OUT');
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

    // ── Update knob indicators ──
    if (setV !== null && this._activeKnob !== 'voltage') {
      this._updateKnobAngle('voltage', setV);
    }
    if (setI !== null && this._activeKnob !== 'current') {
      this._updateKnobAngle('current', setI);
    }
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

  _updateKnobAngle(which, value) {
    // Map value to angle: 0 -> -135°, max -> +135°
    const ent = this._config.entities;
    let min = 0, max = 30;
    if (this._hass) {
      const entityId = which === 'voltage' ? ent.set_voltage : ent.set_current;
      if (entityId && this._hass.states[entityId]) {
        const attrs = this._hass.states[entityId].attributes;
        min = attrs.min || 0;
        max = attrs.max || (which === 'voltage' ? 30 : 5);
      }
    }
    const pct = Math.max(0, Math.min(1, (value - min) / (max - min)));
    const angle = -135 + pct * 270;

    const indicator = this.shadowRoot.getElementById(`knob-indicator-${which}`);
    if (indicator) {
      indicator.setAttribute('transform', `rotate(${angle}, 45, 45)`);
    }
  }

  // ── Event listeners ────────────────────────────────────────────────
  // Uses event delegation on .psu-chassis so innerHTML replacements
  // on children (power button, LCD digits, LEDs) don't destroy listeners.

  _attachEventListeners() {
    const chassis = this.shadowRoot.querySelector('.psu-chassis');
    if (!chassis) return;

    // Single delegated click handler for all interactive elements
    chassis.addEventListener('click', (e) => {
      const target = e.target;

      // Power button - check if click is inside #power-button
      if (target.closest('#power-button')) {
        this._toggleOutput();
        return;
      }

      // LCD click-to-edit - check by id, walking up from target
      const lcdDigits = target.closest('.lcd-digits');
      if (lcdDigits) {
        const id = lcdDigits.id;
        if (id === 'lcd-voltage' || id === 'lcd-set-voltage') {
          this._startEdit('voltage');
        } else if (id === 'lcd-current' || id === 'lcd-set-current') {
          this._startEdit('current');
        }
        return;
      }
    });

    // Knob interactions (these attach to stable container elements)
    this._setupKnob('voltage');
    this._setupKnob('current');

    // Hidden inputs - these are never replaced by innerHTML so direct listeners are fine
    const vInput = this.shadowRoot.getElementById('voltage-edit-input');
    if (vInput) {
      vInput.addEventListener('change', (e) => {
        this._setVoltage(parseFloat(e.target.value));
        this._endEdit('voltage');
      });
      vInput.addEventListener('keydown', (e) => {
        if (e.key === 'Enter') {
          this._setVoltage(parseFloat(e.target.value));
          this._endEdit('voltage');
        } else if (e.key === 'Escape') {
          this._endEdit('voltage');
        }
      });
      vInput.addEventListener('blur', () => this._endEdit('voltage'));
    }

    const iInput = this.shadowRoot.getElementById('current-edit-input');
    if (iInput) {
      iInput.addEventListener('change', (e) => {
        this._setCurrent(parseFloat(e.target.value));
        this._endEdit('current');
      });
      iInput.addEventListener('keydown', (e) => {
        if (e.key === 'Enter') {
          this._setCurrent(parseFloat(e.target.value));
          this._endEdit('current');
        } else if (e.key === 'Escape') {
          this._endEdit('current');
        }
      });
      iInput.addEventListener('blur', () => this._endEdit('current'));
    }
  }

  _setupKnob(which) {
    const area = this.shadowRoot.getElementById(`knob-${which}-area`);
    if (!area) return;

    // Pointer down to start drag
    area.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      this._activeKnob = which;
      const rect = area.getBoundingClientRect();
      const cx = rect.left + rect.width / 2;
      const cy = rect.top + rect.height / 2;
      this._knobCenter = { x: cx, y: cy };
      this._knobStartAngle = Math.atan2(e.clientY - cy, e.clientX - cx);

      const ent = this._config.entities;
      const entityId = which === 'voltage' ? ent.set_voltage : ent.set_current;
      this._knobStartValue = this._getNumber(entityId) || 0;

      area.setPointerCapture(e.pointerId);
      area.style.cursor = 'grabbing';

      const onUp = () => {
        this._onPointerUp();
        area.style.cursor = 'grab';
        area.removeEventListener('pointermove', this._onPointerMove);
        area.removeEventListener('pointerup', onUp);
      };
      area.addEventListener('pointermove', this._onPointerMove);
      area.addEventListener('pointerup', onUp);
    });

    // Mouse wheel for fine adjustment
    area.addEventListener('wheel', (e) => {
      e.preventDefault();
      const ent = this._config.entities;
      const entityId = which === 'voltage' ? ent.set_voltage : ent.set_current;
      const current = this._getNumber(entityId) || 0;
      const step = which === 'voltage' ? this._config.voltage_step : this._config.current_step;
      const delta = e.deltaY < 0 ? step : -step;
      const newVal = Math.max(0, current + delta);

      if (which === 'voltage') {
        this._setVoltage(newVal);
      } else {
        this._setCurrent(newVal);
      }
    }, { passive: false });
  }

  _onPointerMove(e) {
    if (!this._activeKnob || !this._knobCenter) return;

    const angle = Math.atan2(
      e.clientY - this._knobCenter.y,
      e.clientX - this._knobCenter.x
    );
    const deltaAngle = angle - this._knobStartAngle;

    // Convert angle delta to value delta
    // Full rotation (2π) = full range
    const ent = this._config.entities;
    const entityId = this._activeKnob === 'voltage'
      ? ent.set_voltage : ent.set_current;

    let max = this._activeKnob === 'voltage' ? 30 : 5;
    let min = 0;
    if (this._hass && entityId && this._hass.states[entityId]) {
      const attrs = this._hass.states[entityId].attributes;
      min = attrs.min || 0;
      max = attrs.max || max;
    }

    const range = max - min;
    const sensitivity = range / (1.5 * Math.PI); // 270° = full range
    const delta = deltaAngle * sensitivity;
    const step = this._activeKnob === 'voltage'
      ? this._config.voltage_step : this._config.current_step;

    let newVal = this._knobStartValue + delta;
    newVal = Math.round(newVal / step) * step;
    newVal = Math.max(min, Math.min(max, newVal));

    // Update visual immediately
    this._updateKnobAngle(this._activeKnob, newVal);

    // Debounce the service call
    if (this._activeKnob === 'voltage') {
      this._pendingVoltage = newVal;
    } else {
      this._pendingCurrent = newVal;
    }

    clearTimeout(this._knobTimer);
    this._knobTimer = setTimeout(() => {
      if (this._pendingVoltage !== null) {
        this._setVoltage(this._pendingVoltage);
        this._pendingVoltage = null;
      }
      if (this._pendingCurrent !== null) {
        this._setCurrent(this._pendingCurrent);
        this._pendingCurrent = null;
      }
    }, 100);
  }

  _onPointerUp() {
    // Commit any pending value
    if (this._pendingVoltage !== null) {
      this._setVoltage(this._pendingVoltage);
      this._pendingVoltage = null;
    }
    if (this._pendingCurrent !== null) {
      this._setCurrent(this._pendingCurrent);
      this._pendingCurrent = null;
    }
    this._activeKnob = null;
    this._knobCenter = null;
  }

  // ── Direct edit mode ───────────────────────────────────────────────

  _startEdit(which) {
    const ent = this._config.entities;
    const entityId = which === 'voltage' ? ent.set_voltage : ent.set_current;
    const currentVal = this._getNumber(entityId);

    const inputId = which === 'voltage' ? 'voltage-edit-input' : 'current-edit-input';
    const input = this.shadowRoot.getElementById(inputId);
    if (!input) return;

    // Position over the LCD
    const lcdEl = this.shadowRoot.getElementById(
      which === 'voltage' ? 'lcd-voltage' : 'lcd-current'
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
    if (!entityId || !this._hass) return;
    const isOn = this._isOn(entityId);
    this._hass.callService('switch', isOn ? 'turn_off' : 'turn_on', {
      entity_id: entityId
    });
  }

  _setVoltage(value) {
    const entityId = this._config.entities.set_voltage;
    if (!entityId || !this._hass || isNaN(value)) return;
    this._hass.callService('number', 'set_value', {
      entity_id: entityId,
      value: parseFloat(value.toFixed(this._config.decimal_places_voltage))
    });
  }

  _setCurrent(value) {
    const entityId = this._config.entities.set_current;
    if (!entityId || !this._hass || isNaN(value)) return;
    this._hass.callService('number', 'set_value', {
      entity_id: entityId,
      value: parseFloat(value.toFixed(this._config.decimal_places_current))
    });
  }

  // ── Styles ─────────────────────────────────────────────────────────

  _getStyles(lcd) {
    return `
      :host {
        display: block;
      }

      ha-card {
        overflow: hidden;
        background: transparent !important;
        box-shadow: none !important;
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
        cursor: pointer;
        transition: opacity 0.2s;
        min-height: 40px;
        display: flex;
        align-items: center;
      }

      .lcd-digits:hover {
        opacity: 0.85;
      }

      .lcd-digits-sm {
        min-height: 26px;
        cursor: pointer;
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

      .knob-area {
        display: flex;
        flex-direction: column;
        align-items: center;
        user-select: none;
        touch-action: none;
      }

      .knob-svg {
        cursor: grab;
        transition: filter 0.2s;
      }

      .knob-svg:hover {
        filter: brightness(1.1);
      }

      .power-button-area {
        display: flex;
        flex-direction: column;
        align-items: center;
        gap: 6px;
        cursor: pointer;
        user-select: none;
      }

      .power-button-area:active .power-btn-svg {
        transform: scale(0.95);
      }

      .power-btn-svg {
        transition: transform 0.15s;
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
      </div>
    `;

    // Attach listeners
    const fields = ['name', 'lcd_color',
      'voltage_out', 'current_out', 'power_out', 'voltage_in',
      'temp1', 'temp2', 'output_switch', 'output_enabled',
      'set_voltage', 'set_current', 'operating_mode'];

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
     'set_voltage', 'set_current', 'operating_mode'].forEach(id => {
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
  '%c OPENDPS-CARD %c v2.0.0 %c Virtual Lab PSU ',
  'color: white; background: #39ff14; font-weight: bold;',
  'color: #39ff14; background: #222; font-weight: bold;',
  'color: #888; background: #222;'
);
