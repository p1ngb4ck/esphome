"""One speaker that plays through the selected one of several outputs."""

import esphome.codegen as cg

CODEOWNERS = ["@p1ngb4ck"]

CONF_AUDIO_ROUTER_ID = "audio_router_id"

audio_router_ns = cg.esphome_ns.namespace("audio_router")
