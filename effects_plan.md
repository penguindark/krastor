# Plan: modular GLSL background effects with full event feed

Goal: the shader background reacts not only to voice but to ALL assistant
events (LLM thinking, key presses, speaking, listening), the user can pick
any shader or turn the effect off entirely - and the current "totally dark"
state is explained and fixed.

**STATUS: DONE (2026-11-07)** - all phases implemented and screenshot-verified.

## Analysis (2026-11-07)

The architecture is ALREADY modular (`src/effects.h/.cpp`): plugins are plain
`data/shaders/*.frag` files compiled at runtime on a shared fullscreen-triangle
vertex shader, hot-reloaded on file change, listed in Settings (combo +
"Effect brightness" knob), a broken plugin never replaces the running one.

Why the screen looks totally dark - nothing is broken:
1. `bin/config.json` has `gui.effect: ""` -> the built-in "gradient" effect,
   which is intentionally near-black (max channel ~0.19 blue, x0.75
   brightness -> RGB <= ~36).
2. The only plugin, `ring_spectrogram.frag`, is audio-only: with the mic
   closed (u_level=0, empty spectrum ring) it draws a very dim base + faint
   strands, so it also reads as black most of the time.
3. The uniform contract only carries AUDIO events:
   `u_time, u_resolution, u_level, u_spectrum, u_spec_head, u_listening,
   u_speaking, u_intensity`. LLM-thinking and key-press states never reach
   the shader, and there is no "off" choice ("" = gradient, not disabled).

All the event sources already exist in `AppState` (gui.cpp):
`worker == Worker::Thinking` (LLM request in flight, includes tool loop and
compaction), `worker == Worker::Listening || stt.streaming()`,
`speaking` (atomic), and the input buffer (typing = text changed this frame).

## Phase 1 - extend the uniform contract (event -> shader)

No backward-compat constraint (user decision): the contract is redesigned
cleanly and `ring_spectrogram.frag` is updated to use the new events.
New uniforms in `kHeader` (effects.cpp):
- `u_thinking`  1 while an LLM request is in flight (worker Thinking/Compacting)
- `u_typing`    decaying pulse: set to 1.0 on every input-buffer change
  (the `nk_edit_string` return value already flags "edited this frame"),
  decays ~3/s in the frame loop - key-press feedback
(`u_streaming` deliberately NOT added: llm_client has no token streaming, so
a streaming flag would be a lie; the struct below makes it cheap to add
when real streaming lands.)

Future-proof plumbing: `EffectEngine::draw(int w, int h, float t,
const EffectInput&)` where `EffectInput` (effects.h) bundles
level/hist/head/listening/speaking/thinking/typing/intensity - adding a
future event never changes the draw() signature again.
gui.cpp: `typing_pulse` member set on edit, decayed per frame;
thinking = `worker == Thinking || Compacting`.

## Phase 2 - "Off" choice

- `EffectEngine::select("off", ...)`: valid name -> engine marks itself
  disabled; `draw()` early-returns; the frame shows the flat clear color.
- Settings combo gains "off" (first entry, display "Off (flat background)");
  persisted as `gui.effect: "off"`. `""` keeps meaning built-in gradient
  (backward compatible).
- config.example.json documents the three kinds: "off" | "" (gradient) |
  plugin display name.

## Phase 3 - new sample shaders that showcase the event feed - DONE

- `data/shaders/thinking_pulse.frag` - ambient drifting waves; slow breath
  while idle, faster + violet while `u_thinking`, expanding ring on
  `u_typing`, warm glow on `u_speaking`, cool tint on `u_listening`.
- `data/shaders/plasma_drift.frag` - plasma field whose second harmonic
  fades in only while thinking; typing ripple, speaking lift, listening
  scan, mic level widens the bands.
- `ring_spectrogram.frag` updated with thinking shimmer + typing ripple.

## Phase 4 - verification + docs - DONE

- Screenshot harness: each effect launched with `gui.effect` set to
  gradient / ring / thinking_pulse / plasma_drift / off and confirmed
  distinct renders; "off" = flat background.
- `--tool-test`/`--session-test`/`--media-test` PASS on both toolchains
  (no GL self-test: headless has no context, and `select()` already
  reports compile failures into the Settings status line).
- Docs synced: development_log.md, src/development.md (effects row),
  project.md (shader paragraph). `config.example.json` needs no change:
  `gui.effect` accepts "off" | "" (gradient) | any plugin display name.

## Out of scope

- Per-effect custom parameters (each shader keeps only the shared contract).
- Audio-spectrum changes (SpectrumAnalyzer untouched).
- Post-processing over the chat UI (shader stays a background pass under the
  Nuklear render).

## Effort

Phase 1 ~1h (contract + plumbing), Phase 2 ~30m, Phase 3 ~1h (shader tuning
by screenshot), Phase 4 ~1h.
