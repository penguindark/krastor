# Sessions plan — persistence, switching, compaction

Status: **built** (2026-11-07; this file is the design of record, updated to
match the implementation).
Target: **Phase 7** in `project.md` roadmap.

Goals (user requirements):
1. Sessions live in a `sessions/` folder next to the executable.
2. **One folder per session**, so attachments can live inside the same folder.
3. A way to pick an old session or start a new one, in-app.
4. A `/compact` command that compresses the session with the **currently
   selected LLM**, sizing the summarization pass to the model's max context.
5. Sessions deletable from the command line **and** from the GUI.
6. opencode-style slash commands (`/sessions`, `/session ...`, `/compact`).

---

## 1. Folder layout

```
bin/krastor.exe
sessions/                     # exe_dir()/../sessions (sibling of bin/, same
                              # resolution rule as tools/); fallback ./sessions
                              # when the parent dir is not writable; sessions.dir
                              # in config.json overrides everything
  20261107-181530-a3f2/       # session id: YYYYMMDD-HHMMSS-<4 hex>, sorts chronologically
    meta.json                 # mutable metadata (rewritten on change, small)
    log.jsonl                 # append-only message log (one JSON object per line)
    attachments/              # binary payloads referenced by log.jsonl
      img-1.png
      img-2.png
    archive/                  # created by /compact: previous log kept here
      pre-compact-1.jsonl
  20261109-090112-77c0/
    ...
```

- `sessions/` is **git-ignored** (personal data). Auto-created on first save.
- Config overrides: `sessions.dir` (path, default `sessions/` next to exe),
  `sessions.autosave` (bool, default true).

### 1.1 `meta.json`

```jsonc
{
  "id": "20261107-181530-a3f2",
  "title": "fix selection bug",      // auto: first user msg, ~48 chars; renamable
  "created": "2026-11-07T17:15:30Z", // UTC ISO-8601
  "updated": "2026-11-07T18:02:11Z", // last appended message
  "model_ref": "halogen/Qwen3.8-...",// model in use at creation (informational)
  "messages": 24,                    // message count in log.jsonl
  "compactions": 1                   // how many times /compact ran
}
```

### 1.2 `log.jsonl` — one message per line

Same shape as the OpenAI message the `Conversation` holds, except images are
**file references** instead of inline base64 (keeps files small and diffable):

```jsonc
{"role":"user","content":"...","images":["attachments/img-1.png"]}
{"role":"assistant","content":"...","tool_calls":[{"id":"..","name":"..","arguments":".."}]}
{"role":"tool","tool_call_id":"..","content":".."}
{"role":"user","content":"[Conversation summary — compacted 2026-11-07 ...]\n<summary text>"}
```

- Append-only: a finished turn (assistant reply + any tool traffic) is appended
  in one burst; a half-written last line (crash mid-turn) is skipped on load.
- The summary after a compaction is a plain `user` message with a fixed marker
  prefix (`[Conversation summary`), so the wire format never changes and any
  model can consume it. `ChatLine` rendering shows it as a `sys` divider.
- Load = read all lines back into a `Conversation` (images re-inflated from
  files into base64 exactly as the GUI attaches them today).

## 2. Core module: `src/session_store.{h,cpp}`

```cpp
struct SessionInfo {           // one row of the list
    std::string id, title, created, updated, model_ref;
    int messages = 0;
    int compactions = 0;
};

class SessionStore {
public:
    static std::string default_dir();          // exe_dir()/sessions, fallback ./sessions
    explicit SessionStore(std::string dir);    // creates the dir lazily on create()

    std::string create(const std::string& title, const std::string& model_ref);
    bool load(const std::string& id, Conversation& conv, Meta& meta) const;
    void append(const std::string& id, const std::vector<Message>& new_msgs);
    // writes images to attachments/ and rewrites them as file refs first
    bool rename(const std::string& id, const std::string& title);
    bool remove(const std::string& id);        // fs::remove_all of the session folder
    std::vector<SessionInfo> list() const;     // updated-desc (newest first)
    bool exists(const std::string& id) const;
};
```

- `Conversation` gains one accessor to snapshot messages added since an index
  (for incremental append) and a `Conversation::replace_history(...)` used by
  load and compact. System prompt stays owned by config, not persisted.
- Serialization helpers live here (Message <-> jsonl line, base64 <-> file).
- Self-test flag `krastor --session-test`: create/append/load/rename/remove
  round-trip in a temp dir, image file-ref round-trip, corrupt-line skip,
  list ordering. PASS/FAIL + exit code, like `--tool-test`.

## 3. Compaction (`/compact [focus]`)

Module `src/compactor.{h,cpp}`, shared by CLI and GUI.

### 3.1 Context budget (tuned to the model)

```
ctx        = cfg.llm.context_size        // from llm_models.json limit.context
reserve_out= cfg.llm.max_tokens          // leave room for the summary itself
headroom   = max(512, ctx/16)            // request overhead, tools JSON, drift
budget     = ctx - reserve_out - headroom
```

