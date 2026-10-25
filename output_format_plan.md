# Krastor — Output Formatting Plugins + Color Emoji

**Goal:** transform the raw LLM reply into colored, formatted output in the chat
panel, like opencode's TUI: Markdown rendering, fenced code blocks with
language syntax highlighting (python, cpp, ...), collapsible "Thought" blocks
for model reasoning, and full-color emoji.

**Core idea:** the chat already renders ANSI/SGR-styled runs (`src/ansi.h` ->
per-token `nk_draw_text`, codepoint hit-map, ANSI-stripped copy). So every
formatter plugin is a pure `string -> string` transform that emits ANSI
escapes. All existing passes (wrap, selection, copy, stats sub-line, TTS)
keep working untouched: only the *displayed* text is styled; conversation
history and TTS always use the raw reply.

Verified enablers:
- `libs/nuklear/nuklear_glfw_gl3.h:295` binds the texture **per draw command**
  and `nuklear.h` splits batches on texture change -> a second GL texture
  (emoji atlas) can be drawn with `nk_draw_image` without patching the backend.
- `AnsiRun` supports bold, dim (SGR 2), truecolor fg/bg -> enough vocabulary
  for every style below.
- Parallel builds: `CMAKE_BUILD_PARALLEL_LEVEL=16` set in all build presets.

---

## Architecture — formatter chain

`src/formatters.{h,cpp}`: `OutputFormatter` applies the enabled plugins to a
raw reply before it is stored in `ChatLine::text`.

```
raw LLM text -> ThinkSplitter (Step 3) -> MarkdownFormatter (Step 1)
                                        -> CodeHighlighter (Step 2, called
                                           by the markdown pass on fences)
             -> ANSI-styled string
```

- Hook points: `gui.cpp` `poll()` (assistant line only) and the CLI reply
  print in `main.cpp`. `conv.add_assistant()` / `speak_async()` keep raw text.
- Kill-switch: `gui.format_output` (config, default true). Input already
  containing ESC bytes (pre-styled tool output) passes through unchanged.

## Step 1 — Markdown formatter (`src/md_format.{h,cpp}`)

Block scanner (line-based) + inline scanner, emitting SGR codes:

| Markdown | Rendering |
|---|---|
| `# .. ####` | bold; h1 bright blue, h2+ cyan |
| `**bold**` | SGR 1 |
| `*italic*` | bold + soft purple (`_` emphasis not handled: snake_case safety) |
| `` `code` `` | khaki fg on dark bg |
| `- item` / `1. item` | orange bullet/number (like the opencode screenshot), indent kept |
| `> quote` | SGR 2 dim + `│` bar |
| `---` | dim `────` rule |
| `[text](url)` | blue text + dim ` (url)` |
| tables | column-aligned, dim `│` separators, bold header, separator row dropped |
| ``` ```lang ``` | dim language label line + dark-bg block (highlighting in Step 2) |

## Step 2 — Syntax highlighting (`src/highlight.{h,cpp}` + language packs)

Plugin philosophy of `data/shaders/*.frag`: languages are **data files**,
`data/languages/*.json`:

```json
{ "name": "Python", "line_comment": "#", "block_comment": ["\"\"\"", "\"\"\""],
  "strings": ["\"", "'"], "keywords": ["def", "class", "..."],
  "types": ["int", "..."], "builtins": ["print", "..."], "decorators": true }
```

- Shipped packs: python, cpp, c, json, jsonc, bash, ps1, javascript, cmake, ini.
- Per-line single-pass scanner: strings -> comments -> numbers -> identifiers
  -> punctuation; stateful across lines only for block comments / triple quotes.
- Colors: keyword magenta, string green, comment dim gray, number orange,
  type cyan, decorator yellow. Uses fg-only resets (`39`) so the code-block
  background survives.
- Unknown language / missing pack = plain block.

## Step 3 — Thinking blocks (opencode-style `Thought · 5.4s`)

- `llm_client.cpp`: parse `choices[0].message.reasoning_content` (LM Studio /
  DeepSeek-style extension) into `LlmReply::reasoning`; fallback: extract
  inline `<think>`/`<thought>` tags from `content`.
- `ChatLine` gains `collapsible`/`collapsed` flags; the renderer records a
  click rect for the header line (hit pass already tracks per-line rects).
- Header: `+ Thought · 5.4s` orange (collapsed `+` / expanded `-`), click
  toggles; body = reasoning in dim (SGR 2), indented. Collapsed by default.
  Duration = worker-measured elapsed (same clock as the stats sub-line).
- Collapsed bodies are excluded from the document/hit-map, so selection and
  copy never see hidden text.

## Step 4 — Color emoji

1. `tools/gen_emoji_atlas.py` (Pillow, `pip install pillow` once): renders
   **Noto Color Emoji.ttf** (CBDT color bitmaps) ->
   `data/fonts/emoji_atlas.png` (RGBA grid, 64 px cells, ~240 common emoji)
   + `data/fonts/emoji_atlas.json` manifest `{ "U+1F600": [col,row,w,h] }`.
   The .ttf is git-ignored; the generated atlas is committed.
2. `src/emoji.{h,cpp}`: loads the PNG via **stb_image** (new `libs/stb/`),
   uploads one `GL_RGBA` texture, manifest -> `unordered_map<char32_t, Rect>`.
3. Chat draw pass: split each token into text/emoji segments (UTF-8 decode +
   map lookup); text -> `nk_draw_text` as today, emoji -> `nk_draw_image`
   quads with atlas UVs, square advance = font height, baseline-aligned.
4. Emoji = 1 codepoint in the hit-map (each emoji is its own token, so the
   monospace selection math stays exact); Ctrl+C already copies raw UTF-8.
   ZWJ/flags: strip U+FE0F, common ZWJ map to a representative glyph,
   unknown -> `?` (documented limit).
5. Bonus: bake Latin-1 + box-drawing `nk_rune` ranges for JetBrains Mono so
   accented text stops rendering as `?`.

## Step 5 — Docs & verification

- Per commit: `development_log.md`, `src/development.md`, `project.md` (AGENT.md rule).
- `KRASTOR_UITEST=1` seeds a reply with headings, a python block, lists,
  inline code, a thought block and emoji -> screenshot-verified via shot.ps1.
- Both toolchains (`mingw-debug`, `msvc-2026-debug`) build clean per commit.

## Commit order

1. `Formatter chain + Markdown formatter (headings, inline, lists, quotes)`
2. `Syntax highlighting with data-driven language packs`
3. `Reasoning parsing + collapsible Thought blocks`
4. `Color emoji atlas + Unicode font ranges`

## Known limits / open items

- Emoji subset ~240 (1024^2 atlas); full set would need a 4096^2 atlas.
- No italic/underline glyphs in a mono font -> approximated by color/bold.
- Thought blocks only appear if the server/model emits reasoning.
