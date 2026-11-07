#include "session_store.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include <nlohmann/json.hpp>

#include "clipboard.h"  // base64_encode
#include "config.h"     // Config::exe_dir()

namespace fs = std::filesystem;
using nlohmann::json;

namespace krastor {

namespace {

// ---- time helpers (UTC, no std::filesystem::clock: missing in MinGW) ----

std::string iso_now()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string stamp_now()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    return buf;
}

std::string random_hex4()
{
    static std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(0, 0xffff);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%04x", dist(rng));
    return buf;
}

// ---- base64 decode: declared in session_store.h (encode lives in
// clipboard.cpp); defined below, outside this anonymous namespace. ----

// ---- Message <-> jsonl line ----

json message_to_json(const Message& m, const std::vector<std::string>& image_refs)
{
    json j;
    j["role"] = m.role;
    if (!m.content.empty())
        j["content"] = m.content;
    if (!image_refs.empty())
        j["images"] = image_refs;
    if (!m.tool_calls.empty()) {
        json calls = json::array();
        for (const auto& tc : m.tool_calls)
            calls.push_back({{"id", tc.id}, {"name", tc.name},
                             {"arguments", tc.arguments}});
        j["tool_calls"] = std::move(calls);
    }
    if (!m.tool_call_id.empty())
        j["tool_call_id"] = m.tool_call_id;
    return j;
}

bool message_from_json(const json& j, Message& out, const fs::path& session_dir)
{
    if (!j.is_object() || !j.contains("role") || !j["role"].is_string())
        return false;
    out = Message{};
    out.role = j["role"].get<std::string>();
    if (j.contains("content") && j["content"].is_string())
        out.content = j["content"].get<std::string>();
    if (j.contains("tool_call_id") && j["tool_call_id"].is_string())
        out.tool_call_id = j["tool_call_id"].get<std::string>();
    if (j.contains("tool_calls") && j["tool_calls"].is_array()) {
        for (const auto& c : j["tool_calls"]) {
            if (!c.is_object())
                continue;
            ToolCall tc;
            tc.id = c.value("id", "");
            tc.name = c.value("name", "");
            tc.arguments = c.value("arguments", "");
            out.tool_calls.push_back(std::move(tc));
        }
    }
    // Image file references -> re-inflate to base64 exactly as the GUI
    // attaches them. A missing file drops that image (log stays loadable).
    if (j.contains("images") && j["images"].is_array()) {
        for (const auto& ref : j["images"]) {
            if (!ref.is_string())
                continue;
            std::ifstream f(session_dir / ref.get<std::string>(), std::ios::binary);
            if (!f)
                continue;
            std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)),
                                             std::istreambuf_iterator<char>());
            out.images.push_back(base64_encode(bytes.data(), bytes.size()));
        }
    }
    return true;
}

// Writes the base64 images of `msg` into <sdir>/attachments/ starting after
// sequence `seq` (updated); returns the log references ("attachments/img-N.png").
std::vector<std::string> store_images(const fs::path& sdir, const Message& msg,
                                      int& seq)
{
    std::vector<std::string> refs;
    for (const auto& b64 : msg.images) {
        const auto bytes = base64_decode(b64);
        if (bytes.empty())
            continue;
        std::string ref;
        do {
            ref = "img-" + std::to_string(++seq) + ".png";
        } while (fs::exists(sdir / "attachments" / ref));
        std::ofstream img(sdir / "attachments" / ref,
                          std::ios::binary | std::ios::trunc);
        if (!img)
            continue;
        img.write(reinterpret_cast<const char*>(bytes.data()),
                  std::streamsize(bytes.size()));
        refs.push_back("attachments/" + ref);
    }
    return refs;
}

// Highest img-<n> sequence currently in a session's attachments/ folder.
int next_attach_seq(const fs::path& sdir)
{
    int seq = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(sdir / "attachments", ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("img-", 0) == 0) {
            const int n = std::atoi(name.c_str() + 4);
            if (n > seq)
                seq = n;
        }
    }
    return seq;
}

json meta_to_json(const SessionMeta& m)
{
    return {
        {"id", m.id},        {"title", m.title},
        {"created", m.created}, {"updated", m.updated},
        {"model_ref", m.model_ref}, {"messages", m.messages},
        {"compactions", m.compactions},
    };
}

