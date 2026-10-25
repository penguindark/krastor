# spectogram_plan.md — Font scaling, pluggable GLSL effects, hands-free cycle

Detailed implementation plan for three Phase 5 features of Krastor.
Confirmed defaults: font scaling affects the **entire UI**; the spectrogram uses the
**live mic only when hands-free mode is ON** (synthetic animation otherwise); while
Krastor is thinking/speaking the **mic input is dropped** (no barge-in yet).

Reference: `project.md` §4.3 (GUI), §4.1 (I/O panel), §8 (roadmap). This file will be
moved to the repo root and referenced from `project.md` when implementation starts.

---

## Feature A — Font size with Ctrl +/−

### Design
Nuklear bakes glyphs into a texture atlas at fixed pixel heights, so runtime scaling
means: pre-bake a ladder of sizes at startup and swap the style font.

1. **Font ladder** (`src/gui.cpp`, in `run_gui` before `nk_glfw3_font_stash_end`):
   - Load `data/fonts/jetbrains-mono.ttf` into memory once (existing code).
   - Bake sizes **12, 14, 16, 18, 20, 22, 24, 26, 28, 30** via
     `nk_font_atlas_add_from_memory` (all land in the same atlas); keep the
     `nk_font*` handles in `std::vector<nk_font*> g_fonts` (index = size step).
   - Default index = 18 px (or `cfg.gui.font_size` from config).
   - Fallback: if the TTF is missing, bake the same ladder from the built-in font.
2. **Key handling** (render loop, before `nk_glfw3_new_frame`):
   - Edge-detect with `glfwGetKey(win, ...)` + previous-frame state:
     - `GLFW_KEY_EQUAL` or `GLFW_KEY_KP_ADD` + `GLFW_MOD_CONTROL` → size++
     - `GLFW_KEY_MINUS` or `GLFW_KEY_KP_SUBTRACT` + `GLFW_MOD_CONTROL` → size−−
     - `GLFW_KEY_0` + `GLFW_MOD_CONTROL` → reset to 18
   - Clamp to [12, 30]; on change call `nk_style_set_font(&device.ctx, &g_fonts[i]->handle)`
     and update `cfg.gui.font_size` (saved with the rest of settings).
3. **Layout scaling**: replace the hard-coded row heights with factors of the font
   height `fh = ctx->style.font->height`:
   - top bar `2.4·fh`, input row `3.6·fh`, status `1.2·fh`, attachment chip row `1.4·fh`,
     chat group height = window height − sum of the above (existing formula, now in `fh`).
   - The chat renderer and wrap math already measure from `ctx->style.font` — no change.
4. **Config**: `gui.font_size` (int, default 18) — see §Config.

### Files
`src/gui.cpp`, `src/config.h/.cpp`, `config.example.json`, docs.

### Acceptance
Ctrl+= grows, Ctrl+− shrinks, Ctrl+0 resets; chat wrap, input field, buttons and row
heights all scale coherently; size survives restart; both toolchains build.

---

## Feature B — Pluggable GLSL background effects + circular mic spectrogram

### B1. Plugin system
- **Folder**: `data/shaders/` — one `.frag` file per effect, plain GLSL 330 core.
  Optional first-line comment declares the display name:
  `// krastor-effect: Ring Spectrogram`
  (fallback display name = file stem).
- **Loader** (`src/effects.h/.cpp`, new):
  - `std::vector<EffectInfo> scan(const std::string& dir)` at startup + on demand
    (Settings has a Refresh button).
  - `class EffectEngine` owns: shared fullscreen vertex shader (no VBO, `gl_VertexID`
    big triangle — existing pattern), the compiled `.frag`, compile-error reporting
    (message kept in a string, shown in the status line and printed to stderr;
    previous effect stays active on failure).
  - **Hot-reload**: every 500 ms check `last_write_time` of the active file; recompile
    on change (tweak shaders live while the app runs).
  - Built-in fallback effect = the current animated gradient, kept as an embedded
    string and always selectable as "Built-in: gradient".
