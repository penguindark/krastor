// Krastor — persistent chat sessions (see sessions_plan.md).
// One folder per session inside sessions/ (next to the executable):
//   <id>/meta.json       mutable metadata (title, dates, counters)
//   <id>/log.jsonl       append-only message log, one JSON object per line
//   <id>/attachments/    images referenced by log.jsonl (img-<n>.png)
//   <id>/archive/        log.jsonl copies made by /compact (nothing is lost)
// The system prompt is NOT persisted: it always comes from the config (plus
// the session's agent), so a session restored after a prompt change uses the
// new prompt.

#pragma once

#include <string>
#include <vector>

#include "conversation.h"

namespace krastor {

struct SessionMeta {
    std::string id;
    std::string title;
    std::string created;    // UTC ISO-8601 ("YYYY-MM-DDTHH:MM:SSZ")
    std::string updated;
    std::string model_ref;  // model in use at creation (informational)
    std::string agent;      // active agent id (restored when the session opens)
    int messages = 0;       // message count in log.jsonl
    int compactions = 0;    // how many times /compact ran on this session
};

// Prefix of the summary message a compaction writes into the history; used
// both to build and to recognize summaries (rendered as a chat divider).
inline constexpr const char* kSummaryMarker = "[Conversation summary";
bool is_summary_message(const Message& m);

// Standard base64 decode (padding/whitespace skipped); encode lives in
// clipboard.cpp. Shared with the media system ("base64:" sources).
std::vector<unsigned char> base64_decode(const std::string& in);

class SessionStore {
public:
    // sessions.dir from config; "" -> exe_dir()/sessions (fallback ./sessions
    // when the exe dir is not writable, same resolution rule as tools/).
    static std::string default_dir(const std::string& configured = {});

    explicit SessionStore(std::string dir);

    const std::string& dir() const { return dir_; }

    bool exists(const std::string& id) const;

    // Creates <dir>/<id>/ with meta.json + empty log.jsonl; returns the id
    // ("" on failure). id = YYYYMMDD-HHMMSS-xxxx (chronologically sortable).
    std::string create(const std::string& title, const std::string& model_ref,
                       const std::string& agent = {});

    // Appends messages (none of them the system message) to the log. Base64
    // images are written to attachments/ and replaced by file references.
    bool append(const std::string& id, const std::vector<Message>& msgs);

    // Absolute paths the next `count` images will be written to by append()
    // (img-<n>.png under <dir>/<id>/attachments/, same numbering append()
    // uses). Lets the GUI tell the model where its attachments live before
    // persistence runs. Empty when the session does not exist.
    std::vector<std::string> attachment_paths(const std::string& id,
                                              int count) const;

    // Reads the log back into `conv` (its system prompt is kept). Malformed
    // lines are skipped (crash tolerance: a half-written last line is lost,
    // the rest of the session is not).
    bool load(const std::string& id, Conversation& conv,
              SessionMeta* meta = nullptr) const;

    bool meta_of(const std::string& id, SessionMeta& out) const;
    bool rename(const std::string& id, const std::string& title);
    // Records the agent active in this session (restored on open).
    bool set_agent(const std::string& id, const std::string& agent);
    bool remove(const std::string& id);
    std::vector<SessionMeta> list() const;  // newest `updated` first

    // Compaction support: archives the current log.jsonl to
    // archive/pre-compact-<n>.jsonl, rewrites the log with `msgs` (the
    // summary + kept tail) and bumps meta.compactions.
    bool replace_log_after_compact(const std::string& id,
                                   const std::vector<Message>& msgs);

    // Single-line title from a message: first ~48 chars of the text.
    static std::string title_from_text(const std::string& text);

private:
    std::string dir_;

    std::string session_path(const std::string& id) const;
    bool write_meta(const SessionMeta& m) const;
    bool read_meta(const std::string& id, SessionMeta& out) const;
};

} // namespace krastor
