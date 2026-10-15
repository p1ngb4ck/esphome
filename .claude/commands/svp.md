Session entry for simple_video_player (svp) and svp_relay work. Do this before anything else:

1. Read in full: [`CLAUDE.md`](../../CLAUDE.md), then
   [`esphome/components/simple_video_player/.ai/README.md`](../../esphome/components/simple_video_player/.ai/README.md),
   [`architecture.md`](../../esphome/components/simple_video_player/.ai/architecture.md) and
   [`open-issues.md`](../../esphome/components/simple_video_player/.ai/open-issues.md).
   `protocol.md` and `perf-notes.md` in the same folder only when the topic needs them.
2. Reply with, in 5 lines at most: the hardware/environment facts that matter for the topic, the
   hard rules the topic touches, and the open-issues entries that relate to it.
3. Check `git log --oneline -5` and `git status`; say if the tree is not clean.
4. Then work strictly by the change procedure in README.md section 4 (scope first, integrate into the
   existing mechanisms, diff review, independent check, one topic per commit, docs updated with facts
   that changed).

Topic (optional, `$ARGUMENTS`): if given, it is what this session is about; do steps 1-3 for it and
then propose the scope (step 2 of the change procedure) before writing any code.