bool meta_from_json(const json& j, SessionMeta& out)
{
    if (!j.is_object())
        return false;
    out.id = j.value("id", "");
    out.title = j.value("title", "");
    out.created = j.value("created", "");
    out.updated = j.value("updated", "");
    out.model_ref = j.value("model_ref", "");
    out.messages = j.value("messages", 0);
    out.compactions = j.value("compactions", 0);
    return !out.id.empty();
}

} // namespace

std::vector<unsigned char> base64_decode(const std::string& in)
{
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<unsigned char> out;
    int acc = 0, bits = 0;
    for (const char c : in) {
        const int v = val(c);
        if (v < 0)
            continue;  // skip padding/whitespace
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((acc >> bits) & 0xff));
        }
    }
    return out;
}

bool is_summary_message(const Message& m)
{
    return m.role == "user" && m.content.rfind(kSummaryMarker, 0) == 0;
}

std::string SessionStore::default_dir(const std::string& configured)
{
    if (!configured.empty())
        return configured;
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path exe = fs::path(Config::exe_dir()).parent_path() / "sessions";
    // Portable install: sessions/ may sit next to the exe itself.
    if (fs::exists(exe, ec))
        return exe.string();
    const fs::path beside = fs::path(Config::exe_dir()) / "sessions";
    if (fs::exists(beside, ec))
        return beside.string();
    // Neither exists yet: create lazily under the exe dir; if that is not
    // writable (Program Files), fall back to the working directory.
    fs::create_directories(exe, ec);
    return ec ? std::string("./sessions") : exe.string();
}

SessionStore::SessionStore(std::string dir) : dir_(std::move(dir)) {}

std::string SessionStore::session_path(const std::string& id) const
{
    return (fs::path(dir_) / id).string();
}

bool SessionStore::exists(const std::string& id) const
{
    if (id.empty() || id == "." || id == ".." ||
        id.find_first_of("/\\") != std::string::npos)
        return false;
    std::error_code ec;
    return fs::is_directory(session_path(id), ec);
}

bool SessionStore::write_meta(const SessionMeta& m) const
{
    std::ofstream f(session_path(m.id) + "/meta.json", std::ios::trunc);
    if (!f)
        return false;
    f << meta_to_json(m).dump(2) << '\n';
    return bool(f);
}

bool SessionStore::read_meta(const std::string& id, SessionMeta& out) const
{
    std::ifstream f(session_path(id) + "/meta.json");
    if (!f)
        return false;
    json j = json::parse(f, nullptr, false);
    if (j.is_discarded() || !meta_from_json(j, out))
        return false;
    return true;
}

std::string SessionStore::create(const std::string& title,
                                 const std::string& model_ref)
{
    std::error_code ec;
    fs::create_directories(dir_, ec);
    if (ec)
        return {};
    std::string id;
    for (int attempt = 0; attempt < 5 && id.empty(); ++attempt) {
        const std::string cand = stamp_now() + "-" + random_hex4();
        if (!fs::exists(fs::path(dir_) / cand, ec))
            id = cand;
    }
    if (id.empty())
        return {};
    fs::create_directories(fs::path(dir_) / id / "attachments", ec);
    if (ec)
        return {};
    SessionMeta m;
    m.id = id;
    m.title = title.empty() ? "New session" : title;
    m.created = m.updated = iso_now();
    m.model_ref = model_ref;
    if (!write_meta(m))
        return {};
    std::ofstream log(session_path(id) + "/log.jsonl", std::ios::app);
    return id;
}

bool SessionStore::append(const std::string& id, const std::vector<Message>& msgs)
{
    if (!exists(id))
        return false;
    SessionMeta m;
    if (!read_meta(id, m))
        return false;

    const fs::path sdir = fs::path(dir_) / id;
    int attach_seq = next_attach_seq(sdir);

    std::ofstream log(session_path(id) + "/log.jsonl", std::ios::app);
    if (!log)
        return false;
    for (const auto& msg : msgs) {
        if (msg.role == "system")
            continue;
        // Persist images as files; the log line keeps only the references.
        const auto refs = store_images(sdir, msg, attach_seq);
        log << message_to_json(msg, refs).dump() << '\n';
        ++m.messages;
    }
    m.updated = iso_now();
    log.flush();
    return bool(log) && write_meta(m);
}

