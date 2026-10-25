# Chat UX Plan — response stats, context footer, window size, scrolling, selection & copy

Detailed design for the next Phase 5 increment. Four features, in commit order:

1. **Per-response LLM stats + footer context usage**
2. **Window size persistence**
3. **Mouse-wheel / keyboard scrolling of the chat (follow-bottom)**
4. **Text selection & copy in the chat panel (mouse + Shift+arrows)**

Decisions locked with the user:
- Context window size: **no server probe** (LM Studio's `/v1/models` exposes none) —
  a Settings field `llm.context_size`, **default 131072 (128K)**.
- Copy format: **raw UTF-8** (emoji, accents, everything preserved), ANSI codes stripped.
- Per-response stats line format: **`model - time - tok/s`** (no agent-name prefix).

---

## Step 1 — Per-response stats + footer context stats

### 1a. Usage plumbing (`src/llm_client.h/.cpp`)
- Extend `LlmReply` with:
  - `std::string model;` — from the response JSON `model` field, fallback `config_.model`.
  - `double elapsed_ms;` — measured by the caller (the async worker lambda in
    `gui.cpp send()`), not inside `complete()`.
  - `int prompt_tokens = 0, completion_tokens = 0;`
  - `bool tokens_estimated = false;`
- Parse the OpenAI `usage` object from the chat completion response
  (`usage.prompt_tokens`, `usage.completion_tokens`). When absent (some servers omit
  it), estimate: `completion_tokens ≈ reply chars / 4`, `prompt_tokens ≈ request
  body chars / 4`, and set `tokens_estimated = true`.

### 1b. Per-response stats line (`src/gui.cpp`)
- `ChatLine` gains `std::string meta;` (empty = no sub-line).
- In `poll()`, after a successful reply, set
  `meta = model + " - " + fmt_time(elapsed) + " - " + fmt_rate(tok_s)`
  where `tok_s = completion_tokens / elapsed_s`; prefix `~` when `tokens_estimated`.
  Time format: `4.2s` under 60 s, `11m 30s` at/above 60 s.
- `chat_panel` renders `meta` as a dim gray (`nk_rgb(110,110,120)`) sub-line under
  the reply, slightly indented, using the same `draw_chat_line` machinery (plain text).

### 1c. Context size setting (`src/config.h/.cpp`, `config.example.json`)
- `LlmConfig`: `int context_size = 131072;` — load/save + example file.
- Settings modal: `nk_property_int(ctx, "Context tokens", 1024, &s.context_size,
  1048576, 1024, 1.0f)`; persisted on Save like the other LLM fields.

### 1d. Footer context usage (`src/gui.cpp` status line)
- `AppState` tracks `int last_prompt_tokens = 0;` (updated on every successful reply —
  the last request's prompt size *is* the current context occupancy).
- Status line appends: `  |  ctx: 5,231 / 131,072 (4%)` with thousands separators
  (small `fmt_thousands(int)` helper). Color: normal; **amber** (`nk_rgb(220,180,90)`)
  when usage > 85%; can exceed 100% (server truncates) — show as-is.

### Verification
- Real call against LM Studio (`http://192.168.0.3:8080/v1`): stats line + footer
  update after a reply. `KRASTOR_UITEST=1` additionally auto-sends one short test
  message at startup so a screenshot shows both stats without typing.

---

## Step 2 — Window size persistence

- `GuiConfig`: `int window_w = 900, window_h = 640;` (load/save + example).
- `run_gui`: `glfwCreateWindow(cfg.gui.window_w, cfg.gui.window_h, ...)`; clamp to
  minimum 480x360 on load and on restore.
- Render loop: each frame compare `glfwGetWindowSize` against `cfg.gui.*`; on change
  update cfg and arm a 500 ms debounced `cfg.save()` (same pattern as font zoom).
- Final `cfg.save()` on clean exit (after the loop) so a resize in the last 500 ms
  is not lost.
- Position is **not** persisted (out of scope).

---

## Step 3 — Mouse-wheel & keyboard scrolling (follow-bottom)

Today `chat_panel` force-pins the scroll to the newest line every frame via
`nk_group_set_scroll`, which is exactly what swallows the mouse wheel.

- `AppState`: `bool follow_bottom = true;`
- In `chat_panel`:
  - `follow_bottom == true` → keep the existing force-pin to bottom.
  - Wheel over the chat group (`ctx->input.scroll` non-zero while hovering) →
    `follow_bottom = false`; stop calling `nk_group_set_scroll` and let Nuklear's
    native group scrolling handle the wheel.
  - When the group's scroll offset reaches the computed `bottom` (within a few px,
    e.g. after scrolling down) → `follow_bottom = true` again.
  - New messages pin only while `follow_bottom` (the view stays where the user left
    it otherwise).
- Tune wheel feel: `ctx->style.window.scroll_step_size` / `scrolling_speed` so one
  wheel notch ≈ 3 text lines.
- Keyboard (when chat has focus, see Step 4): **PageUp/PageDown** = ±(visible height
  − 1 line), **Home/End** = top/bottom (End re-enables `follow_bottom`). Implemented
  by adjusting the group scroll offset (same math as the pin code).

---

## Step 4 — Text selection & copy in the chat

The chat is custom-drawn (`nk_draw_text` per styled run), so selection is implemented
manually on top of the layout pass. The input box (a real `nk_edit`) is audited, not
rewritten.

### 4a. Layout hit-map (rebuilt every frame in `chat_panel`)
For each **visual** line produced by the wrap logic, record:
`{ src_line_index, first_codepoint, last_codepoint, [(x0, x1, cp_start, cp_end) per run], y, height }`
into `AppState::chat_layout`. Widths already come from `text_w_px`; group scroll
offset is added to y for hit-testing.

### 4b. Selection model
- Virtual document = all `ChatLine.text` joined by `\n` (prompt prefixes `> `,
  `Krastor: `, `! ` excluded from copyable text).
- `sel_anchor`, `sel_caret` stored as **codepoint offsets** (not bytes) — correct for
  Italian accents and emoji. `sel_active` (non-empty when anchor != caret).
- Clicking the chat sets `chat_focused = true`; focusing the input box clears it.

### 4c. Mouse
- Press inside chat: focus + anchor at hit position; drag extends caret each frame.
- Plain click (no drag): clears selection.
- Double-click: select word (word chars = letters/digits/accents); triple-click:
  select the whole source line.

### 4d. Keyboard (chat focused; read via `glfwGetKey` edge-detection like the
font-zoom hotkeys, so the edit widget's key handling is untouched)
- **Shift+←/→** char, **Shift+↑/↓** visual line, **Shift+Home/End** line bounds.
- **Ctrl+A** select all, **Esc** clear, **Ctrl+C** copy.

### 4e. Rendering
- Selection highlight: per run intersected with the selection range, draw
  `nk_fill_rect` (accent blue, ~45% alpha) behind the text in the same pass.

### 4f. Copy
- Extract the selected substring from the virtual document, strip ANSI escapes
  (reuse the `ansi.h` parser to recover plain text), keep raw UTF-8 (emoji preserved),
  publish with `glfwSetClipboardString`.

### 4g. Input box audit (opencode-like behavior)
- Verify mouse-drag select, Shift+arrows selection, Ctrl+C/X/V all work in the
  `nk_edit` input; the custom `krastor_paste` hook must leave GLFW's clipboard
  copy plugin intact. Fix whatever the audit finds.

### 4h. Interplay & known limits
- Auto-pin pauses while a selection is active (Step 3 follow flag untouched).
- Selection survives scrolling/resize (document offsets; hit-map rebuilt per frame).
- Emoji may render as `?` in-chat (JetBrains Mono atlas range) — copy always
  carries the real characters.

---

## Config schema additions

```json
"llm": { ..., "context_size": 131072 },
"gui": { ..., "window_w": 900, "window_h": 640 }
```

## Files touched

| File | Changes |
|---|---|
| `src/llm_client.h/.cpp` | `LlmReply` stats fields, `usage` parsing + estimate |
| `src/config.h/.cpp`, `config.example.json` | `llm.context_size`, `gui.window_w/h` |
| `src/gui.cpp` | stats sub-line, footer ctx, Settings fields, window size save/restore, follow-bottom scroll, selection + copy, input audit |
| `src/development.md`, `development_log.md`, `project.md` | docs per commit (AGENT.md rule) |

## Commit order

1. `LLM response stats + context usage footer` (Step 1)
2. `Persist window size (debounced autosave + exit save)` (Step 2)
3. `Chat mouse-wheel & keyboard scrolling with follow-bottom` (Step 3)
4. `Chat text selection & copy (mouse + Shift+arrows)` (Step 4)

## Verification

- Both toolchains (`mingw-debug`, `msvc-2026-debug`) build clean per commit.
- `KRASTOR_UITEST=1`: seed a long multi-line chat, auto-send one LLM message
  (stats visible), and programmatically set a selection so the highlight can be
  screenshot-verified (synthetic mouse input does not reach the GLFW window from
  the agent session).
- Config-driven checks: window size restored from `bin/config.json`; context footer
  at seeded token counts incl. >85% amber state.
- Live user test: wheel scroll, drag-select, Shift+arrows, Ctrl+C paste into an
  editor (emoji/accents intact), resize persistence across restarts.
