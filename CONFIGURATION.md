# Configuration reference

The first steps are in [README.md](README.md).

## `sip_phone`

| Option | Default | Description |
|---|---|---|
| `microphone` | required | A `resampler` microphone (esphome-audio); 16 kHz with `echo_cancellation` is the usual choice. |
| `speaker` | required | The call's sound output. |
| `account` | - | The SIP account, see below. It can also be set at runtime (API action `sip_phone_set_account`). |
| `sip_port` | `5060` | Local SIP port. |
| `rtp_port` | `40000` | Local RTP port. |
| `codecs` | `[opus, g722, pcma, pcmu]` | Offered in this order. |
| `opus` | - | `bitrate` (`64000`, 6000-510000), `complexity` (`2`, 0-10). |
| `ptime` | `20ms` | Packet time: 20, 40 or 60 ms. |
| `playout_delay` | `60ms` | Jitter buffer. |
| `ring_timeout` | `60s` | An unanswered call ends (missed) after this long. |
| `task_stack_in_psram` | `false` | The call task's stack in PSRAM. |
| `contacts` | `[]` | Up to 24: `name` (up to 31 characters), `uri` (up to 95, `sip:user@host` or a number). |

### `account`

| Option | Default | Description |
|---|---|---|
| `server` | required | The registrar / proxy. |
| `port` | `5060` | Its port. |
| `username` | required | Up to 63 characters. |
| `password` | required | Up to 63 characters. |
| `auth_username` | `username` | When the authentication name differs. |
| `domain` | `server` | The SIP domain. |
| `display_name` | `""` | Shown to the called party (up to 47 characters). |
| `register_expiry` | `600s` | Registration lifetime. |

### Triggers

| Trigger | Variables |
|---|---|
| `on_registered` | - |
| `on_registration_failed` | `reason` |
| `on_incoming_call` | `uri`, `name` |
| `on_outgoing_call` | `uri` |
| `on_call_established` | - |
| `on_call_ended` | `reason` |
| `on_dtmf` | `digit` |
| `on_message` | `uri`, `text` |
| `on_state` | `state` |

### Actions

| Action | Options |
|---|---|
| `sip_phone.call` | `destination` (a contact's URI, a number or `sip:` URI; templatable) |
| `sip_phone.answer` / `hangup` / `decline` | - |
| `sip_phone.send_dtmf` | `digits` |
| `sip_phone.send_message` | `destination`, `text` |
| `sip_phone.set_mute` / `set_dnd` / `set_auto_answer` | `value` (true / false) |

```yaml
sip_phone:
  id: phone
  microphone: call_mic
  speaker: main_speaker
  account:
    server: fritz.box
    username: !secret sip_user
    password: !secret sip_password
    display_name: Kitchen
  codecs: [g722, opus, pcma]
  contacts:
    - name: Office
      uri: "**621"
  on_incoming_call:
    - logger.log:
        format: "call from %s"
        args: [name.c_str()]

button:
  - platform: template
    name: Call office
    on_press:
      - sip_phone.call:
          id: phone
          destination: "**621"
```

### API actions (Home Assistant)

With `api:`: `sip_phone_call` (destination), `sip_phone_answer`, `sip_phone_hangup`, `sip_phone_decline`,
`sip_phone_send_dtmf` (digits), `sip_phone_send_message` (destination, text), `sip_phone_mute`, `sip_phone_set_dnd`,
`sip_phone_set_auto_answer`, `sip_phone_set_account`, `sip_phone_set_contacts`, `sip_phone_add_contact`,
`sip_phone_remove_contact`.

## Entities

```yaml
text_sensor:
  - platform: sip_phone
    sip_phone_id: phone
    state:
      name: Phone state
    peer:
      name: Caller
    message:
      name: Last message

event:
  - platform: sip_phone
    sip_phone_id: phone
    name: Phone event
```

The event entity fires `registered`, `unregistered`, `registration_failed`, `incoming`, `outgoing`, `answered`,
`ended`, `missed`, `rejected`, `failed`, `dtmf`, `message`, `message_sent`.
