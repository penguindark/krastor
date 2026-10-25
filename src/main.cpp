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
#include "gui.h"
#include "llm_client.h"
#include "stt.h"
#include "tts.h"
#include "voices.h"

namespace {

void print_help()
{
    std::cout <<
        "\nCommands:\n"
        "  /help              show this help\n"
        "  /voices            list available TTS voices\n"
        "  /voice <id>        switch TTS voice at runtime (e.g. /voice en-us-lessac)\n"
        "  /say <text>        speak the text with the current TTS voice\n"
        "  /listen [sec]      record the microphone and transcribe one utterance\n"
        "                     (Parakeet EN+IT auto-detect; default 20 s max)\n"
        "  /transcribe <file> transcribe an audio file (wav/mp3/flac)\n"
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

    // Args: [--gui] [config.json]
    std::string config_path = "config.json";
    bool use_gui = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--gui") use_gui = true;
        else config_path = a;
    }
    std::cout << "Krastor v0.4.0 (Phase 4 voice I/O)\n";

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

    if (use_gui)
        return krastor::run_gui(cfg, voices);

    print_voice(voices);

    krastor::Tts tts(voices, cfg);
    tts.init();
    krastor::Stt stt(cfg);

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

        if (line.rfind("/listen", 0) == 0) {
            double max_s = 20.0;
            std::string arg = line.size() > 7 ? line.substr(7) : "";
            arg.erase(0, arg.find_first_not_of(" \t"));
            if (!arg.empty()) {
                max_s = std::atof(arg.c_str());
                if (max_s <= 0.0) max_s = 20.0;
            }
            std::cout << "[stt] listening... (speak now, up to " << max_s << "s)\n";
            const std::string heard = stt.listen(max_s);
            if (heard.empty()) { std::cout << "[stt] nothing heard\n"; continue; }
            std::cout << "[stt] " << heard << "\n\n";
            line = heard;  // fall through: the transcription goes to the LLM
        } else if (line.rfind("/transcribe", 0) == 0) {
            std::string arg = line.size() > 11 ? line.substr(11) : "";
            arg.erase(0, arg.find_first_not_of(" \t"));
            if (arg.empty()) { std::cout << "usage: /transcribe <audio file>\n"; continue; }
            const std::string text = stt.transcribe_file(arg);
            std::cout << (text.empty() ? "(no speech recognized)" : text) << "\n";
            continue;
        }

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
        if (line.rfind("/say", 0) == 0) {
            std::string arg = line.size() > 4 ? line.substr(4) : "";
            arg.erase(0, arg.find_first_not_of(" \t"));
            if (arg.empty()) { std::cout << "usage: /say <text>\n"; continue; }
            tts.speak(arg);
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
