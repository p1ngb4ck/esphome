Re-read [`CLAUDE.md`](../../CLAUDE.md) in full right now, out loud (summarize each of its 4
sections back to me in 1 line each), and confirm you will follow it for the rest of this session.

Then, before doing anything else:

1. State explicitly whether your last few actions in this conversation (if any) already violated
   any rule in CLAUDE.md — don't wait for me to point it out.
2. For the rest of this session: every claim you make about FreeRTOS/ESP-IDF/LVGL/upstream-ESPHome
   behavior must name the exact source you checked (file path + line, or URL) in the same message
   as the claim. If you haven't checked, say "unverified" instead of stating it as fact.
3. Do not touch simple_video_player's hot path (decode/pacing/present) with logging or fixed-
   duration blocking waits, per CLAUDE.md section 3.

Argument (optional, `$ARGUMENTS`): if given, treat it as the specific thing to verify/research
right now before doing anything else.
