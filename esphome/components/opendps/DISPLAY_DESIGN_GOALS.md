# OpenDPS Display Design Goals

## Hard Rules

- **USE EXISTING IDs ONLY** — no inventing new sensor IDs, label IDs, or entity IDs
- **ALL sizes and positions are FIXED** — no `size_content`, no `flex_grow` on structural containers
- **BOTH horizontal AND vertical sizes must be explicitly set on every widget**
- **Do NOT add features, sensors, or widgets that were not asked for**
- **Do NOT make any changes without explicit user permission**

## Screen

- Physical: 1024×600 (native landscape, no rotation)  → logical LVGL space: **1024×600**
- Content area between header and footer: y=40 to y=560 → **520px tall**

## Top Layer (z-layer above pages)

- `top_layer` has **no layout** — children are absolutely positioned
- **Header**: x=0, y=0, w=1024, h=40
  - Left: time (`lbl_status_time`) with small left padding, format from `g_datetime_fmt`
  - Right (small right padding, left-to-right order): `lbl_vin` → `lbl_t1` → `lbl_t2` (if `g_show_temps`) → WiFi status placeholder
  - `lbl_vin`, `lbl_t1`, `lbl_t2` exist ONLY in the header — not in any page
- **Footer / tab bar**: x=0, y=560, w=1024, h=40
  - 3 tab buttons: MAIN (w=341), GRAPHS (w=341), SETTINGS (w=342)
  - IDs: `tab_btn_main`, `tab_btn_graphs`, `tab_btn_settings`, `tab_lbl_main`, `tab_lbl_graphs`, `tab_lbl_settings`
  - On click: set `g_active_page`, show page, call `apply_theme`

## Pages (content y=40..560 only, no footer bars inside pages)

### page_main
Existing widget IDs to use:
- `lcd_panel` — voltage/current display panel
- `lbl_vout`, `lbl_iout`, `lbl_pout`, `lbl_setv`, `lbl_seti`
- `led_cv`, `led_cc`, `led_cp`, `led_out`, `btn_cv`, `btn_cc`, `btn_cp`
- `sl_v`, `sl_i` — voltage and current sliders
- `pwr_ring`, `lbl_pwr`, `lbl_pwr_icon` — output on/off toggle
- **NO temperature bars or labels in page_main** (`bar_t1`, `bar_t2`, `lbl_t1`, `lbl_t2` are header-only)
- **NO `lbl_vin` in page_main** — header-only

### page_graphs
Existing widget IDs to use:
- `chart_v`, `ser_v` — voltage chart
- `chart_i`, `ser_i` — current chart
- `chart_p`, `ser_p` — power chart
- `lbl_vout_g`, `lbl_iout_g`, `lbl_pout_g` — graph value labels

### page_settings
Existing widget IDs to use:
- `opendps_select_mode` — CV/CC/CP mode select
- `voltage_number`, `current_number` — preset number setpoints
- `sl_bright`, `lbl_bright` — brightness slider
- `sw_lock` — lock switch
- `datetime_format_select`, `theme_select`
- `opendps_baud_select`, `opendps_boot_baud_select`
- `show_temperature_switch` (`sb_temps` is its visibility group)
- `dps_output_switch`, `dps_lock_switch`
- `cal_input`, `opendps_fw_path_input`
- calibration inputs: `input_v_adc_k/c`, `input_v_dac_k/c`, `input_a_adc_k/c`, `input_a_dac_k/c`, `input_vin_adc_k/c`

## Temp Sensors — ONLY 2 EXIST on the OpenDPS device
- `opendps_temp1` → sensor ID, updates `lbl_t1`, `bar_t1`
- `opendps_temp2` → sensor ID, updates `lbl_t2`, `bar_t2`
- Display controlled by `g_show_temps` global and `sb_temps` visibility group
- **No other temperature sensors exist. Do not add any.**

## Globals that drive UI
- `g_vset`, `g_iset` — set voltage/current
- `g_theme` — color theme name
- `g_datetime_fmt` — time format (0=HH:MM, 1=HH:MM:SS, 2=DD.MM.YYYY HH:MM, 3=MM/DD/YYYY HH:MM, 4=YYYY-MM-DD HH:MM)
- `g_show_temps` — bool, show/hide temp display
- `g_accent`, `g_accent_dark`, `g_accent_dim` — resolved colors from theme
- `g_active_page` — 0=main, 1=graphs, 2=settings

## Design Approach
1. Get layout, positions, and sizes right first
2. Minimal vertical spacing
3. Color theme applied via `apply_theme` script
4. Settings page: start minimal, expand later
