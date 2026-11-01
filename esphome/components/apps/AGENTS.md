# apps: guide for AI agents

App slots with interlock: `AppSlot::start()` stops every running app in its interlock list, then fires
`on_start`; `stop()` fires `on_stop`. No state beyond `running_`; apps do their own work in their triggers. Keep it
that small - app logic belongs in the app's own component or YAML.
