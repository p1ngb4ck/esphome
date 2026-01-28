/**
 * OpenDPS Custom Lovelace Card for Home Assistant
 *
 * A feature-rich dashboard card for controlling and monitoring OpenDPS power supplies
 * via ESPHome integration.
 *
 * Features:
 * - Real-time voltage, current, and power display
 * - Output enable/disable control
 * - Voltage and current setpoint adjustment
 * - Calibration controls
 * - Firmware upgrade status
 * - Light/Dark theme support (auto or manual)
 *
 * Installation:
 * 1. Copy this file to your Home Assistant config/www/ directory
 * 2. Add to Lovelace resources:
 *    url: /local/opendps-card.js
 *    type: module
 * 3. Add the card to your dashboard (see example config below)
 *
 * Example card configuration:
 * type: custom:opendps-card
 * name: "My DPS5005"
 * theme: auto  # auto, light, or dark
 * entities:
 *   voltage_in: sensor.opendps_input_voltage
 *   voltage_out: sensor.opendps_output_voltage
 *   current_out: sensor.opendps_output_current
 *   power_out: sensor.opendps_output_power
 *   output_enabled: binary_sensor.opendps_output_state
 *   set_voltage: number.opendps_set_voltage
 *   set_current: number.opendps_set_current_limit
 *   output_switch: switch.opendps_output_enable
 *   temp1: sensor.opendps_heatsink_temperature
 *   temp2: sensor.opendps_ambient_temperature
 * show_calibration: false
 * show_temperatures: true
 * compact: false
 */

class OpenDPSCard extends HTMLElement {
  constructor() {
    super();
    this.attachShadow({ mode: 'open' });
    this._config = {};
    this._hass = null;
    this._calibrationExpanded = false;  // Collapsed by default
    this._firmwareExpanded = false;     // Collapsed by default
  }

  static get properties() {
    return {
      hass: { type: Object },
      config: { type: Object },
    };
  }

  setConfig(config) {
    if (!config.entities) {
      throw new Error('You need to define entities');
    }

    this._config = {
      name: config.name || 'OpenDPS',
      theme: config.theme || 'auto',
      show_calibration: config.show_calibration || false,
      show_firmware: config.show_firmware || false,
      show_temperatures: config.show_temperatures !== false,
      show_input_voltage: config.show_input_voltage !== false,
      compact: config.compact || false,
      decimal_places_voltage: config.decimal_places_voltage ?? 2,
      decimal_places_current: config.decimal_places_current ?? 3,
      decimal_places_power: config.decimal_places_power ?? 2,
      entities: config.entities,
      // Custom colors (optional)
      colors: {
        voltage: config.colors?.voltage || '#4CAF50',
        current: config.colors?.current || '#2196F3',
        power: config.colors?.power || '#FF9800',
        temperature: config.colors?.temperature || '#F44336',
        ...config.colors
      }
    };

    this.render();
  }

  set hass(hass) {
    this._hass = hass;
    this.render();
  }

  get hass() {
    return this._hass;
  }

  _getState(entityId) {
    if (!this._hass || !entityId) return null;
    const state = this._hass.states[entityId];
    return state ? state.state : null;
  }

  _getNumericState(entityId, decimals = 2) {
    const state = this._getState(entityId);
    if (state === null || state === 'unavailable' || state === 'unknown') {
      return '--';
    }
    const num = parseFloat(state);
    return isNaN(num) ? '--' : num.toFixed(decimals);
  }

  _getUnit(entityId) {
    if (!this._hass || !entityId) return '';
    const state = this._hass.states[entityId];
    return state?.attributes?.unit_of_measurement || '';
  }

  _isOn(entityId) {
    const state = this._getState(entityId);
    return state === 'on' || state === 'true' || state === '1';
  }

  _callService(domain, service, data) {
    this._hass.callService(domain, service, data);
  }

  _toggleOutput() {
    const entityId = this._config.entities.output_switch;
    if (!entityId) return;

    const isOn = this._isOn(entityId);
    this._callService('switch', isOn ? 'turn_off' : 'turn_on', {
      entity_id: entityId
    });
  }

