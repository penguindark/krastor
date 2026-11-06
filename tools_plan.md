# Krastor — External Tools System (tools_plan.md)

Goal: give the agent a set of external executable tools it can call through the
OpenAI function-calling protocol, to interact with the outside world (disk, web,
Python). Every tool is a plain `.exe` plus a JSONC manifest describing what it
does, its input schema and its permission policy. Krastor supervises every call
(path validation, ask-permission dialogs, timeouts, output caps).

Decisions already locked with the user:
- Write sandbox default: `~/Documents/KrastorWorkspace/` (created on first run);
  writes elsewhere always ask. Reads are free except a deny list.
- Tool executables are C++20, built by the same CMake project (dual-toolchain
  MinGW + MSVC rule applies).
- No delete/rmdir action in the first increment. **Update (2026-11-07):**
  `delete` was added later, gated by a mandatory user confirmation
  (`ask_actions` in the manifest: always asks, never silenced by grants).
- The Python tool accepts both inline `code` and `script` + `args`.
- Runtime tools folder is the repo-level `tools/` (sibling of `bin/`), NOT
  inside `bin/`. Manifests live at its first level and are named
  `tool_<name>.jsonc`.

---

## 1. Folder layout

```
tools/                          # repo root, sibling of bin/
  tool_files.jsonc              # manifest: file/folder manager
  tool_webfetch.jsonc           # manifest: wget-like web text fetcher
  tool_python.jsonc             # manifest: agent Python interpreter
  filetool.exe                  # CMake post-build copies built exes here
  webfetch.exe
  python/
    venv/                       # full virtualenv for the agent (git-ignored)
      Scripts/python.exe
    requirements.txt            # agent runtime packages (requests, bs4, ...)
    setup.ps1                   # creates venv + installs requirements
  filetool/                     # C++ sources of the files tool
    CMakeLists.txt (or listed in root CMakeLists)
    *.cpp/.h
  webfetch/                     # C++ sources of the webfetch tool
  development.md                # updated: tools system + how to add a tool
  <existing python dev scripts unchanged>
```

Resolution rule in Krastor: tools dir = `exe_dir()/../tools` (dev layout),
fallback `exe_dir()/tools` (portable install). Krastor scans
`<tools>/tool_*.jsonc` at startup; the manifest field `"exe"` is relative to
the tools dir (e.g. `"filetool.exe"`, `"python/venv/Scripts/python.exe"`).

---

## 2. Manifest schema (`tool_<name>.jsonc`)

Parsed with nlohmann/json (comments allowed). Fields:

| Field | Meaning |
|---|---|
| `name` | unique id; becomes the function name exposed to the LLM |
| `description` | what the tool does — shown to the LLM (write it well!) |
| `exe` | executable path relative to the tools dir |
| `enabled` | master switch per tool (default true) |
| `timeout_ms` | hard kill timeout for one invocation |
| `path_args` | names of input args that are filesystem paths (validated by the supervisor) |
| `input_schema` | JSON Schema of the JSON object passed on stdin; forwarded verbatim in the `tools` array of the chat request |
| `permissions` | default policy (overridable per tool in config.json) |

Policy block shape:

```jsonc
"permissions": {
  "read":  { "allow": ["**"],
             "deny":  ["**/.git/**", "C:/Windows/**", "C:/Program Files/**"] },
  "write": { "allow": ["~/Documents/KrastorWorkspace/**"], "ask": true },
  "net":   { "allow": ["**"], "ask": false },          // webfetch
  "run":   { "ask": true }                              // python tool
}
```

Precedence: `deny` > `allow` > `ask` (if not allowed and ask=true → dialog; if
not allowed and ask=false → denied). `~` expands to the user home. Globs are
case-insensitive on Windows. `deny_always` entries in the manifest cannot be
overridden by config.json (safety floor).

Example `tool_files.jsonc` (full):

