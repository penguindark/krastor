// Krastor — local TTS engine (Phase 4).
// Synthesizes speech with Piper voices through the sherpa-onnx C API and plays
// it on the default output device via miniaudio. The model follows
// VoiceRegistry::current(): switching a voice at runtime (CLI /voice, GUI combo)
// transparently reloads the engine on the next utterance.
//
// Windows uses the prebuilt sherpa-onnx shared C API (see libs/development.md);
// on other platforms the engine reports itself unavailable for now.

#pragma once

#include <memory>
#include <string>

namespace krastor {

class VoiceRegistry;
struct Config;

class Tts {
public:
    Tts(VoiceRegistry& voices, const Config& cfg);
    ~Tts();

    Tts(const Tts&) = delete;
    Tts& operator=(const Tts&) = delete;

    // Opens the audio output device. Returns false when synthesis is not
    // supported on this platform. Without a device speak() still synthesizes
    // (silently) so the pipeline stays testable over SSH/headless.
    bool init();

    // Synthesizes `text` with the current voice and blocks until playback
    // finished. Prints a one-line status. Returns false on synthesis failure.
    bool speak(const std::string& text);

    // Requests the currently playing utterance to stop as soon as possible
    // (barge-in when the user speaks over the TTS). Safe from any thread,
    // no-op when nothing is playing; the pending synthesis is skipped too.
    void interrupt();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace krastor