  _setVoltage(value) {
    const entityId = this._config.entities.set_voltage;
    if (!entityId) return;

    this._callService('number', 'set_value', {
      entity_id: entityId,
      value: parseFloat(value)
    });
  }

  _setCurrent(value) {
    const entityId = this._config.entities.set_current;
    if (!entityId) return;

    this._callService('number', 'set_value', {
      entity_id: entityId,
      value: parseFloat(value)
    });
  }

  _pressButton(entityId) {
    if (!entityId) return;
    this._callService('button', 'press', {
      entity_id: entityId
    });
  }

  _toggleCalibration() {
    this._calibrationExpanded = !this._calibrationExpanded;
    this.render();
  }

  _toggleFirmware() {
    this._firmwareExpanded = !this._firmwareExpanded;
    this.render();
  }

  _getTheme() {
    if (this._config.theme === 'light') return 'light';
    if (this._config.theme === 'dark') return 'dark';

    // Auto-detect from HA theme
    if (this._hass) {
      const theme = this._hass.themes?.darkMode;
      return theme ? 'dark' : 'light';
    }

    // Fallback to system preference
    return window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';
  }

  _getStyles() {
    const theme = this._getTheme();
    const isDark = theme === 'dark';

    const colors = {
      // Base colors
      background: isDark ? '#1c1c1c' : '#ffffff',
      cardBackground: isDark ? '#2d2d2d' : '#f5f5f5',
      text: isDark ? '#e0e0e0' : '#212121',
      textSecondary: isDark ? '#9e9e9e' : '#757575',
      border: isDark ? '#404040' : '#e0e0e0',

      // Status colors
      outputOn: '#4CAF50',
      outputOff: isDark ? '#616161' : '#9e9e9e',
      warning: '#FF9800',
      error: '#F44336',

      // Accent colors from config
      voltage: this._config.colors.voltage,
      current: this._config.colors.current,
      power: this._config.colors.power,
      temperature: this._config.colors.temperature,

      // Interactive elements
      buttonBackground: isDark ? '#424242' : '#e0e0e0',
      buttonHover: isDark ? '#616161' : '#bdbdbd',
      inputBackground: isDark ? '#1c1c1c' : '#ffffff',
      inputBorder: isDark ? '#616161' : '#bdbdbd',

      // Shadows
      shadow: isDark ? 'rgba(0,0,0,0.3)' : 'rgba(0,0,0,0.1)',
    };

    return `
      :host {
        --opendps-background: ${colors.background};
        --opendps-card-background: ${colors.cardBackground};
        --opendps-text: ${colors.text};
        --opendps-text-secondary: ${colors.textSecondary};
        --opendps-border: ${colors.border};
        --opendps-output-on: ${colors.outputOn};
        --opendps-output-off: ${colors.outputOff};
        --opendps-voltage: ${colors.voltage};
        --opendps-current: ${colors.current};
        --opendps-power: ${colors.power};
        --opendps-temperature: ${colors.temperature};
        --opendps-button-bg: ${colors.buttonBackground};
        --opendps-button-hover: ${colors.buttonHover};
        --opendps-input-bg: ${colors.inputBackground};
        --opendps-input-border: ${colors.inputBorder};
        --opendps-shadow: ${colors.shadow};
      }

      .card {
        background: var(--opendps-background);
        border-radius: 12px;
        padding: 16px;
        font-family: 'Roboto', 'Segoe UI', sans-serif;
        color: var(--opendps-text);
        box-shadow: 0 2px 8px var(--opendps-shadow);
      }

      .header {
        display: flex;
        justify-content: space-between;
        align-items: center;
        margin-bottom: 16px;
        padding-bottom: 12px;
        border-bottom: 1px solid var(--opendps-border);
      }

      .title {
        font-size: 1.2em;
        font-weight: 500;
        margin: 0;
      }

      .status-indicator {
        display: flex;
        align-items: center;
        gap: 8px;
      }

      .status-dot {
        width: 12px;
        height: 12px;
        border-radius: 50%;
        transition: background-color 0.3s ease;
      }

      .status-dot.on {
        background: var(--opendps-output-on);
        box-shadow: 0 0 8px var(--opendps-output-on);
      }

      .status-dot.off {
        background: var(--opendps-output-off);
      }

      .status-text {
        font-size: 0.85em;
        color: var(--opendps-text-secondary);
        text-transform: uppercase;
        letter-spacing: 0.5px;
      }

      .measurements {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(120px, 1fr));
        gap: 12px;
        margin-bottom: 16px;
      }

      .measurement {
        background: var(--opendps-card-background);
        border-radius: 8px;
        padding: 12px;
        text-align: center;
        transition: transform 0.2s ease;
      }

      .measurement:hover {
        transform: translateY(-2px);
      }

      .measurement-value {
        font-size: 1.8em;
        font-weight: 600;
        font-variant-numeric: tabular-nums;
        line-height: 1.2;
      }

      .measurement-value.voltage { color: var(--opendps-voltage); }
      .measurement-value.current { color: var(--opendps-current); }
      .measurement-value.power { color: var(--opendps-power); }
      .measurement-value.temperature { color: var(--opendps-temperature); }

      .measurement-unit {
        font-size: 0.7em;
        opacity: 0.8;
        margin-left: 2px;
      }

      .measurement-label {
        font-size: 0.75em;
        color: var(--opendps-text-secondary);
        text-transform: uppercase;
        letter-spacing: 0.5px;
        margin-top: 4px;
      }

      .controls {
        display: grid;
        grid-template-columns: 1fr 1fr;
        gap: 12px;
        margin-bottom: 16px;
      }

      .control-group {
        background: var(--opendps-card-background);
        border-radius: 8px;
        padding: 12px;
      }

      .control-label {
        font-size: 0.75em;
        color: var(--opendps-text-secondary);
        text-transform: uppercase;
        letter-spacing: 0.5px;
        margin-bottom: 8px;
      }

      .control-input-group {
        display: flex;
        align-items: center;
        gap: 8px;
      }

      .control-input {
        flex: 1;
        background: var(--opendps-input-bg);
        border: 1px solid var(--opendps-input-border);
        border-radius: 6px;
        padding: 8px 12px;
        font-size: 1.1em;
        color: var(--opendps-text);
        text-align: center;
        font-variant-numeric: tabular-nums;
        outline: none;
        transition: border-color 0.2s ease;
      }

      .control-input:focus {
        border-color: var(--opendps-voltage);
      }

      .control-input.current:focus {
        border-color: var(--opendps-current);
      }

      .control-unit {
        font-size: 0.9em;
        color: var(--opendps-text-secondary);
        min-width: 24px;
      }

      .control-buttons {
        display: flex;
        gap: 4px;
      }

      .btn {
        background: var(--opendps-button-bg);
        border: none;
        border-radius: 6px;
        padding: 8px 12px;
        font-size: 0.9em;
        color: var(--opendps-text);
        cursor: pointer;
        transition: background-color 0.2s ease, transform 0.1s ease;
        user-select: none;
      }

      .btn:hover {
        background: var(--opendps-button-hover);
      }

      .btn:active {
        transform: scale(0.95);
      }

      .btn-small {
        padding: 6px 10px;
        font-size: 0.8em;
      }

      .btn-icon {
        padding: 8px;
        min-width: 36px;
        display: flex;
        align-items: center;
        justify-content: center;
      }

      .output-toggle {
        width: 100%;
        padding: 16px;
        font-size: 1.1em;
        font-weight: 500;
        border-radius: 8px;
        transition: all 0.3s ease;
      }

      .output-toggle.on {
        background: var(--opendps-output-on);
        color: white;
      }

      .output-toggle.off {
        background: var(--opendps-button-bg);
      }

      .output-toggle.on:hover {
        background: #43A047;
      }

      .temperatures {
        display: flex;
        gap: 16px;
        padding: 12px;
        background: var(--opendps-card-background);
        border-radius: 8px;
        margin-bottom: 16px;
      }

      .temperature-item {
        display: flex;
        align-items: center;
        gap: 8px;
      }

      .temperature-icon {
        color: var(--opendps-temperature);
        font-size: 1.2em;
      }

      .temperature-value {
        font-size: 1em;
        font-variant-numeric: tabular-nums;
      }

      .temperature-label {
        font-size: 0.75em;
        color: var(--opendps-text-secondary);
      }

      .input-voltage {
        display: flex;
        align-items: center;
        gap: 8px;
        padding: 8px 12px;
        background: var(--opendps-card-background);
        border-radius: 8px;
        margin-bottom: 16px;
        font-size: 0.9em;
      }

      .input-voltage-label {
        color: var(--opendps-text-secondary);
      }

      .input-voltage-value {
        font-weight: 500;
        font-variant-numeric: tabular-nums;
      }

      .calibration-section {
        border-top: 1px solid var(--opendps-border);
        padding-top: 16px;
        margin-top: 16px;
      }

      .calibration-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        cursor: pointer;
        padding: 8px;
        margin: -8px;
        border-radius: 6px;
        transition: background-color 0.2s ease;
        user-select: none;
      }

      .calibration-header:hover {
        background: var(--opendps-card-background);
      }

      .calibration-title {
        font-size: 0.85em;
        font-weight: 500;
        display: flex;
        align-items: center;
        gap: 8px;
      }

      .calibration-chevron {
        font-size: 0.9em;
        transition: transform 0.3s ease;
        color: var(--opendps-text-secondary);
      }

      .calibration-chevron.expanded {
        transform: rotate(180deg);
      }

      .calibration-content {
        overflow: hidden;
        max-height: 0;
        opacity: 0;
        transition: max-height 0.3s ease, opacity 0.2s ease, margin-top 0.3s ease;
        margin-top: 0;
      }

      .calibration-content.expanded {
        max-height: 200px;
        opacity: 1;
        margin-top: 12px;
      }

      .calibration-buttons {
        display: flex;
        flex-wrap: wrap;
        gap: 8px;
      }

      /* Firmware upgrade section - reuses calibration styles */
      .firmware-section {
        border-top: 1px solid var(--opendps-border);
        padding-top: 16px;
        margin-top: 16px;
      }

      .firmware-header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        cursor: pointer;
        padding: 8px;
        margin: -8px;
        border-radius: 6px;
        transition: background-color 0.2s ease;
        user-select: none;
      }

      .firmware-header:hover {
        background: var(--opendps-card-background);
      }

      .firmware-title {
        font-size: 0.85em;
        font-weight: 500;
        display: flex;
        align-items: center;
        gap: 8px;
      }

      .firmware-chevron {
        font-size: 0.9em;
        transition: transform 0.3s ease;
        color: var(--opendps-text-secondary);
      }

      .firmware-chevron.expanded {
        transform: rotate(180deg);
      }

      .firmware-content {
        overflow: hidden;
        max-height: 0;
        opacity: 0;
        transition: max-height 0.3s ease, opacity 0.2s ease, margin-top 0.3s ease;
        margin-top: 0;
      }

      .firmware-content.expanded {
        max-height: 300px;
        opacity: 1;
        margin-top: 12px;
      }

      .firmware-status {
        display: flex;
        align-items: center;
        gap: 8px;
        padding: 8px 12px;
        background: var(--opendps-card-background);
        border-radius: 6px;
        margin-bottom: 12px;
        font-size: 0.9em;
      }

      .firmware-status-label {
        color: var(--opendps-text-secondary);
      }

      .firmware-status-value {
        font-weight: 500;
      }

      .firmware-progress {
        margin-bottom: 12px;
      }

      .firmware-progress-bar {
        height: 8px;
        background: var(--opendps-card-background);
        border-radius: 4px;
        overflow: hidden;
      }

      .firmware-progress-fill {
        height: 100%;
        background: var(--opendps-voltage);
        transition: width 0.3s ease;
      }

      .firmware-progress-text {
        font-size: 0.8em;
        color: var(--opendps-text-secondary);
        text-align: center;
        margin-top: 4px;
      }

      .firmware-buttons {
        display: flex;
        flex-wrap: wrap;
        gap: 8px;
      }

      .compact .measurements {
        grid-template-columns: repeat(4, 1fr);
      }

      .compact .measurement {
        padding: 8px;
      }

      .compact .measurement-value {
        font-size: 1.4em;
      }

      .compact .controls {
        grid-template-columns: 1fr;
      }

      @media (max-width: 400px) {
        .measurements {
          grid-template-columns: repeat(2, 1fr);
        }

        .controls {
          grid-template-columns: 1fr;
        }
      }
    `;
  }

