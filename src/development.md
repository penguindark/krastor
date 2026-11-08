# src/ — C++ sources

Development doc for this folder (see `../project.md` for the master description).

## Purpose
All C++20 source code of the Krastor agent. Built by the root `CMakeLists.txt`;
executables are emitted to `../bin`, intermediates to `../obj`.

## Current state (Phase 5, 2026-10-29) — GUI chat interface
| Module | Responsibility |
|---|---|
| `main.cpp` | CLI REPL: commands (`/help /voices /voice <id> /say <text> /listen [sec] [idx] /devices /mic [sec] [idx] /transcribe <file> /reset /sessions /session /agent [list|use <id>] /compact /title /config /exit`), everything else goes to the LLM; at startup resolves `llm.model_ref` against `llm_models.jsonc` (prints `model: <ref> -> <base_url>`), applies the active agent (`--agent <id>` overrides `agent.active`; `base_llm` keeps the untouched global so `/agent use` can switch back) and applies the `stt.denoise`/`stt.echo_cancel` toggles to `AudioFx` before launching CLI or `--gui`; headless self-test flags `--icon-test`, `--fx-test`, `--tts-filter-test`, `--tool-test`, `--config-test` and `--session-test` exit early with PASS/FAIL, `--tool-run <tool> <json>` runs one tool call without the LLM, session flags `--session <id> --new-session --list-sessions --delete-session <id> --compact [focus]` |
| `config.h/.cpp` | **Two-level JSONC config** (see `config.example.jsonc`): `config.jsonc` is user-owned (comments preserved forever, the app NEVER writes it) and `state.json` next to the exe is app-owned (`save()`, atomic tmp+rename) — precedence defaults < config.jsonc < state.json < env API key (`KRASTOR_API_KEY` -> `OPENAI_API_KEY`). `load()` parses both through one `apply_json(cfg, root, with_agents)` (a broken state.json is ignored, a broken config.jsonc throws); lookup `exe/config.jsonc` > `exe/config.json` > `./config.jsonc` > `./config.json`. Sections: `llm` (incl. `context_size`, `model_ref` — the selected `llm_models.jsonc` entry, `reasoning_effort` low\|medium\|max), `tts` (models_dir, default_voice, speed, `auto_speak` — auto-speak replies, **off by default**), `stt`, `gui` (font_size, effect, effect_intensity, window_w/h/x/y — position -1 = OS default), `agent` (name, `active` — the selected agent id, continuous, ignore_while_speaking), `agents` map (user-only, never saved), `colors` (user-only chat scheme: text/system/meta/thinking/thinking_body/`block_bg` - the chat block panel background - + `md_*` markdown palette, opencode-ish defaults, white text), `chat` (user-only block layout px: bar_w 3, bar_gap 14, pad_x 12, pad_y 10, block_gap 10), `tools`, `sessions`. Helpers: `find_agent`/`next_agent` (circular), `parse_hex_color` (fallback azzurro 120,180,255), `compose_system_prompt(cfg, agent, tools_active)` = agent-or-global prompt + guarded tool/media/ANSI suffixes (never doubled) |
| `llm_models.h/.cpp` | **LlmModelRegistry**: parses `llm_models.jsonc` (opencode.jsonc `provider` schema — `options|settings.baseURL/apiKey`, `models.<id>.name`, `limit.context/output`, `thinking`, `//` comments allowed) into `provider/model` refs; `apply_model_ref()` fills `LlmConfig` base_url/api_key/model/context_size/supports_tools and sets `reasoning_effort` from the entry's `thinking`; `apply_agent_llm(out, base, agent, reg)` layers the agent's `model_ref` + `thinking` over the global config (precedence agent > model entry > global). File lookup: exe dir (.jsonc then legacy .json), then working dir; missing/invalid file falls back to explicit config fields |
| `md_format.h/.cpp` | **Markdown -> ANSI/SGR**: headings, bold/italic, inline code, lists (orange bullets), blockquotes, rules, links, tables (OpenCode-style box grid, violet header, GFM :---/---:/:---: alignment), fenced code blocks (dim language-label header bar + continuous dark block background incl. blank lines; optional `CodeHighlightFn` hook for the syntax highlighter). Source lines containing ESC pass through verbatim (terminal passthrough: ANSI art, pre-styled tool output) while the other lines keep their formatting; a line whose SGR sequences lost their ESC byte (models drop control chars) is repaired first via `ansi_repair()`, and language-less fenced blocks carrying an ANSI payload (2+ repairable lines) are repaired too, so pasted colored banners render colored instead of literal `[31m` text. The palette is a **`MdPalette`** struct of prebuilt SGR strings (h1/h2/bullet/italic/link/sep/code/code_bg/block_bg/table_head, opencode-ish defaults, `md_palette_default()`); `md_to_ansi(md, hl, pal)` takes it as an optional 3rd arg and `MdPalette::set(key, "#rrggbb")` overrides one entry (this is how config.jsonc `colors.md_*` reaches the formatter). `kAnsiUsagePrompt` tells the model the chat renders SGR colors |
| `formatters.h/.cpp` | **OutputFormatter**: plugin chain applied to raw LLM replies before display (GUI + CLI); kill-switch `gui.format_output`; carries the `MdPalette` built from `cfg.colors.md` into `md_to_ansi`; conversation history and TTS always use the raw text |
| `highlight.h/.cpp` | **Highlighter**: fenced-code syntax highlighting driven by data packs `data/languages/*.json` (keywords/types/builtins, line/block comments, strings, triple quotes, decorators). Per-line scanner with cross-line state for block comments/triple quotes; fg-only ANSI resets keep the block background. Shipped packs: python, cpp, json, jsonc, bash, powershell, javascript, cmake, ini |
| `llm_client.h/.cpp` | `LlmReply::reasoning` parsed from `reasoning_content`/`reasoning` or inline `<think>`/`<thinking>`/`<thought>` blocks (empty delimiters are impossible by construction: an empty open tag once spun `extract_inline_thinking` into an infinite loop that hung the worker after the reply arrived); the GUI shows it as a collapsible orange "Thought · Ns" block (click header to expand; body dim, not selectable in v1) |
| `emoji.h/.cpp` | **EmojiAtlas**: loads `data/fonts/emoji_atlas.png` (generated by `tools/gen_emoji_atlas.py` from Noto Color Emoji) into one GL_RGBA texture + codepoint->region map; the chat token renderer draws atlas codepoints as `nk_draw_image` sprite quads (square advance = line height), text runs via `nk_draw_text` as before. U+FE0F stripped; unknown emoji fall back to `?` |
| `icons.h/.cpp` | **IconAtlas**: rasterizes every `data/icons/*.svg` (Lucide set, ISC) into one white/alpha GL_RGBA sprite atlas at startup via **LunaSVG** (`libs/lunasvg` git submodule) — this is the ONLY translation unit that includes `lunasvg.h`, so the SVG library is a fully isolated, swappable dependency (static lib, `LUNASVG_DISABLE_LOAD_SYSTEM_FONTS/EXTERNAL_RESOURCES`, SVG bytes passed via `Document::loadFromData`, `currentColor` rewritten to white pre-parse). Icons drawn on the floating action buttons, the top-bar buttons (hands-free mic/mic-off, speak volume-2/volume-x, settings), the attachment chips (x) and the Settings modal Save/Close (check/x), tinted at draw time; fallback chain SVG -> emoji atlas -> text. `gui.cpp`'s `button_icon()` draws icon+text buttons manually (`nk_panel_alloc_space` + `nk_do_button` + `nk_draw_image`/`nk_draw_text`) because `nk_button_image_text` centers the text over the full width and paints it under the icon on narrow buttons. Headless self-test: `krastor --icon-test` rasterizes without GL, asserts per-icon ink coverage (2%..75%), dumps `icon_atlas_test.png` for visual inspection |
| `audio_fx.h/.cpp` | **AudioFx** (process-wide singleton): microphone input enhancement shared by `stt.cpp` (capture) and `tts.cpp` (playback reference tap). Pipeline per mic block (16 kHz mono float): SpeexDSP **acoustic echo cancellation** (160-sample frames, 256 ms tail, far-end ring fed by `push_playback()` from the TTS output callback, nominal 100 ms playback→mic offset with Speex's own delay estimator) → RNNoise **noise suppression** (resampled 16k→48k with the Speex resampler, 480-sample frames, back to 16k). ONLY translation unit including `rnnoise.h`/`speex/*.h` (isolation rule like `icons.cpp`); both libs are static submodules built via `libs/audiofx/`. Toggles `set_denoise()/set_echo_cancel()` from `stt.denoise`/`stt.echo_cancel` (config + Settings checkboxes, applied live). Headless self-test: `krastor --fx-test` asserts stationary-noise RMS drops >6 dB (RNNoise) and a synthetic 100 ms echo drops >6 dB (AEC), dumps `fx_test_in/out.wav` for listening |
| `conversation.h/.cpp` | message list (system + history), reset; `Message.images` carries base64 PNG attachments (vision input) |
| `llm_client.h/.cpp` | OpenAI-compatible `POST {base_url}/chat/completions`, JSON via nlohmann; messages with images are sent as multimodal `content` part arrays (`text` + `image_url` data URLs); parses the `usage` block (`prompt_tokens`/`completion_tokens`) + response `model` into `LlmReply`, falls back to a chars/4 estimate (`tokens_estimated`) when the server omits usage; `parse_usage_cost()` reads the vendor-extension `usage.cost` (OpenRouter et al.: number or numeric string, absent/invalid/negative = 0) into `LlmReply::cost_usd` — local servers never send it, so cost UI stays hidden |
| `http_client.h` + `http_client_winhttp.cpp` / `http_client_curl.cpp` | minimal HTTPS POST: **WinHTTP on Windows** (OS TLS, identical on MinGW+MSVC), **libcurl on Linux**. **`CancelToken`** aborts a blocking call from another thread: WinHTTP closes the in-flight request handle cross-thread (the documented abort trick, ownership exchanged atomically so it is closed exactly once), curl polls the flag in its progress callback |
| `voices.h/.cpp` | TTS voice registry: discovers `models/tts-piper-*`, **runtime voice switching** (`select()` callable at any time; `tts.cpp` reads `current()`) |
| `tts.h/.cpp` | **TTS engine**: Piper via sherpa-onnx C API (`SherpaOnnxOfflineTts`, espeak-ng `data_dir`), model (re)loaded lazily when the selected voice changes; every input passes through `tts_filter()` first (see `tts_text.h`) so the speakers only get plain prose; playback via a **dedicated `ma_device`** opened at the voice's native rate (mono float) — its data callback pulls the current utterance's PCM and feeds the exact samples reaching the speakers to `AudioFx::push_playback()` as the AEC far-end reference; `speak()` blocks until playback ends; **`interrupt()`** (atomic cancel flag checked by the callback and the wait-loop) enables barge-in; works silently when no output device exists |
| `tts_text.h/.cpp` | **tts_filter()**: pure Markdown -> plain-speech text transform, the voice-side mirror of `md_to_ansi` (applied once inside `Tts::speak()`, so no markup can reach the synthesizer from GUI, CLI `/say` or the voice-test FAB). Drops fenced code blocks silently (code-only reply -> nothing spoken), inline-code backticks, heading/bold/italic/strike/quote/list markers (text kept), links keep their text and drop the URL, bare URLs dropped, tables flattened to comma-separated cells, horizontal rules, HTML tags, emoji (UTF-8-aware, Italian accents preserved) and ANSI escapes; collapses whitespace. `snake_case` identifiers survive (word-boundary `_` kept). Self-test: `krastor --tts-filter-test` (table-driven, 20 cases) |
| `stt.h/.cpp` | **STT engine**: NVIDIA Parakeet TDT 0.6B v3 int8 (`nemo_transducer`) + Silero VAD via sherpa-onnx C API, mic capture via miniaudio (16 kHz mono float). The mic callback first passes every block through `AudioFx::instance().process()` (echo cancel + denoise, see `audio_fx.h`) before publishing it to the level meters, the visualization ring and the VAD feeders. `listen()` returns the first VAD-closed utterance (~0.5 s after speech ends); `transcribe_file()` decodes wav/mp3/flac to 16 kHz mono for headless tests; `list_input_devices()` + explicit `device_index` selection (`stt.input_device` in config, -1 = system default); `monitor()` prints a live level meter to diagnose muted/blocked/wrong mics. **Stream mode** (`start_stream()/stop_stream()/streaming()`): feeder thread drains the mic ring, runs the VAD in 512-sample windows and hands every closed segment to a callback; `recognize()` decodes raw PCM on the caller's thread (used by the GUI hands-free pipeline). All thread bodies are exception-guarded (an escaping exception would `terminate()` the process). EN+IT auto-detected, punctuation + capitalization included |
| `ansi.h` | header-only **ANSI/SGR parser**: splits text with escape codes into styled runs (fg/bg 16+256+truecolor, bold); unsupported sequences stripped. Powers the terminal-style chat renderer. `ansi_repair()` re-inserts the ESC byte that models often drop when copying colored tool output into a reply (bare `[31m`/`[0m` text) and converts the literal text `\033[` (octal escape written as prose) into a real ESC so no `\033` noise stays visible: only lines that clearly carry an ANSI payload (2+ SGR sequences or an explicit `[0m`) are touched, so prose like "wait [5m]" stays literal |
| `input_history.h` | header-only **console input history** (bash-style Up/Down recall): `push()` records submitted lines (trimmed, empty/consecutive duplicates skipped, oldest evicted past `gui.history_size`, 0 = off), `prev()`/`next()` walk the entries while browsing (Down past the newest returns the draft and ends browsing), `leave()` resets browsing when the user edits a recalled line. Key handling + caret placement live in `gui.cpp`; covered by `--gui-test` |
| `clipboard.h/.cpp` | clipboard **image** capture (Win32 `CF_DIB` → RGBA → PNG via stb; Linux stub), `load_image_file()` (any stb-readable image → PNG, used by drag & drop), `base64_encode()` for data URLs, `copy_image_to_clipboard()` (image bytes → CF_DIB bottom-up 32bpp — powers click-to-copy on chat images) |
| `media.h/.cpp` | **inline chat media** (images + mini sound player): `MediaBlock` (kind/src/req sizes/GL texture/native size, owned by a `ChatLine`), token parsing (`[image: src \| w \| h]`, `[sound: src]` on a dedicated reply line; sizes `-`/`*`/`auto` = auto, `base64:` payload = self-contained buffer, `clipboard` = volatile), `split_media_tokens()` (reply → ordered text/media segments; tokens never reach markdown or TTS), `media_fit()` (requested size, missing axis from the native aspect, then ALWAYS contained inside the text-area box, ratio preserved), `resolve_media_bytes()` (file/clipboard/base64), `SoundPlayer` (miniaudio `ma_decoder` from memory → full f32 PCM → own `ma_device`, separate from the TTS device; play/stop + position/duration for the UI, `make_beep_pcm()` test tone), `kMediaUsagePrompt` (system-prompt text: the model prefers `[image: ...]` to show images; appended in GUI + CLI, `Conversation::set_system_prompt()` re-syncs it after construction). Chat: click an image = copy to clipboard (CF_DIB). Headless self-test: `krastor --media-test` |
| `fft.h` | header-only iterative radix-2 FFT (512-pt, Hann window applied by caller) |
| `audio_analysis.h/.cpp` | **SpectrumAnalyzer**: mic window → 64 log-spaced amplitude bands (dB-compressed, fast-attack/slow-release smoothing) + 128×64 scrolling history ring uploaded as R8 texture; `synthetic(t)` feed keeps effects alive without the mic |
| `effects.h/.cpp` | **EffectEngine**: pluggable GLSL background — built-in gradient + `data/shaders/*.frag` plugins compiled at runtime on a shared fullscreen-triangle vertex shader with a fixed uniform contract (`u_resolution/u_time/u_level/u_spectrum/u_spec_head/u_listening/u_speaking/u_thinking/u_typing/u_copy/u_intensity`), events bundled in the extensible `EffectInput` struct (audio + LLM-thinking + key-press + clipboard-copy pulse); the plugin's `main()` is renamed via `#define main effect_main` and a real `main()` applies the global brightness (`u_intensity`, Settings "Effect brightness"); `scan()` lists plugins ("off" + built-in + `// krastor-effect:` display names), `select()` validates by compiling (broken plugin never replaces the running one; "off" disables the pass entirely), `check_reload()` hot-reloads on file change |
| `gui.h/.cpp` | **GUI (Phase 5)**: the window is the agent's main interface. Terminal-style chat panel with **styled-run rendering** (`nk_draw_text` per token): ANSI colors/bold/backgrounds, diff-style full-line backgrounds (a line fills its full width only when EVERY token shares one background; mixed lines like table rows paint per-token so inline-code gray never bleeds past the cell), indentation-preserving word wrap, **follow-bottom scrolling** (auto-pinned to the newest line; the mouse wheel — 3 lines/notch — or scrolling back up releases the pin, reaching the bottom re-enables it). **Chat text selection & copy**: the wrap pass records a per-frame pixel→(line, codepoint) hit-map; click/drag selects, double-click selects a word, triple-click the line, Shift+arrows/Home/End extend, Ctrl+A selects all, Ctrl+C copies ANSI-stripped **raw UTF-8** (emoji/accents preserved) to the clipboard, Esc clears; the input box keeps Nuklear's native selection/copy with `krastor_copy` overriding `device.ctx.clip.copy` so its Ctrl+C fires the toast too). **Copy toast** (opencode-style): a dark rounded pill with 3 px cyan side bars and "Copied to clipboard", bottom-center, shown by `AppState::notify_copied()` on every copy (chat selection, image click, input box); lives 2 s, a new copy resets the timer, fades over the last 0.3 s; a separate `NK_WINDOW_NO_INPUT|NOT_INTERACTIVE` Nuklear window with transparent fixed_background and **zeroed window padding** (the window clip is the content area: with default padding the edge bars were clipped away); the same event pulses `copy_pulse` into the shader feed (`u_copy`). **Per-response stats sub-line** under each reply (`model - time - tok/s`, dim gray, `~` prefix when tokens were estimated; elapsed measured in the async worker) and a **context-usage footer** on the status line (`ctx: N / total (x%)`, thousands separators, amber past 85%; total = `llm.context_size`, occupancy = last reply's `prompt_tokens`); while waiting for the LLM the status line shows a fixed-length `thinking…` with an opencode-style shimmer: a band of light sweeps across the glyphs (per-glyph `nk_draw_text` with a lerped color, 1.6 s coast cycle, smootherstep head/tail, faint resting tint floor — see `draw_shimmer_text`; glyph positions and sweep front must share cell units), followed by an opencode-style indeterminate block meter (`draw_block_meter`: 6 squares, light wave sweeping, 1.4 s cycle) and an `esc interrupt` hint — **Esc really interrupts** the running request (global edge-detected handler calls `stop()` while Thinking/Compacting; idle Esc still clears the chat selection). In the chat itself a blinking white block cursor (filled rect, 1.1 s cycle, ~55% duty) sits on a fresh line under the newest content while thinking; trap: inside the scrolled chat group use `nk_widget_bounds` (subtracts the group scroll offset), NOT `nk_layout_widget_bounds` (raw content coords — the cursor drew off-screen). **Ctrl +/-/0 font zoom** (ladder 12–30 px baked into the atlas at startup, `nk_style_set_font` swap, all row heights proportional to font height, debounced auto-save of `gui.font_size`). **Multiline input** — OpenCode-style full-width dark box with a 3 px blue accent bar and a 2 px blinking caret (Enter sends, Shift+Enter newline, 16 KB buffer), cut/copy/paste via GLFW clipboard + custom paste hook (Ctrl+V pastes text **or attaches a clipboard image**), **drag & drop** (images → attachments, text files → inserted into the input), attachment chips (x icon + size label, click to remove), a **floating action-button cluster** in the lower-right corner (icon-only SVG icons (data/icons via LunaSVG, fallback emoji-atlas sprites) with hover tooltips: send 🚀, attach image 📎, listen 👂 (Parakeet STT, auto-sends the transcription), test voice 🎵, stop ❌; dimmed while the worker is busy), **Speak ON/OFF** auto-TTS toggle (volume-2/volume-x icon), status line, gear icon → **settings modal** (agent name, model picked from `llm_models.jsonc` (URL/key/context come from that file), voice, background effect + brightness, temperature, max tokens, system prompt, ignore-while-busy checkbox; Save/Close carry the check/x icons; Save writes state.json next to the exe and hot-reloads the LLM client). **Hands-free continuous cycle** (top-bar "Hands-free: ON/OFF" toggle (mic/mic-off icon; the whole top bar is compact: `fh*1.7` row, columns only as wide as the labels need), auto-starts when `agent.continuous`): Stt stream mode → pipeline worker thread (`recognize`) → utterance queue → render loop drains it and reuses `send()` (single LLM worker path); echo guard drops segments while thinking/speaking when `agent.ignore_while_speaking`; with the guard **off**, a heard utterance while speaking triggers **barge-in**: `tts.interrupt()` cuts playback and the utterance is processed. **Configurable agent name** (`agent.name`): window title, top bar, chat prefix and greeting. **Effect brightness** slider (`gui.effect_intensity`, 0.2–3, debounced auto-save) → `u_intensity` uniform. **Window size persistence** (`gui.window_w/h`): window created at the saved size (clamped ≥ 480×360), 500 ms debounced autosave on resize + final save on clean exit. The main window is fully transparent (shader shows through); readability comes from the translucent chat group (alpha 105) and the opaque Settings modal (pushed `fixed_background`). UI font: JetBrains Mono (`data/fonts/`, fallback to built-in). LLM/STT/TTS run on worker threads (`std::async`/`std::thread`) so the 60 fps loop never blocks; a **GLSL fragment shader** draws the animated background behind the UI. `KRASTOR_UITEST=1` env seeds styled test lines + clipboard image and auto-sends one short LLM message (screenshot self-test of the stats line + footer). Launched with `krastor --gui`. **Multi-agent**: the top-left name is an `nk_combo` listing every `config.jsonc` agent (items in their own color, `*` on the active one, 3 px identity bar on its left edge); **Shift+Tab** cycles circularly (edge-detected; the buffered `\t` is dropped from the glfw char queue so the input box never eats it). `AppState::apply_agent(id)` is the single switch path (combo/hotkey/session restore): persona name (top bar + window title), accent color, effective LLM config (`apply_agent_llm`: agent model_ref + thinking over the global), the agent's `permissions` override (`ToolRunner::set_agent_permissions`, merged last so an agent can tighten - e.g. the read-only "Krastor read" writes only to `%TEMP%` - never widen the floor) and recomposed system prompt; refused while the worker is busy. The agent color tints the input accent bar + caret, the `thinking…` shimmer sweep and the block meter (`draw_block_meter` takes the color); the chat block cursor stays white. **Block scheme** (opencode): every block is `[colored bar bar_w][bar_gap][rounded panel in colors.block_bg with pad_x/pad_y inner padding]`, blocks separated by `block_gap` - all five px values live in the user-owned config.jsonc `chat` section. "you" messages wear the sending agent's color (`ChatLine::bar_r/g/b`, captured at send time, restored messages get the session agent's color); tool/thought/diff/media blocks wear the dim meta gray; assistant replies and `! ` sys lines stay plain (no bar, no panel). The panel background must be painted BEFORE the content rows (painter's order), so each block's height is measured first with dry passes (`draw_chat_line`/`draw_diff_block`/`draw_media_image`/`draw_media_sound` all take `indent` + `dry` params: dry = measure-only, no layout no draw) and the top padding row doubles as the layout probe whose slot anchors the panel rect. No `> ` prefix anywhere, assistant replies carry **no role prefix at all** (the agent name lives in the top bar). Chat colors come from the user-owned `colors` scheme (parsed once in the AppState ctor): white body text, gray `! ` system lines, dim meta, orange thought header. `KRASTOR_UITEST_AGENT=<id>` starts on that agent (screenshot color check). |

Verified (both `mingw-debug` and `msvc-2026-debug`): `/say` speaks IT+EN through the
speakers with runtime voice switching; `/transcribe` returns exact transcriptions
(punctuation, accents) of generated test WAVs in both languages; `/listen` opens the
microphone, runs the VAD loop and times out cleanly on silence. sherpa-onnx binaries
are git-ignored — run `tools/download_deps.py` once after cloning.

## External tools system (Phase 6, 2026-11-07)
See `../tools_plan.md` for the full design. The agent calls external `.exe`
tools through OpenAI function calling; Krastor supervises every call.
| Module | Responsibility |
|---|---|
| `permissions.h/.cpp` | Policy engine: `ToolPolicy` (per-class read/write/net/run rules with glob `allow`/`deny` + `ask`, plus `deny_always` floor), glob matcher (`**` crosses separators, `*` stays in a segment, case-insensitive on Windows), `canonical_path()` (home-expand + weakly_canonical), `workspace_dir()` (`~/Documents/KrastorWorkspace`, created on demand), `tool_usage_prompt()` (system-prompt text: sandbox path + "simplest tool first, python is the LAST resort" rule; appended by `compose_system_prompt` in GUI + CLI **guarded** against double-append - old saved configs carry the suffix inline, an unguarded append duplicated it 24x), `GrantStore` (session-scoped "always allow" prefixes + tool-level grants for targetless run checks - "Always allow" on python approves the tool itself, scoped to that tool only; `list()`/`clear()` back the `/grants` and `/grants clear` commands - the way to inspect or revoke grants without restarting), `AskHandler` interface (Allow/Always/Deny). Precedence: deny_always > deny > allow > ask > default; `ask=true` with an empty allow list gates **everything** (that is how `run.ask` makes every python execution ask) |
| `tool_registry.h/.cpp` | Scans `tools_dir()` (exe/../tools, fallback exe/tools) for `tool_*.jsonc` JSONC manifests: name/description/input_schema forwarded to the LLM, `exe`/`args` (fixed argv, resolved against the manifest dir) /`cwd` ("workspace" sandbox or tools dir)/`timeout_ms`/`path_args`/`net_args`/`ask_actions`/`action_class`/`setup_hint`; config.jsonc `tools.<name>.{enabled,permissions}` overrides; `tools_json()` builds the OpenAI `tools` array |
| `tool_runner.h/.cpp` | Spawns a tool (CreateProcessW + inherited pipes on Windows, fork/exec + dup2 on POSIX), writes the args JSON + injected `_policy`/`_workspace`/`_approved:true` on stdin, captures stdout/stderr (200 KB cap, hard timeout kill), parses the `{"ok":...}` envelope. Permission gate before spawn: path args canonicalized + checked, `net_args` (URLs) checked as strings, empty-path check for run; **`ask_actions` force Ask even against session grants** (file deletion always confirms; "Always allow" never silences them); deny floors still win over ask. **Windows pipe rule: the child's ends stay inheritable, the parent's ends are cleared with SetHandleInformation** (reversed flags -> broken pipe err 232) |
| `tool_loop.h/.cpp` | `agent_complete()`: complete() + agentic loop — tool_calls go through the runner, results are appended as `role:"tool"` messages and the model is asked again; hard cap 8 rounds, tool output capped at 8 KB per call, hooks (`on_call`/`on_done`) for UI/status rendering; the turn's `cost_usd` is the sum of every round's API-reported cost (`next_round` carries the running total), while `prompt_tokens` stay the final round's = current context occupancy; the filetool edit/write `diff` member is stripped from the message fed to the model (`strip_diff_for_model`, the GUI reads it from `on_done`) so diffs never bloat the context |
| `llm_client` (tools) | `complete(conv, cancel, tools_json)` attaches the `tools` array; parses `finish_reason: tool_calls` replies (`content` may be null) into `LlmReply::tool_calls`; `Message` carries `tool_calls`/`tool_call_id` for serialization |
| `config` (tools) | `ToolsConfig`: `tools.enabled` master switch + raw per-tool `overrides_json` round-trip; `LlmConfig::supports_tools` set from `llm_models.jsonc` `capabilities.tools` - tools are only offered when the model declares function calling |
| `gui.cpp` (tools) | `AppState : AskHandler` — worker blocks in `ask()` until the **permission modal** (Allow once / Always allow / Deny) answers; the modal dismisses as soon as the request is answered (`ask()` clears `pending_ask` on wake, the modal also skips an answered request) and its detail box gets an explicit row height so the args/paths scroll visibly; completed tool calls drain in `poll()` as collapsible `⚙ tool · action` chat blocks (reuses the Thought-block renderer via `think_label`; python-style `{"stdout","error"}` envelopes are decoded to raw text by `tool_result_body` - tolerant `json_string_field` scanner for cap_text-truncated restores - so ASCII art keeps its lines and ANSI colors render, and `tool_args_body` decodes a python `code` arg); `files edit/write` results carrying a structured `diff` become a collapsible **side-by-side diff block** instead (`draw_diff_block`: clickable `← Edit path +N −M` header via `diff_rects`, old|new panes with red/green row backgrounds + dim line numbers, unified single-column fallback under 640 px, hunk-gap separators, 240-row view cap, rows clipped not wrapped; `KRASTOR_UITEST` seeds a sample diff block); Settings checkbox "Enable external tools (restart)" |
| `main.cpp` (tools) | CLI `ConsoleAsk` (y/N/a prompt), tool system-prompt hint with the sandbox path, debug flags: `--tool-run <tool> <json>` single call (auto-allow), `--tool-test` headless self-test (44 checks: sandbox write/read/edit/list, delete confirm+deny+floor, deny floors, ask-deny honour, read-only agent overrides, webfetch text + file:// block, websearch results [SKIP when DDG's anti-bot page answers], python run + traceback + ask gate, always-allow stickiness for targetless run tools) |

## Sessions system (Phase 7, 2026-11-07)
See `../sessions_plan.md` for the full design. Persistent chat sessions,
session switching and LLM-driven compaction.
| Module | Responsibility |
|---|---|
| `session_store.h/.cpp` | **SessionStore**: one folder per session under `sessions/` (exe/../sessions, fallback ./sessions, `sessions.dir` override): `meta.json` (title/dates/counters + `agent` — the active agent id, restored when the session opens; absent in pre-agent metas, tolerated — + `prompt_tokens`/`cost_usd` — context occupancy + session cost for the footer meter, restored on open, absent in pre-usage metas = 0) + append-only `log.jsonl` (one message per line, images stored as `attachments/img-<n>.png` file refs, base64 in/out) + `archive/pre-compact-<n>.jsonl`. `create(title,model_ref,agent)/append/load/rename/set_agent/set_usage/remove/list/meta_of/replace_log_after_compact`; `attachment_paths(id,n)` predicts the paths append() will write (the GUI puts them in the user message so the model can re-find its pasted images); malformed log lines are skipped on load (crash tolerance); ids are `YYYYMMDD-HHMMSS-xxxx`; `is_summary_message()` recognizes `[Conversation summary` markers. Self-test `krastor --session-test` (44 checks) |
| `compactor.h/.cpp` | **compact_conversation()** (opencode v2 style): budget = `context_size - max_tokens - headroom(≥512, ctx/16)`; recent tail kept verbatim (≤ budget/5, never starting on a `tool` result), head split into ≤70%-budget chunks (tool_call/tool pairs never split) and folded rolling into one summary by the *active* LLM; **fixed structured template** prompt (Objective/Requirements/Decisions/Work State/Next Move/Relevant Files/Important Context, terse bullets, exact paths/commands preserved, summary in the conversation's language) with **update mode** (history starting with the summary marker folds newer messages into the checkpoint instead of restarting); transcript escalates through 5 clipping levels (full → outputs clipped to 2k head+tail → outputs dropped → user text only → omitted) until it fits the budget; `compact_buffer_tokens("NN%"|tokens, ctx)` = max(NN% of window, min(16k, ctx/2)) for auto-compaction; new history = summary user-message + tail. Never throws, cancellable, progress callback for the UI; `CompactResult::cost_usd` sums the summarization calls' API cost, `estimate_conv_tokens(msgs)` exposes the ~4 chars/token estimate (session-restore fallback when meta predates `prompt_tokens`). Test hooks `compactor_test_prompt/transcript/est` (`--session-test`) |
| `conversation.h/.cpp` (sessions) | `replace_history()` (system prompt kept) + `messages_since(idx)` autosave cursor |
| `config` (sessions) | `SessionsConfig`: `sessions.dir` / `autosave` / `continue_last` / `auto_compact` (default true: compact at turn end when the last prompt came within `compact_buffer` of `context_size`) / `compact_buffer` (`"NN%"` or raw tokens, floored at min(16k, ctx/2)) / `last_id` (state.json only: the session active on the previous run, resumed at startup) |
| `main.cpp` (sessions) | startup session pick (`--session` > `--new-session` > continue-last > new), autosave after every turn (auto-title from the first user message), commands `/sessions`, `/session info|new|open|delete|rename` (index `#N`, id or unique prefix), `/compact [focus]`, `/title`; `/reset` now starts a new session folder; flags `--list-sessions` / `--delete-session` / `--compact` exit before the voice stack |
| `gui.cpp` (sessions) | `AppState::switch_to_session()` is the single switch path (autosave-before-load, refused while the worker is busy; records `sessions.last_id` in state so a restart resumes the session in use): top-bar **session button** (current title, fitted to its column; opens the session modal), **Ctrl+O** session modal (list/open, delete with inline "sure?" confirm, New/Rename/Close footer sized to fit the window exactly), **Ctrl+Shift+O** cycles recent; top-bar action buttons are **icon-only** (`button_icon_only`, no labels); slash commands in the input box (`/sessions /session /compact /clean /title`); `/clean` = `clean_screen()` clears the visible chat only (session + LLM history untouched), also a floating button (`eraser` icon); help overlay via **F1**, `/help` or the `help` floating button; autosave in `poll()` when a turn completes; `/compact` runs on the worker (`Worker::Compacting`, Stop cancels); **auto-compact** after each turn (`maybe_auto_compact()`, `sessions.auto_compact`) and **overflow recovery** (`is_context_overflow()` → compact + `launch_llm()` retry once per turn, `launch_llm()` is the worker-start extracted from `send()`); window title follows `name — session`; `rebuild_lines_from_conv()` re-renders a loaded history (tool calls as collapsed `⚙` blocks, summary as divider) |

## Conventions
- C++20, no compiler-specific extensions without a guarded abstraction
  (must build with both MinGW GCC and MSVC). Example guard: WinHTTP vs libcurl
  selected by CMake per platform.
- English comments. Warnings: `/W4 /utf-8` on MSVC, `-Wall -Wextra` on GCC.
- UTF-8 console output on Windows (`SetConsoleOutputCP`).

## Gotchas learned
- MinGW's and MSVC's `winhttp.h` both use `URL_COMPONENTS.nPort` (not `nPortNumber`).
- `#pragma comment(lib, ...)` must be guarded with `#ifdef _MSC_VER` (GCC warns).
- The MinGW install is compiler-only (no curl/openssl headers) — hence WinHTTP.
- Nuklear (master 4.13): text field is `nk_edit_string_zero_terminated` (no
  `nk_input_text`); alignment uses combined enums (`NK_MIDDLE_LEFT`, `NK_MIDDLE_CENTER`,
  not `NK_CENTER`); window-close check is `nk_window_is_closed`;
  `nk_label_colored_wrap(ctx, text, color)` takes **no** alignment argument.
- **Nuklear is locally patched for soft-wrap** (see `libs/development.md`):
  multiline edit boxes wrap at `nk_text_edit::wrap_width` via
  `nk_textedit_wrap_scan`, shared by `nk_textedit_layout_row`,
  `nk_edit_draw_text` and the cursor/selection/scrollbar calc loop in
  `nk_do_edit`. Anything that touches edit drawing/selection must keep all
  three in sync (one text pass, selection cut per visual row) or rows
  drift when a selection appears. Re-applying the patch after a Nuklear
  upgrade means redoing those four spots + the `nk_edit_draw_text`
  signature (5 call sites).
- Nuklear layout/render gotchas (cost hours):
  - `nk_layout_row_dynamic(ctx, 0, 1)` does **not** fill remaining space — it makes a
    min-height row. Give scrollable groups an explicit pixel height (window height
    minus the other fixed rows) via `nk_layout_row_begin`+`nk_layout_row_push(1.0f)`.
  - The window body is painted from `style.window.fixed_background` (use
    `nk_style_item_color(...)`), **not** `style.window.background` (that only colors
    dynamic panels/groups). Set both for a uniform translucent look.
  - `nk_window_set_scroll` scrolls the **outer window**, not the current group; use
    `nk_group_set_scroll(ctx, id, x, y)` with the group's own id. Nuklear only clamps
    the offset while the scrollbar is visible, so compute the bottom offset yourself
    rather than pinning to a huge value — but take it from the group's **actual
    layout end** (`panel->at_y + row.height − bounds.y − bounds.h`, the same value
    Nuklear's own scrollbar max uses), not from a parallel sum of measured row
    heights: if the two ever diverge, clamping to the smaller one leaves the last
    lines unreachable below the clamp ("scroll sticks, last lines invisible").
    **Read with `nk_group_get_scroll`, never `nk_window_get_scroll`**: inside a group
    `ctx->current` is still the main window (groups are panels, not windows), so
    `nk_window_get_scroll` returns the main window's `scrollbar.y` (always 0) while
    the group's live offset lives in the parent window's value store — mixing the two
    makes wheel/PageUp/PageDown never accumulate (each notch re-computes from 0).
  - **`nk_glfw3_render` buffer args are BYTES, not vertex counts.** The small default
    (4096) holds ~200 vertices and silently drops every command after them — the
    classic "only the first row renders" symptom. Use `512*1024, 128*1024`.
  - **`nk_fill_rect` truncates x/y/w/h to integers** (`cmd->x = (short)rect.x`).
    With fractional layout coordinates (fractional font metrics, DPI scaling)
    adjacent per-token background fills lose up to ~1px right/bottom each and
    leave 1px dark seams — the "black border around spaces" on colored chat
    lines. Round the rect **outward** (`floor(x)`, `ceil(x+w)`) before filling
    (`fill_px()` in gui.cpp); later fills draw over the ≤1px overlap.
- glad 0.1.36 CLI: `--api "gl=3.3" --profile core`, and it reads `gl.xml` from the
  CWD when present (downloaded from Khronos if absent).
- sherpa-onnx: use the **C API** (`c_api.h`) — the MSVC-built import lib links fine
  from MinGW when only `extern "C"` functions are used. Pin **v1.13.8**: v2.0.0
  ignores `data_dir` (espeak-ng) for VITS, which breaks Piper voices.
- miniaudio: pin the **0.11.22 release** header (master renamed the buffer/decoder
  APIs). `ma_decoder` only accepts containers, so raw float PCM must be wrapped
  in a 44-byte in-memory WAV header (still used by file decoding; TTS playback
  now streams through a dedicated `ma_device` data-callback instead).
- Input DSP libraries (see `src/audio_fx.h`): **RNNoise's float API expects
  int16-scaled samples (±32768)**, not normalized ±1 — feeding ±1 audio makes
  it "denoise" silence and output near-zero (the `--fx-test` denoise stage
  caught exactly this). The **Speex AEC API is int16-only** (`speex_echo_cancellation`),
  so both stages scale in/out around the library calls. Neither rnnoise nor
  speexdsp ships CMakeLists.txt (autotools only): `libs/audiofx/` compiles the
  needed sources and provides a hand-written `config.h` for SpeexDSP
  (`HAVE_CONFIG_H`). RNNoise's model tables (`rnnoise_data.c/h`) are **not in
  the git repo** — `tools/download_deps.py` fetches the sha256-addressed
  tarball into git-ignored `libs/rnnoise-data/`.
- Nuklear 4.13.3 specifics (chat renderer):
  - There is **no public `nk_text_width`** — measure text via the font callback:
    `font->width(font->userdata, font->height, s, len)`.
  - `nk_font_atlas_add_from_file` is compiled out unless `NK_INCLUDE_STANDARD_IO`
    is defined; load the TTF into memory and use `nk_font_atlas_add_from_memory`.
  - `nk_draw_text` truncates text to the rect width, so per-token drawing must
    pass a rect at least as wide as the token (`nk_text_width`-measured + 2).
  - `nk_widget_bounds(ctx)` peeks at the last allocated widget's rect without
    consuming a slot — used to get absolute pixel coords for manual drawing.
  - Clipboard plugin signatures: `nk_plugin_paste = void(*)(nk_handle,
    nk_text_edit*)` — the callback must call `nk_textedit_paste` itself; the glfw
    backend installs text-only paste, we override `ctx.clip.paste` to fall back
    to clipboard-image attachment when the clipboard has no text.
  - `NK_EDIT_BOX | NK_EDIT_SIG_ENTER` mixes two enum types → `-Wdeprecated` on
    both toolchains; cast each flag to `nk_flags` before `|`.
  - Enter handling inside `nk_textedit_key`: with `NK_EDIT_CTRL_ENTER_NEWLINE`,
    Shift+Enter inserts `\n`; else `NK_EDIT_SIG_ENTER` sets `NK_EDIT_COMMITTED`;
    else Enter inserts `\n`. So multiline + Enter-sends works with
    `NK_EDIT_BOX | NK_EDIT_SIG_ENTER | NK_EDIT_CTRL_ENTER_NEWLINE`.
  - Text injected into an edit buffer from outside the widget keeps Nuklear's
    old caret; while the field is active `nk_edit_string` re-reads
    `win->edit.cursor/sel_start/sel_end` every frame, so writing them (char
    index via `nk_utf_len`) right before the call places the caret - used by
    the input-history recall to land the caret at the end.
  - A `//` comment ending with a backslash swallows the next source line (line
    continuation) — bit us in `ansi.h` ("ESC \" comment).
- Win32 clipboard images: `CF_DIB` via `GlobalLock`; DIBs are bottom-up unless
  `biHeight < 0`; rows padded to 4 bytes. Re-encode to PNG with stb for the
  OpenAI `image_url` data URL.
- Nuklear 4.13.3: `nk_style_set_font` sets `style.font` and resets the min row
  height - in this version widget styles have **no** own font fields, so one call
  rescales every widget. `nk_font_atlas_add_from_memory` **borrows** the buffer
  until `nk_font_stash_end` (keep it alive until then).
- Nuklear 4.13.3: a `nk_group` is a **panel inside the parent window, not a
  window** - `nk_window_get_bounds()` inside a group returns the *parent
  window's* rect (made every chat hover test true for clicks anywhere, which
  unfocused the input edit on every click). The group's own rect is
  `ctx->current->layout->bounds`.
- Nuklear draws `style.window.fixed_background` for **every fixed window**
  (no flag needed) - a translucent main window must set its alpha to 0, and
  popups/modals need their own pushed background. Dynamic windows (groups,
  combos) use `style.window.background` (an `nk_color`, push with
  `nk_style_push_color`, not `push_style_item`).
- Nuklear 4.13.3 title-bar **X button sets `NK_WINDOW_HIDDEN`, never
  `NK_WINDOW_CLOSED`** (nuklear.h close-button handler), so
  `nk_window_is_closed()` never fires for it. If app state then keeps calling
  `nk_begin()` on the hidden window, it returns 0 with `win->layout == 0` and
  the next widget call trips `NK_ASSERT(ctx->current->layout)`
  (e.g. `nk_row_layout`, nuklear.h:22370) - abort, no C++ exception, so the
  `set_terminate` logger stays silent. Correct closable-window pattern:
  `if (!nk_begin(...)) { nk_end(ctx); nk_window_show(ctx, name, NK_SHOWN);
  ...treat as closed...; }` - the `NK_SHOWN` un-hide lets the window be
  reopened later (and lets the GC free it meanwhile).
- Nuklear's `nk_input` is **global**: an overlapping window (e.g. the Settings
  modal) does not swallow input for windows beneath it, so hover-rect checks
  (`nk_input_is_mouse_hovering_rect`) fire for clicks/drags/wheel landing on
  the modal too. App-level windows that overlap must gate their own input on
  "modal open" (see `modal_blocks` in `chat_panel`).
- Nuklear 4.13.3 has **no** `nk_window_get_cursor` / `nk_layout_row_bounds` -
  to custom-draw over the next widget's slot (e.g. the input box accent bar),
  compute it after `nk_layout_row_begin`+`push`: top = `ctx->current->layout->at_y`
  (panel_layout advances at_y to the row top), x = `bounds.x +
  style.window.padding.x`, width = `nk_window_get_width()`. Draw with
  `nk_fill_rect(&ctx->current->buffer, ...)` **after** the widget call so the
  command lands on top of its background.
- Shader `main()` wrapper trick: prepend `#define main effect_main` + a
  `void effect_main();` prototype, append the real `main()` after
  `#undef main` — without the `#undef` the footer's `main` is renamed too and
  GLSL reports "already defined".
- GLSL `pow(x, y)` is **undefined for x < 0** — use `x*x` for squared falloff
  (`exp(-pow(r/a, 2.0))` silently renders garbage/negative-radius regions).
- An uncaught C++ exception on **any** thread calls `std::terminate` → MSVC
  "Debug Error! abort() has been called" with zero context (the GUI has no
  console). Mitigation: every thread body + `future::get()` is try/caught, and
  `main()` installs a `std::set_terminate` handler that logs the real cause to
  `exe_dir()/krastor_error.log`.
- `std::filesystem::rename` replaces an existing file on MinGW+MSVC → safe
  atomic-save primitive (write `.tmp`, rename).
- nlohmann 3.12: `ignore_comments` defaults to **false** in `parse()` and
  `operator>>` - parsing jsonc (`//` comments, e.g. `llm_models.jsonc`,
  `config.jsonc`) needs
  the explicit `json::parse(file, nullptr, /*allow_exceptions*/true,
  /*ignore_comments*/true)`.
- CreateProcessW + `STARTF_USESTDHANDLES`: with `bInheritHandle=TRUE` on the
  pipes, the **child's** ends (child stdin / parent read / parent write ends
  handed to the child) must stay inheritable and the **parent's** opposite
  ends must be cleared via `SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0)`.
  Reversed flags leave the child holding the parent's write end: the child
  never sees stdin EOF and the parent's reader never terminates -> "tool
  produced no output (exit 0)" / `WriteFile` err 232 (ERROR_BROKEN_PIPE).
- `std::filesystem::path::home_path()` and `std::filesystem::clock` are
  **missing in MinGW libstdc++** (C++20 library gaps): read `USERPROFILE`/
  `HOME` for the home dir and `GetFileTime` (1601-based 100 ns ticks, shift
  11644473600 s) for file mtimes instead. Raw `file_time_type::
  time_since_epoch()` prints 24th-century dates on Windows.
- Tool self-enforcement protocol: Krastor injects `_policy`, `_workspace` and
  `_approved:true` into the args; a tool must apply `deny_always` **always**
  (even approved calls) and the full policy when `_approved` is absent (manual
  terminal run), so a compromised/buggy caller cannot bypass the floor.
- Chat selection codepoint numbering must be **cumulative across all ANSI
  runs** of a message body (`tokenize_runs`' `lcp`): a per-run restart makes
  tokens of different runs (inline code, bold, colors) share offsets, so a
  dragged range highlights one scattered word per run instead of the span.
- A Nuklear `nk_group_begin` only gets space if a layout row was pushed
  before it (`nk_layout_row_dynamic(ctx, h, 1)`) - without one a modal's
  detail group collapses to a sliver with a useless scrollbar.
- Blocking-dialog lifecycle: the asking worker must clear the pending-request
  pointer when it wakes up (and the modal must skip an answered request) -
  otherwise the dialog keeps re-rendering after the click and keeps blocking
  input while the worker already resumed.
- This Nuklear (4.13.3) has `nk_button_label`, NOT `nk_label_button` (that is a
  newer/other API name) - the label-button helper does not exist here.
- `(cond ? "" : " ") + std::string` fails to compile: both branches are
  `const char*`, so `+` is pointer arithmetic, not string concat. Build the
  string explicitly (`if (!t.empty()) t += " ";`).
- Session autosave ordering: always flush the OUTGOING session (autosave)
  BEFORE loading/creating another one - after `conv` was replaced the pending
  tail belongs to the wrong session and would be appended to the new folder.
- Modal scrollable groups must size from the LIVE layout, and the exact
  formula is: after the top rows, `remaining = nk_window_get_content_region(ctx).y
  + .h - (layout->at_y + layout->row.height)`, group gets `remaining - footer
  row - 3*spacing`. Three traps found the hard way:
  1. `nk_window_get_content_region` returns `layout->clip` (the window's WHOLE
     content rect, NOT the remaining space); fixed `mh - K` formulas overflow
     once title bar and spacing are counted.
  2. `nk_panel_layout` advances `at_y` only when the NEXT row is declared, so
     the just-drawn row is missing from `at_y` - subtract `layout->row.height`
     too (it already includes the row spacing). Missing this overestimated the
     space by exactly one row: outer scrollbar + footer buttons cut off.
  3. groups KEEP their scroll offset across frames, so call
     `nk_window_set_scroll(ctx,0,0)` right after `nk_group_begin` (a group IS a
     window; `nk_group_set_scroll_id` does not exist in 4.13.3) or a
     previously-scrolled box opens looking empty.
  Belt-and-braces: modals also pass `NK_WINDOW_NO_SCROLLBAR`, so the only
  scrollbar that can ever appear is the group's own inner one.
- Nuklear `nk_button_label`/`nk_label` do NOT clip their text: a string wider
  than its layout column bleeds over neighbouring widgets and past the window
  border. Truncate to the column width first (`fit_text` in gui.cpp: UTF-8
  codepoint-safe, appends "..", measures with `text_w_px`).
- The vendored nuklear.h dark table sets `NK_COLOR_TOGGLE_CURSOR` to the
  window color: a checked checkbox looked empty and the unchecked gray one
  looked filled (state read inverted). gui.cpp overrides
  `style.checkbox.cursor_normal/cursor_hover` with a bright accent after
  `nk_glfw3_init`.
- `style.window.padding` is cached into the layout clip by `nk_begin` - to
  inset a window's content, push it BEFORE `nk_begin` (pop after `nk_end`).
  `style.window.spacing` is different: `nk_panel_layout` reads it live per
  row, so pushing it after `nk_begin` works.
- The fullscreen main window MUST be submitted with `NK_WINDOW_BACKGROUND`:
  a click anywhere inside a window moves it to the top of Nuklear's stack, and
  since the main window's bounds contain every point, any click on the chat
  input would push it above the open modals - which can then never regain the
  top (the fullscreen window always "covers" them) and get marked
  `NK_WINDOW_ROM`: every widget inside, including the title-bar X, silently
  stops responding (frozen Settings modal). A BACKGROUND window is never
  raised and never gets ROM; clicking a modal instead transfers activation to
  it. Regression test: `krastor --gui-test` drives a headless nk_context with
  synthetic click frames (press and release must be SEPARATE frames: 4.13.3
  buttons trigger on press, `nk_input_is_mouse_pressed` needs `down && clicked`).
- The chat group's wheel scroll is handled by the app itself (it zeroes
  `input.mouse.scroll_delta.y` so Nuklear never sees it). Two traps: the
  handler runs BEFORE the content is drawn, so it can only clamp at the
  PREVIOUS frame's bottom (`chat_bottom`) - an unclamped overshoot is
  rendered immediately (wheel bounce at the end); and the follow-bottom pin
  must be released when the post-`group_end` offset is below the bottom
  (scrollbar drag), or dragging up is impossible.
- Media tokens must be split out of an LLM reply BEFORE `fmt.format()`
  (markdown would eat the square brackets) and out of the TTS text (the
  voice must not read them). Image textures are uploaded lazily on the
  RENDER thread at first draw - worker threads never touch GL; the bytes
  are freed after upload except for volatile `clipboard` sources.
- This vendored miniaudio is pre-0.7 API: `ma_device_init/uninit` on a
  value (no `ma_device_open`), format enum is lowercase `ma_format_f32`,
  decoded rate read from `decoder.outputSampleRate` (no getter). Nuklear
  4.13 `nk_progress` takes `nk_size*` + max (0..100 style), not a float.
- Tool output is untrusted bytes: truncating at a fixed BYTE count can cut
  a UTF-8 codepoint, and nlohmann then throws `type_error.316` at dump time
  - fatal when the throw lands on the main thread (session autosave).
  `cap_text` cuts at a codepoint boundary and `utf8_clean()` sanitizes
  every tool result before it enters the conversation.

## Planned layout (roadmap)
- `gui/` — GLSL effect passes, terminal I/O panel, attachments input (Phase 5)
- STT/TTS live directly in `src/` (`stt.*`, `tts.*`, `voices.*`) — no `media/` subfolder needed