- **Uniform contract** prepended to every plugin source (plugins use what they want):
  ```glsl
  uniform vec2      u_resolution; // framebuffer px
  uniform float     u_time;       // seconds since start
  uniform float     u_level;      // smoothed mic RMS 0..1 (0 when mic off)
  uniform sampler2D u_spectrum;   // R8 texture: x = time history, y = 64 log bands
  uniform float     u_spec_head;  // 0..1: newest column position in u_spectrum.x
  uniform float     u_listening;  // 1 while hands-free stream is active
  uniform float     u_speaking;   // 1 while TTS playback is running
  ```
  Plugins write `out_color`.
- **Selection**: Settings modal gets an "Effect" `nk_combo` (list from the loader);
  choosing applies immediately and persists `gui.effect`.

### B2. Audio analysis feed
- **Ring buffer** in `Stt` (`src/stt.h/.cpp`): 16384 floats (1 s @ 16 kHz),
  single producer (miniaudio capture callback) / single consumer (render thread),
  `std::atomic<size_t>` write cursor; `Stt::latest(float* dst, size_t n)` copies the
  last `n` samples. The mic callback also keeps updating the existing peak meter.
- **FFT** (`src/fft.h`, header-only, ~60 lines): iterative radix-2, N = 512,
  Hann window. No new dependency (dual-toolchain safe).
- **Bands** (`src/audio_analysis.h/.cpp` or inside gui frame code):
  - every frame: `latest(512)` → FFT → magnitude of 256 bins → **64 log-spaced bands**
    (≈ 60 Hz…7.8 kHz) → dB scale → attack/release smoothing (fast attack ~0.5,
    slow release ~0.08) → clamp 0..1.
  - upload into the **spectrum history texture**: 128 (time) × 64 (bands), `GL_R8`,
    ring-written at a moving column (uniform `u_spec_head`), `GL_LINEAR` filtering.
  - `u_level` = smoothed RMS of the same window.
- **No mic → synthetic feed**: when the stream is not running, the same pipeline is
  fed with band-limited noise (sum of slow sines + hash noise) so effects still
  animate; `u_listening = 0` lets plugins dim/desaturate the synthetic state.

### B3. First plugin — `data/shaders/ring_spectrogram.frag`
Reproduces the reference image (cyan/magenta mesh ribbons on a dark ring):
- polar coords: `r = length(p)`, `θ = atan(p.y, p.x)` with `p` centered, aspect-corrected.
- band lookup: `b = texture(u_spectrum, vec2(fract(θ/2π − flow(t)), y))` — history
  flows around the ring; two ribbon layers read different band ranges (low-mid →
  cyan, mid-high → magenta) and different flow speeds/phases.
- radius displacement `R(θ) = R0 + amp·b`; intensity = soft band
  `exp(−k·(|p| − R(θ))²)`; angular ripple (many thin strands) gives the mesh look;
  additive blend of the two ribbons + faint outer glow; dark navy base gradient.
- reacts to `u_level` (global pulse) and `u_speaking` (brighten while TTS plays).

### Files
`src/effects.h/.cpp` (new), `src/fft.h` (new), `src/stt.h/.cpp` (ring buffer),
`src/gui.cpp` (engine wiring, texture, Settings combo), `data/shaders/*.frag`,
`data/shaders/development.md`, config, docs.

### Acceptance
Effect dropdown lists `data/shaders/*.frag` + built-in; selecting switches instantly;
editing a `.frag` while running hot-reloads; with hands-free ON, speaking moves the
ring bands live (user validates audio-reactivity); with mic OFF the ring still
animates synthetically; a broken shader does not crash and reports the GLSL error.

---

## Feature C — Continuous hands-free cycle (mic → STT → LLM → text → TTS)

