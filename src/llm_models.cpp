#include "llm_models.h"

#include "config.h"

#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace krastor {

std::string LlmModelRegistry::default_path()
{
    const std::string exe = Config::exe_dir() + "/llm_models.json";
    std::ifstream probe(exe);
    if (probe)
        return exe;
    return "llm_models.json";  // working directory fallback
}

bool LlmModelRegistry::load(const std::string& path)
{
    entries_.clear();
    error_.clear();

    std::ifstream file(path);
    if (!file) {
        error_ = "llm_models: cannot open " + path;
        return false;
    }

    nlohmann::json root;
    try {
        // ignore_comments = true: the file is opencode.jsonc-style (// allowed).
        root = nlohmann::json::parse(file, nullptr, true, true);
    } catch (const nlohmann::json::exception& e) {
        error_ = "llm_models: invalid JSON in " + path + ": " + e.what();
        return false;
    }

    if (!root.is_object() || !root.contains("provider") ||
        !root["provider"].is_object()) {
        error_ = "llm_models: " + path + " has no \"provider\" object";
        return false;
    }

    for (const auto& [pid, prov] : root["provider"].items()) {
        if (!prov.is_object())
            continue;
        LlmModelEntry base;
        base.provider = prov.value("name", pid);
        // opencode uses "options"; some configs spell it "settings" — accept both.
        const nlohmann::json* opts = nullptr;
        if (prov.contains("options") && prov["options"].is_object())
            opts = &prov["options"];
        else if (prov.contains("settings") && prov["settings"].is_object())
            opts = &prov["settings"];
        if (opts) {
            base.base_url = opts->value("baseURL", std::string());
            base.api_key = opts->value("apiKey", std::string());
        }
        if (!prov.contains("models") || !prov["models"].is_object())
            continue;
        for (const auto& [mid, m] : prov["models"].items()) {
            LlmModelEntry e = base;
            e.ref = pid + "/" + mid;
            e.display = m.is_object() ? m.value("name", mid) : mid;
            if (m.is_object() && m.contains("limit") && m["limit"].is_object()) {
                e.context = m["limit"].value("context", 0);
                e.output = m["limit"].value("output", 0);
            }
            if (m.is_object() && m.contains("capabilities") &&
                m["capabilities"].is_object())
                e.tools = m["capabilities"].value("tools", false);
            if (e.base_url.empty())
                continue;  // unusable without an endpoint
            entries_.push_back(std::move(e));
        }
    }

    if (entries_.empty())
        error_ = "llm_models: no usable models in " + path;
    return !entries_.empty();
}

const LlmModelEntry* LlmModelRegistry::find(const std::string& ref) const
{
    for (const auto& e : entries_)
        if (e.ref == ref)
            return &e;
    return nullptr;
}

bool apply_model_ref(LlmConfig& llm, const LlmModelRegistry& reg)
{
    if (llm.model_ref.empty())
        return false;
    const LlmModelEntry* e = reg.find(llm.model_ref);
    if (!e)
        return false;
    llm.base_url = e->base_url;
    llm.api_key = e->api_key;
    // The wire model id is the part after "provider/".
    llm.model = llm.model_ref.substr(llm.model_ref.find('/') + 1);
    if (e->context > 0)
        llm.context_size = e->context;
    llm.supports_tools = e->tools;
    return true;
}

} // namespace krastor