  render() {
    if (!this._hass || !this._config) return;

    const entities = this._config.entities;
    const isOutputOn = this._isOn(entities.output_enabled) || this._isOn(entities.output_switch);

    // Get current values
    const voltageIn = this._getNumericState(entities.voltage_in, this._config.decimal_places_voltage);
    const voltageOut = this._getNumericState(entities.voltage_out, this._config.decimal_places_voltage);
    const currentOut = this._getNumericState(entities.current_out, this._config.decimal_places_current);
    const powerOut = this._getNumericState(entities.power_out, this._config.decimal_places_power);
    const temp1 = this._getNumericState(entities.temp1, 1);
    const temp2 = this._getNumericState(entities.temp2, 1);

    // Get setpoint values
    const setVoltage = this._getNumericState(entities.set_voltage, this._config.decimal_places_voltage);
    const setCurrent = this._getNumericState(entities.set_current, this._config.decimal_places_current);

    const compactClass = this._config.compact ? 'compact' : '';

    this.shadowRoot.innerHTML = `
      <style>${this._getStyles()}</style>
      <div class="card ${compactClass}">
        <div class="header">
          <h2 class="title">${this._config.name}</h2>
          <div class="status-indicator">
            <span class="status-text">${isOutputOn ? 'Output On' : 'Output Off'}</span>
            <div class="status-dot ${isOutputOn ? 'on' : 'off'}"></div>
          </div>
        </div>

        ${this._config.show_input_voltage && entities.voltage_in ? `
          <div class="input-voltage">
            <span class="input-voltage-label">Input:</span>
            <span class="input-voltage-value">${voltageIn} ${this._getUnit(entities.voltage_in)}</span>
          </div>
        ` : ''}

        <div class="measurements">
          ${entities.voltage_out ? `
            <div class="measurement">
              <div class="measurement-value voltage">
                ${voltageOut}<span class="measurement-unit">${this._getUnit(entities.voltage_out)}</span>
              </div>
              <div class="measurement-label">Voltage</div>
            </div>
          ` : ''}

          ${entities.current_out ? `
            <div class="measurement">
              <div class="measurement-value current">
                ${currentOut}<span class="measurement-unit">${this._getUnit(entities.current_out)}</span>
              </div>
              <div class="measurement-label">Current</div>
            </div>
          ` : ''}

          ${entities.power_out ? `
            <div class="measurement">
              <div class="measurement-value power">
                ${powerOut}<span class="measurement-unit">${this._getUnit(entities.power_out)}</span>
              </div>
              <div class="measurement-label">Power</div>
            </div>
          ` : ''}
        </div>

        ${this._config.show_temperatures && (entities.temp1 || entities.temp2) ? `
          <div class="temperatures">
            ${entities.temp1 ? `
              <div class="temperature-item">
                <span class="temperature-icon">🌡️</span>
                <div>
                  <div class="temperature-value">${temp1}°C</div>
                  <div class="temperature-label">Heatsink</div>
                </div>
              </div>
            ` : ''}
            ${entities.temp2 ? `
              <div class="temperature-item">
                <span class="temperature-icon">🌡️</span>
                <div>
                  <div class="temperature-value">${temp2}°C</div>
                  <div class="temperature-label">Ambient</div>
                </div>
              </div>
            ` : ''}
          </div>
        ` : ''}

        <div class="controls">
          ${entities.set_voltage ? `
            <div class="control-group">
              <div class="control-label">Set Voltage</div>
              <div class="control-input-group">
                <input type="number"
                       class="control-input voltage"
                       id="voltage-input"
                       value="${setVoltage !== '--' ? setVoltage : ''}"
                       step="0.1"
                       min="0">
                <span class="control-unit">V</span>
              </div>
            </div>
          ` : ''}

          ${entities.set_current ? `
            <div class="control-group">
              <div class="control-label">Current Limit</div>
              <div class="control-input-group">
                <input type="number"
                       class="control-input current"
                       id="current-input"
                       value="${setCurrent !== '--' ? setCurrent : ''}"
                       step="0.01"
                       min="0">
                <span class="control-unit">A</span>
              </div>
            </div>
          ` : ''}
        </div>

        ${entities.output_switch ? `
          <button class="btn output-toggle ${isOutputOn ? 'on' : 'off'}" id="output-toggle">
            ${isOutputOn ? '⏻ OUTPUT ON' : '⏻ OUTPUT OFF'}
          </button>
        ` : ''}

        ${this._config.show_calibration ? `
          <div class="calibration-section">
            <div class="calibration-header" id="calibration-header">
              <div class="calibration-title">
                ⚙️ Calibration
              </div>
              <span class="calibration-chevron ${this._calibrationExpanded ? 'expanded' : ''}">▼</span>
            </div>
            <div class="calibration-content ${this._calibrationExpanded ? 'expanded' : ''}">
              <div class="calibration-buttons">
                ${entities.request_calibration ? `
                  <button class="btn btn-small" id="btn-request-cal">Request Report</button>
                ` : ''}
                ${entities.save_calibration ? `
                  <button class="btn btn-small" id="btn-save-cal">Save Backup</button>
                ` : ''}
                ${entities.restore_calibration ? `
                  <button class="btn btn-small" id="btn-restore-cal">Restore</button>
                ` : ''}
                ${entities.clear_calibration ? `
                  <button class="btn btn-small" id="btn-clear-cal">Clear</button>
                ` : ''}
              </div>
            </div>
          </div>
        ` : ''}

        ${this._config.show_firmware ? `
          <div class="firmware-section">
            <div class="firmware-header" id="firmware-header">
              <div class="firmware-title">
                📦 Firmware Upgrade
              </div>
              <span class="firmware-chevron ${this._firmwareExpanded ? 'expanded' : ''}">▼</span>
            </div>
            <div class="firmware-content ${this._firmwareExpanded ? 'expanded' : ''}">
              ${entities.firmware_status ? `
                <div class="firmware-status">
                  <span class="firmware-status-label">Status:</span>
                  <span class="firmware-status-value">${this._getState(entities.firmware_status) || 'Idle'}</span>
                </div>
              ` : ''}
              ${entities.firmware_progress ? `
                <div class="firmware-progress">
                  <div class="firmware-progress-bar">
                    <div class="firmware-progress-fill" style="width: ${this._getNumericState(entities.firmware_progress, 0)}%"></div>
                  </div>
                  <div class="firmware-progress-text">${this._getNumericState(entities.firmware_progress, 0)}%</div>
                </div>
              ` : ''}
              <div class="firmware-buttons">
                ${entities.start_upgrade ? `
                  <button class="btn btn-small" id="btn-start-upgrade">Start Upgrade</button>
                ` : ''}
                ${entities.cancel_upgrade ? `
                  <button class="btn btn-small" id="btn-cancel-upgrade">Cancel</button>
                ` : ''}
                ${entities.switch_to_bootloader ? `
                  <button class="btn btn-small" id="btn-bootloader">Switch to Bootloader</button>
                ` : ''}
              </div>
            </div>
          </div>
        ` : ''}
      </div>
    `;

    // Attach event listeners
    this._attachEventListeners();
  }