### C1. `Stt` stream mode (`src/stt.h/.cpp`)
- `bool start_stream(int device_index, std::function<void(std::vector<float>)> on_utterance)`
  — opens the mic persistently; the capture callback feeds the ring buffer (B2) **and**
  `SherpaOnnxAcceptWaveformOffline` into the VAD; closed VAD segments are popped and
  delivered via `on_utterance(samples)` on the **audio callback thread**.
- `void stop_stream()`; `bool streaming() const`.
- `listen()` (CLI + one-shot GUI button) is refactored to share the same core and is
  disabled while a stream is active (button shows "Hands-free active").
- Threading rule: VAD touched only by the audio callback; recognizer used only by the
  pipeline worker (never concurrently).

### C2. Pipeline worker (`src/gui.cpp`)
- `AppState` gains:
  - `std::deque<std::vector<float>> utt_q` + `std::mutex` + `std::condition_variable`;
  - worker `std::thread` loop: wait → pop utterance →
    `Heard` event("…") → `llm.complete(conv)` → `Reply` event;
  - `std::mutex evt_mtx; std::vector<PipelineEvent> evt_q;` polled every frame by the
    render loop (existing non-blocking pattern — the loop never blocks).
- **Echo guard**: the utterance callback drops segments while
  `worker != None || speaking` when `agent.ignore_while_speaking` (default true).
  Rendered as: while Krastor thinks/speaks, mic segments are discarded (status shows
  "ignored (speaking)"). True barge-in is out of scope.
- On `Reply`: append chat line; if `auto_speak` → `speak_async` (existing TTS path).
- On `Heard`: append the transcript as the user line and `conv.add_user(text)`
  (done inside the worker under the same conversation-snapshot discipline as today).

### C3. GUI + config
- Top bar: **"Hands-free: ON/OFF"** toggle next to Speak toggle. Starting it calls
  `stt.start_stream(cfg.stt.input_device, cb)` and starts the worker; stopping tears
  both down. Mic privacy light is on only in this mode (per confirmed default).
- Status line states: `hands-free: idle` / `heard: "<text>"` / `thinking…` / `speaking…`.
- Config additions (§ below): `agent.continuous` (auto-start stream at launch),
  `agent.ignore_while_speaking`.

### Files
`src/stt.h/.cpp`, `src/gui.cpp`, `src/config.h/.cpp`, `config.example.json`, docs.

### Acceptance
Toggle ON → speak a phrase → transcript appears → LLM reply appears → spoken when
Speak ON, without pressing anything; no self-trigger from TTS output; toggle OFF
returns to manual mode; CLI `/listen` unchanged.

---

---

## Feature D — Config persistence next to the executable, save-on-validated-change

### Design
The runtime configuration becomes a self-contained file **next to the executable**
(`bin/config.json`), rewritten automatically every time a setting change has been
**validated** — "validated" meaning the change was proven to work, not merely entered.

1. **Location** (`src/config.h/.cpp`):
   - New helper `std::string exe_dir()`: Win32 `GetModuleFileNameA(nullptr, ...)`
     (works on MinGW + MSVC) → parent path; Linux `readlink("/proc/self/exe")`.
   - Load order at startup: `exe_dir()/config.json` first; if absent, fall back to
     the current CWD `config.json` (backward compatibility with the existing local
     config) and migrate it by writing the validated copy next to the exe.
   - `Config::save()` always writes `exe_dir()/config.json` **atomically**: serialize
     to `config.json.tmp`, `fs::rename` over the target (never leaves a half-written
     file). `bin/` is git-ignored, so secrets stay out of the repo (env-var API key
     fallback is kept).
