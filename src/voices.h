// Krastor — TTS voice registry.
// Discovers local Piper voices under the models directory and tracks the voice
// currently selected at runtime. The voice can be changed at any time while the
// app is running (CLI: /voice <id>; later: GUI dropdown). The Phase 4 TTS engine
// reads VoiceRegistry::current() and (re)loads the matching model on change.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace krastor {

struct Voice {
    std::string id;          // short id, e.g. "it-it-paola"
    std::string folder;      // directory name inside models/, e.g. "tts-piper-it-it-paola"
    std::string language;    // "it", "en", ...
    std::string display_name;
};

class VoiceRegistry {
public:
    // Scans `models_dir` for folders named "tts-piper-*" and builds the voice list.
    // Returns false if no voice was found.
    bool discover(const std::string& models_dir);

    const std::vector<Voice>& voices() const { return voices_; }

    // Selects a voice by id or folder name. Returns false (and keeps the current
    // voice) when the id is unknown. Safe to call at any time during runtime.
    bool select(const std::string& id_or_folder);

    // Current voice, or nullptr when none discovered.
    const Voice* current() const;

private:
    std::vector<Voice> voices_;
    int current_index_ = -1;
};

} // namespace krastor
