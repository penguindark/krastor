// Krastor — LLM agent with voice input/output and real-time graphics interface.
// Phase 3: core agent CLI — config, OpenAI-compatible LLM chat loop and runtime
// TTS voice selection. STT/TTS audio and the GUI arrive in Phases 4/5.
// See project.md (master description) and src/development.md (this folder).

#include <cstdlib>
#include <iostream>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "config.h"
#include "conversation.h"
#include "llm_client.h"
#include "voices.h"

namespace {

void print_help()
{
    std::cout <<
        "\nCommands:\n"
        "  /help              show this help\n"
        "  /voices            list available TTS voices\n"
        "  /voice <id>        switch TTS voice at runtime (e.g. /voice en-us-lessac)\n"
        "  /reset             clear the conversation (keeps system prompt)\n"
        "  /config            show the active LLM configuration\n"
        "  /exit              quit\n"
        "Anything else is sent to the LLM.\n\n";
}

void print_voice(const krastor::VoiceRegistry& voices)
{
    const auto* v = voices.current();
    std::cout << (v ? "current TTS voice: " + v->id + " [" + v->language + "]"
                    : "no TTS voice discovered")
              << '\n';
}

void list_voices(const krastor::VoiceRegistry& voices)
{
    if (voices.voices().empty()) {
        std::cout << "no voices found (expected models/tts-piper-* folders)\n";
        return;
    }
    std::cout << "TTS voices:\n";
    for (const auto& v : voices.voices())
        std::cout << (v.id == (voices.current() ? voices.current()->id : "") ? " * " : "   ")
                  << v.id << "  (" << v.language << ")\n";
}

} // namespace

int main(int argc, char** argv)
{
#ifdef _WIN32
    // Correct rendering of accented Italian text in the console.
    SetConsoleOutputCP(CP_UTF8);
#endif

    const std::string config_path = (argc > 1) ? argv[1] : "config.json";
    std::cout << "Krastor v0.3.0 (Phase 3 core agent)\n";

    krastor::Config cfg = [&] {
        try {
            return krastor::Config::load(config_path);
        } catch (const std::exception& e) {
            std::cerr << "config error: " << e.what() << '\n';
            ::exit(1);
        }
    }();

    krastor::VoiceRegistry voices;
    if (voices.discover(cfg.tts.models_dir)) {
        if (!cfg.tts.default_voice.empty() && !voices.select(cfg.tts.default_voice))
            std::cerr << "warning: default_voice '" << cfg.tts.default_voice
                      << "' not found, using first discovered voice\n";
    }
    print_voice(voices);

    if (cfg.llm.api_key.empty())
        std::cout << "note: no API key configured (config.json llm.api_key or "
                     "KRASTOR_API_KEY) — LLM calls will fail\n";

    krastor::LlmClient llm(cfg.llm);
    krastor::Conversation conversation(cfg.llm.system_prompt);
    print_help();

    std::string line;
    while (true) {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line))
            break;  // EOF (Ctrl+Z/Ctrl+D)
        if (line.empty())
            continue;

        if (line == "/exit" || line == "/quit")
            break;
        if (line == "/help") { print_help(); continue; }
        if (line == "/voices") { list_voices(voices); continue; }
        if (line == "/reset") { conversation.reset(); std::cout << "(conversation cleared)\n"; continue; }
        if (line == "/config") {
            std::cout << "base_url: " << cfg.llm.base_url << "\n"
                      << "model:    " << cfg.llm.model << "\n"
                      << "api_key:  " << (cfg.llm.api_key.empty() ? "(not set)" : "(set)") << "\n";
            print_voice(voices);
            continue;
        }
        if (line.rfind("/voice", 0) == 0) {
            std::string arg = line.size() > 6 ? line.substr(6) : "";
            // trim leading spaces
            arg.erase(0, arg.find_first_not_of(" \t"));
            if (arg.empty()) { print_voice(voices); continue; }
            if (voices.select(arg))
                std::cout << "TTS voice switched -> " << voices.current()->id << '\n';
            else
                std::cout << "unknown voice '" << arg << "' (see /voices)\n";
            continue;
        }
        if (line[0] == '/') {
            std::cout << "unknown command (try /help)\n";
            continue;
        }

        conversation.add_user(line);
        std::cout << "...thinking..." << std::flush;
        auto reply = llm.complete(conversation);
        std::cout << "\r" << std::string(40, ' ') << "\r";  // clear the placeholder
        if (reply.ok) {
            conversation.add_assistant(reply.content);
            std::cout << reply.content << "\n\n";
        } else {
            // The user turn stays in history; the model can still see the question.
            std::cerr << "LLM error: " << reply.error << "\n\n";
        }
    }
    std::cout << "bye.\n";
    return 0;
}