2. **Validation rules per setting** (what "it works" means):
   | Setting | Validated by | Saved when |
   |---|---|---|
   | `llm.base_url / model / api_key` | async **probe request** to `{base_url}/chat/completions` (1-token "ping"); HTTP 2xx = valid | probe succeeds; on failure the modal shows the error and the old values stay persisted (in-memory edits are still discarded on cancel) |
   | `llm.temperature / max_tokens / system_prompt` | range checks (0 ≤ temp ≤ 2, max_tokens > 0) | with the LLM probe on Save (they ride the same request) |
   | `tts.voice` | Piper model **loads successfully** (`tts.ensure_loaded()` for the new voice returns ok) | after the load succeeds |
   | `stt.input_device` | device **opens** (`start_stream`/open test succeeds) | after the open succeeds |
   | `gui.font_size` | applied without error (trivially true) | debounced ~500 ms after the last Ctrl +/− |
   | `gui.effect` | shader **compiles** (link + compile status OK) | only on successful compile (a broken .frag is never persisted) |
   | `agent.continuous / ignore_while_speaking`, `auto_speak` | toggle applied (stream started/stopped ok) | immediately |
3. **Auto-save wiring** (`src/gui.cpp`): every validated change calls a single
   `save_config(cfg)` entry point (mutex-guarded, debounced for rapid changes like
   font zoom). The Settings modal keeps its Save button, but Save now means
   "apply + validate"; persistence happens per validated change, not per session.
4. **Failure UX**: validation failures never write the file; the status line / modal
   shows why (e.g. `LLM probe failed: connection refused`, `voice load failed`,
   `shader compile error`). The app keeps running with the last known-good config.

### Files
`src/config.h/.cpp` (exe_dir, atomic save, new sections), `src/gui.cpp` (validation +
auto-save wiring), `src/llm_client.h/.cpp` (lightweight `probe()` call),
`config.example.json`, docs.

### Acceptance
Edit LLM URL to a bad value → error shown, `bin/config.json` unchanged; fix it →
probe OK → file updated; restart → everything comes back from `bin/config.json`
regardless of the working directory the exe is launched from; font/effect/voice
changes survive a restart without any manual save.

---

## Config schema additions (`config.example.json` + `config.h/.cpp`)
```json
"gui":   { "font_size": 18, "effect": "ring_spectrogram" },
"agent": { "continuous": false, "ignore_while_speaking": true }
```
Defaults when keys are absent; `Config::save()` writes them back.

## Implementation order (each step = dual-toolchain build + docs + commit, per AGENT.md)
1. **D** — `exe_dir()` + atomic save to `bin/config.json` + CWD fallback/migration +
   `save_config()` entry point (validation rules wired per feature as they land).
2. **A** — font scaling + `gui.font_size` (first auto-saved setting, debounced).
3. **B1+B2** — ring buffer, FFT, band/texture pipeline, effect engine + plugin loader +
   hot-reload + Settings dropdown, synthetic spectrum, built-in gradient as plugin #0.
4. **B3** — `ring_spectrogram.frag` wired to the real spectrum.
5. **C** — Stt stream mode, pipeline worker, Hands-free toggle, echo guard, config.
6. **D (completion)** — LLM probe in the settings modal, voice/mic/effect validation
   gates (each validation rule lands with the feature it guards).

## Verification
- Screenshots via `KRASTOR_UITEST=1` hook: font min/mid/max; effect selection;
  synthetic-spectrum ring rendering (deterministic noise seed).
- FFT self-check: 440 Hz sine peaks in the expected band (temporary `--selftest` flag).
- Audio behaviors (mic reactivity, hands-free loop, echo guard) validated by the user
  per project convention; assistant validates everything visual/structural.

## Risks / mitigations
- **Config file corruption** → atomic tmp+rename write; last-known-good kept in memory.
- **Two builds share `bin/`** (mingw + msvc presets both emit `bin/krastor.exe`) →
  one shared `bin/config.json` is intentional: both toolchains read/write the same file.
- **Probe request cost/latency** → 1-token probe on a worker thread; modal shows
  "validating…" and Save is non-blocking.
- **Acoustic echo** (mic hears speakers) → drop-while-speaking guard; recommend headphones.
- **VAD/recognizer thread safety** → strict ownership rules above (C1).
- **Two mic users** (stream + one-shot listen) → mutually exclusive, UI enforces.
- **Atlas memory** for 10 font sizes ≈ a few MB — acceptable.
- **Shader compile errors in plugins** → previous effect retained, error surfaced in UI.