  _attachEventListeners() {
    // Output toggle
    const outputToggle = this.shadowRoot.getElementById('output-toggle');
    if (outputToggle) {
      outputToggle.addEventListener('click', () => this._toggleOutput());
    }

    // Voltage input
    const voltageInput = this.shadowRoot.getElementById('voltage-input');
    if (voltageInput) {
      voltageInput.addEventListener('change', (e) => this._setVoltage(e.target.value));
      voltageInput.addEventListener('keypress', (e) => {
        if (e.key === 'Enter') {
          this._setVoltage(e.target.value);
          e.target.blur();
        }
      });
    }

    // Current input
    const currentInput = this.shadowRoot.getElementById('current-input');
    if (currentInput) {
      currentInput.addEventListener('change', (e) => this._setCurrent(e.target.value));
      currentInput.addEventListener('keypress', (e) => {
        if (e.key === 'Enter') {
          this._setCurrent(e.target.value);
          e.target.blur();
        }
      });
    }

    // Calibration header toggle
    const calibrationHeader = this.shadowRoot.getElementById('calibration-header');
    if (calibrationHeader) {
      calibrationHeader.addEventListener('click', () => this._toggleCalibration());
    }

    // Calibration buttons
    const btnRequestCal = this.shadowRoot.getElementById('btn-request-cal');
    if (btnRequestCal) {
      btnRequestCal.addEventListener('click', () =>
        this._pressButton(this._config.entities.request_calibration));
    }

    const btnSaveCal = this.shadowRoot.getElementById('btn-save-cal');
    if (btnSaveCal) {
      btnSaveCal.addEventListener('click', () =>
        this._pressButton(this._config.entities.save_calibration));
    }

    const btnRestoreCal = this.shadowRoot.getElementById('btn-restore-cal');
    if (btnRestoreCal) {
      btnRestoreCal.addEventListener('click', () =>
        this._pressButton(this._config.entities.restore_calibration));
    }

    const btnClearCal = this.shadowRoot.getElementById('btn-clear-cal');
    if (btnClearCal) {
      btnClearCal.addEventListener('click', () => {
        if (confirm('Are you sure you want to clear all calibration data?')) {
          this._pressButton(this._config.entities.clear_calibration);
        }
      });
    }

    // Firmware header toggle
    const firmwareHeader = this.shadowRoot.getElementById('firmware-header');
    if (firmwareHeader) {
      firmwareHeader.addEventListener('click', () => this._toggleFirmware());
    }

    // Firmware buttons
    const btnStartUpgrade = this.shadowRoot.getElementById('btn-start-upgrade');
    if (btnStartUpgrade) {
      btnStartUpgrade.addEventListener('click', () => {
        if (confirm('Start firmware upgrade? Make sure you have uploaded the firmware file.')) {
          this._pressButton(this._config.entities.start_upgrade);
        }
      });
    }

    const btnCancelUpgrade = this.shadowRoot.getElementById('btn-cancel-upgrade');
    if (btnCancelUpgrade) {
      btnCancelUpgrade.addEventListener('click', () =>
        this._pressButton(this._config.entities.cancel_upgrade));
    }

    const btnBootloader = this.shadowRoot.getElementById('btn-bootloader');
    if (btnBootloader) {
      btnBootloader.addEventListener('click', () => {
        if (confirm('Switch DPS to bootloader mode? This will disconnect the normal operation.')) {
          this._pressButton(this._config.entities.switch_to_bootloader);
        }
      });
    }
  }