```jsonc
{
  "name": "files",
  "description": "Manage files and folders on disk: list, stat, read (byte ranges), write, edit (search & replace), append, rename, mkdir.",
  "exe": "filetool.exe",
  "enabled": true,
  "timeout_ms": 15000,
  "path_args": ["path", "from", "to"],
  "input_schema": {
    "type": "object",
    "properties": {
      "action":  { "enum": ["list","stat","read","write","edit","append","rename","mkdir"] },
      "path":    { "type": "string" },
      "recursive": { "type": "boolean" },
      "start":   { "type": "integer", "description": "read: first byte (seek)" },
      "end":     { "type": "integer", "description": "read: last byte excluded" },
      "content": { "type": "string", "description": "write/append: file content" },
      "find":    { "type": "string", "description": "edit: text to find" },
      "replace": { "type": "string", "description": "edit: replacement" },
      "to":      { "type": "string", "description": "rename: new path" }
    },
    "required": ["action", "path"]
  },
  "permissions": {
    "read":  { "allow": ["**"], "deny": ["**/.git/**", "C:/Windows/**"] },
    "write": { "allow": ["~/Documents/KrastorWorkspace/**"], "ask": true }
  }
}
```

---

## 3. Invocation protocol (supervisor ⇄ tool)

- Krastor spawns `exe` (CreateProcess on Windows, fork/exec on Linux), no
  shell, args passed as a single JSON object on **stdin**:
  `{"action":"read","path":"...","start":0,"end":4096,"_policy":{...}}`
  (`_policy` = the effective policy, so the tool self-enforces too —
  defense in depth).
- Tool writes one JSON object on **stdout**: `{"ok":true,"result":...}` or
  `{"ok":false,"error":"..."}`. stderr is captured for the log/GUI block.
- Hard timeout → kill process tree → `{"ok":false,"error":"timeout"}`.
- Output cap ~200 KB: truncated with a `"[truncated]"` marker.
- Exit code != 0 with unparsable stdout → error with last stderr lines.

---

## 4. Permission enforcement (two layers)

1. **Supervisor (Krastor, before spawn)**: `ToolRunner` classifies the call
   (`read` if action is read-only, `write` for mutating fs actions, `net`,
   `run`), resolves every `path_args` entry to an absolute canonical path
   (no symlink surprises: `std::filesystem::weakly_canonical`), matches the
   policy. Outcomes: ALLOW / DENY (error returned to the LLM as tool result) /
   ASK. ASK → GUI modal "Allow once / Always for this session / Deny" showing
   tool name, action and resolved paths (CLI: y/n/a prompt). "Always" grants
   are kept in memory for the session only.
2. **Tool itself**: re-checks `_policy` before touching the disk/net. A tool
   run manually from a terminal behaves the same way.

Config overrides in `config.json` (section `tools`):
`tools.enabled` (global), `tools.<name>.enabled`, `tools.<name>.permissions.*`
(merge over the manifest, except `deny_always`). Settings modal gets a
"Enable external tools" checkbox + status line listing loaded tools.

---

## 5. Agent-side integration (Krastor core)

- `llm_models.json`: add `"capabilities": { "tools": true, ... }` to models
  that support function calling (halogen — verified live: it returns
  `finish_reason: tool_calls`; crucible already declares it). Tools are only
  sent to models declaring the capability.
- `src/tool_registry.h/.cpp`: loads `tool_*.jsonc`, validates, exposes
  `entries()` + OpenAI `tools` JSON array builder.
- `src/tool_runner.h/.cpp`: process spawn + stdio JSON + timeout + caps +
  permission gate (calls into `permissions.h`).
- `src/permissions.h/.cpp`: glob matcher, policy merge, ask-callback interface
  (GUI modal / CLI prompt implement it).
- `llm_client`: request gains `tools` array; `LlmReply` gains
  `tool_calls: vector<{id, name, arguments_json}>`.
- `conversation`: new message role `"tool"` carrying `tool_call_id` + result
  text; assistant messages with tool_calls are appended verbatim.
- **Agentic loop** (GUI worker + CLI REPL): `complete()` → while
  `tool_calls` and iterations < 8: run each call through ToolRunner, append
  tool results, call again. Stop button cancels between calls.
- **GUI rendering**: each tool call is a collapsible chat block (like the
  Thought block): `⚙ files · read notes.txt → 4.2 KB`, expanded shows the
  JSON args + output; spinner while running; denied/failed calls marked.