Token estimate reuses the existing heuristic in `llm_client.cpp`
(~4 chars/token when the server doesn't report usage).

### 3.2 Algorithm

1. Split the history (excluding system prompt) into **chunks** whose estimated
   tokens fit `budget * 0.7` (never split a tool_call/tool-result pair apart).
2. If everything fits in one chunk: single summarization call.
   Otherwise **rolling map-reduce**: summarize chunk 1, then
   `summary_so_far + chunk 2` → new summary, and so on. Each call is a plain
   `LlmClient::complete()` with the *current* model/config — no tools attached.
3. Summarization prompt (fixed, English): preserve goals, decisions, file
   paths/commands run, open questions, and anything the user asked to remember;
   `focus` argument from the user is appended as extra instruction.
4. Optional tail: the last messages whose total fits `budget * 0.2` are kept
   **verbatim after** the summary (recency beats fidelity).
5. New history = system prompt + summary message + kept tail.

### 3.3 On-disk effect

- Old `log.jsonl` → `archive/pre-compact-<n>.jsonl` (nothing is ever lost).
- `log.jsonl` rewritten: summary message + kept tail.
- `meta.json`: `compactions++`, `messages` updated. Chat gets a `sys` line:
  `compacted 24 messages -> summary (~1.2k tok), archived`.
- The GUI context meter naturally drops on the next request.

## 4. Commands

Same parser used by the CLI loop and the GUI input box (a small shared
`handle_session_command()` in `session_store`/`main` helper; GUI treats a
leading `/` as a command instead of sending to the LLM).

| Command | Effect |
|---|---|
| `/sessions` | list sessions: `#`, id, updated, title, msg count |
| `/session list` | alias for `/sessions` (GUI: text listing in the chat, ids copyable) |
| `/session` | show current session (id, title, turns, compactions) |
| `/session new [title]` | start a fresh session (current one stays saved) |
| `/session open <id or #>` | load that session (id prefix accepted) |
| `/session delete <id or #>` | delete permanently (GUI asks confirm) |
| `/session rename <title>` | rename current session |
| `/compact [focus]` | compact current session (section 3) |
| `/clean` | clear the visible chat only — session and LLM history untouched (GUI: also the eraser floating button) |
| `/voice` | (GUI) toggle TTS output — same flag as the top-bar speaker button |
| `/mic` | (GUI) toggle microphone capture (hands-free pipeline) |
| `/title [title]` | alias for `/session rename` |

CLI-only flags (pre-loop): `--session <id>` start with a session,
`--new-session`, `--list-sessions`, `--delete-session <id>`, `--compact`.

`/reset` stays as-is (clears the live conversation) but now also **starts a new
session folder** so the cleared state is persisted, matching user intent.

## 5. Frontend integration

### 5.1 CLI (`main.cpp`)

- Startup: `--session`/`--new-session`/flags above; default = **continue the
  most recent session** if any (config `sessions.continue_last`, default true),
  else create a new one. Print the session id at startup.
- After every completed turn (incl. tool loop): `store.append(...)` the new
  messages; update `meta.json`.
- `/compact` runs the compactor with a console spinner; permission to run is
  implicit (user typed it).

### 5.2 GUI (`gui.cpp`)

- `AppState` gets `SessionStore store; std::string session_id;`.
  Autosave hook in `poll()` where the LLM reply is drained (the single point
  where a turn is complete on the main thread).

#### Session switching (first-class)

- **Quick switcher**: a combo in the top bar showing the current session
  title; opening it lists the 10 most recent sessions (title + date) plus a
  "+ new session" entry — one click switches. Implemented as a Nuklear combo
  inside the main window, so it cannot ROM the UI like a child window would.
  Hotkey **Ctrl+O** opens the full session modal; **Ctrl+Shift+O** cycles to
  the previous session (list order).
- **Session modal** (same pattern as the Settings modal, scaled like the
  permission modal): opened from the top-bar button, Ctrl+O or `/sessions`.
  Scrollable list, newest first, current one highlighted; clicking a row
  switches to that session immediately; per-row **Delete** (inline confirm:
  click again = yes); footer: **New session**, **Rename** current.
- **Switch rules**: refused while `worker != None` (same guard as the settings
  model switch); the current session is autosaved *before* switching so
  nothing is lost; the chat is rebuilt from the loaded session (see below);
  the title bar updates to `Krastor — <session title>`.
- `/session open <id or #>` and the CLI `--session <id>` perform the same
  switch path (shared `switch_to_session(id)` helper in AppState).
- Loading a session rebuilds `lines`: user→`you`, assistant→`krastor`
  (tool_calls rendered as collapsed `⚙` blocks like live ones), tool messages
  folded into their call block, summary message → `sys` divider. Attachments
  are already inlined as base64 in the rebuilt `Conversation`.
- `/compact` in the input box runs on a worker thread (spinner line in chat),
  then reloads the compacted history into `conv` and `lines`.

## 6. Implementation phases

| Phase | Deliverable |
|---|---|
| **A — Store** | `session_store.{h,cpp}` + `Conversation` accessors + config keys + `--session-test` self-test |
| **B — CLI** | auto-save, continue-last, `--session/--new-session/--list-sessions/--delete-session/--compact` flags, `/sessions /session /compact` commands |
| **C — Compactor** | `compactor.{h,cpp}` budget + rolling summary + archive; wired to `/compact` in CLI |
| **D — GUI** | autosave, session switching (quick-switcher popup, Ctrl+O modal, Ctrl+Shift+O cycle, shared `switch_to_session()`), session modal (list/open/new/delete/rename), title bar, slash-command parsing in the input box, GUI `/compact` worker |
| **E — Polish** | docs sync (project.md Phase 7, development.md, COLD_START.md, AGENT.md, development_log.md), dual-toolchain clean, manual test pass |

## 7. Decisions & non-goals (first increment)

- **Folder-per-session** (user request) over a single JSONL-per-file store:
  attachments and compaction archives sit next to the messages.
- Append-only JSONL over rewriting one `messages.json`: crash-safe, cheap,
  trivially inspectable.
- Sessions are **local-only**, no sync, no cross-instance locking (two running
  Krastors editing the same session = last writer wins; acceptable).
- Compaction never deletes the archive; a session folder can always be
  replayed from `archive/` manually.
- Attachments today are only pasted/dropped **images**; the file-ref format
  already supports arbitrary files if non-image attachments come later.
- No automatic compaction yet (only explicit `/compact`); a "compact when
  context > 85%" auto-prompt is a follow-up candidate.
