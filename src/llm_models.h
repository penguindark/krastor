// Krastor — LLM model registry loaded from llm_models.json.
// The file uses the opencode.jsonc "provider" schema:
//   { "provider": { "<id>": {
//         "name": "...", "npm": "@ai-sdk/openai-compatible",
//         "options"|"settings": { "baseURL": "...", "apiKey": "..." },
//         "models": { "<model-id>": { "name": "...",
//                       "limit": { "context": N, "output": N } } } } } }
// Comments are allowed (nlohmann parses // and /* */ by default). Each model
// becomes an entry referenced as "<provider-id>/<model-id>"; the Settings
// modal only picks one of these refs, everything else comes from this file.

#pragma once

#include <string>
#include <vector>

namespace krastor {

struct LlmConfig; // config.h — apply_model_ref() fills it from an entry

struct LlmModelEntry {
    std::string ref;      // "provider_id/model_id" (stored in config.json)
    std::string display;  // model "name" from the file (or the model id)
    std::string provider; // provider "name" from the file (or the provider id)
    std::string base_url;
    std::string api_key;
    int context = 0;      // limit.context (0 = not specified)
    int output = 0;       // limit.output (0 = not specified)
};

class LlmModelRegistry {
public:
    // exe_dir()/llm_models.json when present, else ./llm_models.json.
    static std::string default_path();

    // Parses `path`. Returns false (and keeps the registry empty) when the
    // file is missing or malformed; error() explains why.
    bool load(const std::string& path);

    const std::vector<LlmModelEntry>& entries() const { return entries_; }
    const LlmModelEntry* find(const std::string& ref) const;
    const std::string& error() const { return error_; }

private:
    std::vector<LlmModelEntry> entries_;
    std::string error_;
};

// When llm.model_ref names a registry entry, overwrite base_url/api_key/model
// (and context_size when the entry declares limit.context). Returns false and
// changes nothing when the ref is empty or unknown (explicit config fields
// then keep working as before).
bool apply_model_ref(LlmConfig& llm, const LlmModelRegistry& reg);

} // namespace krastor