- System prompt addition (config default): tell the agent which tools exist
  and the sandbox rules, keep it short.

---

## 6. Built-in tools

### 6.1 `filetool.exe` (tools/filetool/)
Actions: `list` (entries, `recursive` flag, size+mtime per entry), `stat`,
`read` (byte range `start`/`end`, returns text + total size + returned range;
binary sniff → refuse with a hint), `write` (create/overwrite, parent dirs
created inside the sandbox), `edit` (find/replace, first occurrence, reports
replacements count), `append`, `rename` (also moves), `mkdir`, `delete`
(permanent; added later with a mandatory confirmation - manifest
`ask_actions: ["delete"]`, directories need `recursive:true` unless empty).
UTF-8 paths. Self-enforces `_policy`.

### 6.2 `webfetch.exe` (tools/webfetch/)
Args: `url` (http/https only), `max_bytes`. GET via the shared http client
(WinHTTP/curl compiled into the tool target). HTML → readable text: drop
`script`/`style`/`noscript`, strip tags, decode common entities, collapse
whitespace (logic shared with `tts_text.cpp` via a small static lib
`krastor_text` built from both krastor.exe and webfetch.exe). Non-HTML
content-types pass through as text when plausible. Output capped.

### 6.3 Python tool (`tool_python.jsonc` → `python/venv/Scripts/python.exe`)
- `tools/python/setup.ps1`: `python -m venv tools/python/venv` +
  `pip install -r requirements.txt` (requests, beautifulsoup4 to start).
  `venv/` is git-ignored. If the venv is missing the tool reports
  `disabled: run tools/python/setup.ps1` at registry load.
- Input: `code` (inline, run as `python -c`) OR `script` + `args`. cwd is
  always the workspace sandbox. Timeout 60 s.
- Policy: `run.ask = true` by default — every execution asks, and the dialog
  shows the code about to run. Users may relax it in config.json.

---

## 7. Implementation phases

| Phase | Deliverable |
|---|---|
| **A — Core** | `tool_registry`, `tool_runner`, `permissions` (+ glob matcher + ask interface), `llm_client` tool_calls, `conversation` tool role, agentic loop (GUI worker + CLI), GUI collapsible tool blocks + permission modal, `tools.*` config section, `capabilities.tools` in llm_models.json |
| **B — filetool** | `tools/filetool/` sources + `tool_files.jsonc` + CMake targets copying exe+manifest into `tools/`, sandbox auto-create on first run |
| **C — webfetch** | `tools/webfetch/` + `tool_webfetch.jsonc` + shared `krastor_text` static lib (HTML→text) |
| **D — python + polish** | `tools/python/` (setup.ps1, requirements.txt, manifest), Settings UI (global toggle + per-tool status), `krastor --tool-test` headless self-test, docs |

Definition of done per phase: builds clean on **mingw-debug and
msvc-2026-debug**, existing self-tests still PASS, docs updated
(`tools/development.md` new section, `src/development.md`, `project.md` §4.4 +
roadmap, `development_log.md`, `COLD_START.md` setup step for the python venv).

## 8. Headless self-test (`krastor --tool-test`)

- Registry: loads the three shipped manifests, rejects malformed ones with a
  clear error; tool list order deterministic.
- Permission matcher table-driven: deny > allow > ask, `~` expansion,
  case-insensitive Windows globs, `deny_always` cannot be overridden.
- filetool round-trip in a temp dir (policy pointed there): write → read full
  → read byte range → edit find/replace → append → rename → mkdir → list;
  verify JSON envelopes and error paths (missing file, binary sniff, path
  outside policy denied).
- webfetch smoke: fetch a local `file://`? No — keep it honest: fetch
  `https://example.com` when the network is up, assert the text contains
  "Example Domain"; skip gracefully offline.
- python tool smoke (only when venv exists): `print(6*7)` → "42".

## 9. Security notes / non-goals (first increment)

- No delete, no shell interpolation, no env passthrough to tools (minimal env),
  tools run with the user's privileges — the policy layer is guardrails, not a
  hard sandbox. Documented as such.
- "Always allow" grants are session-scoped, never persisted.
- Manifest `deny_always` floor is not overridable via config.json.
