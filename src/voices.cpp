#include "voices.h"

#include <algorithm>
#include <cctype>

namespace krastor {

namespace {

// "tts-piper-it-it-paola" -> "it-it-paola"
std::string voice_id_from_folder(const std::string& folder)
{
    const std::string prefix = "tts-piper-";
    if (folder.rfind(prefix, 0) == 0)
        return folder.substr(prefix.size());
    return folder;
}

// "it-it-paola" -> "it"
std::string language_from_id(const std::string& id)
{
    const auto pos = id.find('-');
    return pos == std::string::npos ? id : id.substr(0, pos);
}

std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

bool VoiceRegistry::discover(const std::string& models_dir)
{
    voices_.clear();
    current_index_ = -1;

    std::error_code ec;
    std::filesystem::path root(models_dir);
    if (!std::filesystem::is_directory(root, ec))
        return false;

    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        const std::string folder = entry.path().filename().string();
        if (!entry.is_directory() || folder.rfind("tts-piper-", 0) != 0)
            continue;
        Voice voice;
        voice.folder = folder;
        voice.id = voice_id_from_folder(folder);
        voice.language = language_from_id(voice.id);
        voice.display_name = voice.id;
        voices_.push_back(std::move(voice));
    }

    std::sort(voices_.begin(), voices_.end(),
              [](const Voice& a, const Voice& b) { return a.id < b.id; });
    if (!voices_.empty())
        current_index_ = 0;
    return !voices_.empty();
}

bool VoiceRegistry::select(const std::string& id_or_folder)
{
    const auto needle = to_lower(id_or_folder);
    for (size_t i = 0; i < voices_.size(); ++i) {
        if (to_lower(voices_[i].id) == needle || to_lower(voices_[i].folder) == needle) {
            current_index_ = static_cast<int>(i);
            return true;
        }
    }
    return false;
}

const Voice* VoiceRegistry::current() const
{
    if (current_index_ < 0 || current_index_ >= static_cast<int>(voices_.size()))
        return nullptr;
    return &voices_[current_index_];
}

} // namespace krastor