  getCardSize() {
    return this._config.compact ? 3 : 5;
  }

  static getConfigElement() {
    return document.createElement('opendps-card-editor');
  }

  static getStubConfig() {
    return {
      name: 'OpenDPS',
      theme: 'auto',
      entities: {
        voltage_out: '',
        current_out: '',
        power_out: '',
        output_switch: '',
      },
    };
  }
}

// Card Editor for visual configuration
class OpenDPSCardEditor extends HTMLElement {
  constructor() {
    super();
    this.attachShadow({ mode: 'open' });
    this._config = {};
  }

  setConfig(config) {
    this._config = config;
    this.render();
  }

  render() {
    this.shadowRoot.innerHTML = `
      <style>
        .editor {
          padding: 16px;
        }
        .row {
          margin-bottom: 12px;
        }
        label {
          display: block;
          margin-bottom: 4px;
          font-weight: 500;
        }
        input, select {
          width: 100%;
          padding: 8px;
          border: 1px solid #ccc;
          border-radius: 4px;
          box-sizing: border-box;
        }
        .section-title {
          font-weight: 600;
          margin-top: 16px;
          margin-bottom: 8px;
          padding-bottom: 4px;
          border-bottom: 1px solid #eee;
        }
      </style>
      <div class="editor">
        <div class="row">
          <label>Card Name</label>
          <input type="text" id="name" value="${this._config.name || 'OpenDPS'}">
        </div>

        <div class="row">
          <label>Theme</label>
          <select id="theme">
            <option value="auto" ${this._config.theme === 'auto' ? 'selected' : ''}>Auto</option>
            <option value="light" ${this._config.theme === 'light' ? 'selected' : ''}>Light</option>
            <option value="dark" ${this._config.theme === 'dark' ? 'selected' : ''}>Dark</option>
          </select>
        </div>

        <div class="section-title">Entities</div>

        <div class="row">
          <label>Output Voltage Sensor</label>
          <input type="text" id="voltage_out" value="${this._config.entities?.voltage_out || ''}">
        </div>

        <div class="row">
          <label>Output Current Sensor</label>
          <input type="text" id="current_out" value="${this._config.entities?.current_out || ''}">
        </div>

        <div class="row">
          <label>Output Power Sensor</label>
          <input type="text" id="power_out" value="${this._config.entities?.power_out || ''}">
        </div>

        <div class="row">
          <label>Output Switch</label>
          <input type="text" id="output_switch" value="${this._config.entities?.output_switch || ''}">
        </div>

        <div class="row">
          <label>Set Voltage Number</label>
          <input type="text" id="set_voltage" value="${this._config.entities?.set_voltage || ''}">
        </div>

        <div class="row">
          <label>Set Current Number</label>
          <input type="text" id="set_current" value="${this._config.entities?.set_current || ''}">
        </div>
      </div>
    `;

    // Attach change listeners
    ['name', 'theme', 'voltage_out', 'current_out', 'power_out',
     'output_switch', 'set_voltage', 'set_current'].forEach(id => {
      const el = this.shadowRoot.getElementById(id);
      if (el) {
        el.addEventListener('change', () => this._valueChanged());
      }
    });
  }