bool SessionStore::load(const std::string& id, Conversation& conv,
                        SessionMeta* meta) const
{
    if (!exists(id))
        return false;
    SessionMeta m;
    read_meta(id, m);
    std::ifstream log(session_path(id) + "/log.jsonl");
    if (!log)
        return false;
    const fs::path sdir = fs::path(dir_) / id;
    std::vector<Message> history;
    std::string line;
    while (std::getline(log, line)) {
        if (line.empty())
            continue;
        json j = json::parse(line, nullptr, false);
        if (j.is_discarded())
            continue;  // half-written line after a crash: skip
        Message msg;
        if (message_from_json(j, msg, sdir))
            history.push_back(std::move(msg));
    }
    conv.replace_history(std::move(history));
    if (meta)
        *meta = m;
    return true;
}

bool SessionStore::meta_of(const std::string& id, SessionMeta& out) const
{
    return exists(id) && read_meta(id, out);
}

bool SessionStore::rename(const std::string& id, const std::string& title)
{
    SessionMeta m;
    if (!read_meta(id, m))
        return false;
    m.title = title;
    m.updated = iso_now();
    return write_meta(m);
}

bool SessionStore::remove(const std::string& id)
{
    if (!exists(id))
        return false;
    std::error_code ec;
    fs::remove_all(fs::path(dir_) / id, ec);
    return !ec;
}

std::vector<SessionMeta> SessionStore::list() const
{
    std::vector<SessionMeta> out;
    std::error_code ec;
    if (!fs::is_directory(dir_, ec))
        return out;
    for (const auto& e : fs::directory_iterator(dir_, ec)) {
        if (!e.is_directory(ec))
            continue;
        SessionMeta m;
        if (read_meta(e.path().filename().string(), m)) {
            out.push_back(std::move(m));
        } else {  // folder without (valid) meta.json: still list it
            m.id = e.path().filename().string();
            m.title = "(untitled)";
            out.push_back(std::move(m));
        }
    }
    std::sort(out.begin(), out.end(), [](const SessionMeta& a, const SessionMeta& b) {
        if (a.updated != b.updated)
            return a.updated > b.updated;
        return a.id > b.id;  // same-second tie: ids carry the creation stamp
    });
    return out;
}

bool SessionStore::replace_log_after_compact(
    const std::string& id, const std::vector<Message>& msgs)
{
    if (!exists(id))
        return false;
    SessionMeta m;
    if (!read_meta(id, m))
        return false;

    std::error_code ec;
    fs::create_directories(fs::path(dir_) / id / "archive", ec);
    const std::string log_path = session_path(id) + "/log.jsonl";
    // Archive the old log: pre-compact-<n>.jsonl, n = compactions + 1.
    const std::string archive =
        session_path(id) + "/archive/pre-compact-" +
        std::to_string(m.compactions + 1) + ".jsonl";
    fs::copy_file(log_path, archive,
                  fs::copy_options::overwrite_existing, ec);
    if (ec)
        return false;

    {   // Rewrite the log with the compacted history. Tail messages may still
        // carry inline base64 images (inflated at load): re-store them as new
        // attachment files so the rewritten log references real files.
        const fs::path sdir = fs::path(dir_) / id;
        int attach_seq = next_attach_seq(sdir);
        std::ofstream log(log_path, std::ios::trunc);
        if (!log)
            return false;
        for (const auto& msg : msgs) {
            if (msg.role == "system")
                continue;
            const auto refs = store_images(sdir, msg, attach_seq);
            log << message_to_json(msg, refs).dump() << '\n';
        }
        log.flush();
        if (!log)
            return false;
    }
    m.compactions += 1;
    m.messages = 0;
    for (const auto& msg : msgs)
        if (msg.role != "system")
            ++m.messages;
    m.updated = iso_now();
    return write_meta(m);
}

std::string SessionStore::title_from_text(const std::string& text)
{
    std::string out;
    for (const char c : text) {
        if (c == '\n' || c == '\r')
            break;
        out += c;
        if (out.size() >= 48)
            break;
    }
    // trim trailing spaces
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out.empty() ? "New session" : out;
}

} // namespace krastor