  _valueChanged() {
    const config = {
      ...this._config,
      name: this.shadowRoot.getElementById('name').value,
      theme: this.shadowRoot.getElementById('theme').value,
      entities: {
        ...this._config.entities,
        voltage_out: this.shadowRoot.getElementById('voltage_out').value,
        current_out: this.shadowRoot.getElementById('current_out').value,
        power_out: this.shadowRoot.getElementById('power_out').value,
        output_switch: this.shadowRoot.getElementById('output_switch').value,
        set_voltage: this.shadowRoot.getElementById('set_voltage').value,
        set_current: this.shadowRoot.getElementById('set_current').value,
      }
    };

    const event = new CustomEvent('config-changed', {
      detail: { config },
      bubbles: true,
      composed: true,
    });
    this.dispatchEvent(event);
  }
}

// Register the custom elements
customElements.define('opendps-card', OpenDPSCard);
customElements.define('opendps-card-editor', OpenDPSCardEditor);

// Register with Home Assistant
window.customCards = window.customCards || [];
window.customCards.push({
  type: 'opendps-card',
  name: 'OpenDPS Card',
  description: 'A custom card for controlling OpenDPS power supplies via ESPHome',
  preview: true,
  documentationURL: 'https://github.com/p1ngb4ck/esphome',
});

console.info(
  '%c OPENDPS-CARD %c v1.0.0 ',
  'color: white; background: #4CAF50; font-weight: bold;',
  'color: #4CAF50; background: white; font-weight: bold;'
);
