// Krastor — local STT engine (Phase 4).
// NVIDIA Parakeet TDT 0.6B v3 (multilingual EN+IT, auto-detected, punctuated)
// driven by Silero VAD speech segments through the sherpa-onnx C API; the
// microphone is captured with miniaudio. Mirrors the validated Python reference
// in tools/parrot.py (VAD segments -> offline recognizer).
//
// Windows uses the prebuilt sherpa-onnx shared C API (see libs/development.md);
// on other platforms the engine reports itself unavailable for now.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace krastor {

struct Config;

class Stt {
public:
    explicit Stt(const Config& cfg);
    ~Stt();

    Stt(const Stt&) = delete;
    Stt& operator=(const Stt&) = delete;

    // Loads Parakeet + Silero VAD on first call (takes a few seconds).
    // Returns false when unsupported or models are missing.
    bool ensure_loaded();

    // Enumerates capture devices, one per line: "0  Microphone (Realtek Audio)  [default]".
    // The index is what `device_index` refers to in listen()/monitor().
    static std::string list_input_devices();

    // Records from the selected/default mic for `seconds` while printing a live
    // level meter, and returns the peak level in [0, 1]. A peak near 0 means the
    // device receives no signal (wrong device, muted, or Windows privacy block).
    double monitor(double seconds = 5.0, int device_index = -1);

    // Records from the default microphone and returns the transcription of the
    // first complete utterance (ends ~0.5 s after speech stops). Returns an
    // empty string when nothing was heard within `max_seconds`.
    // device_index < 0 selects the system default microphone.
    std::string listen(double max_seconds = 20.0, int device_index = -1);

    // Transcribes a WAV/MP3/FLAC file (decoded + resampled to 16 kHz mono).
    // Used for testing without a microphone and for future file attachments.
    std::string transcribe_file(const std::string& path);

    // Transcribes raw 16 kHz mono PCM (`n` samples). Used by the hands-free
    // pipeline to decode VAD segments outside this engine. Must not run
    // concurrently with listen() or another recognize() call.
    std::string recognize(const float* samples, size_t n);

    // Continuous hands-free capture: opens the mic and runs the VAD on an
    // internal feeder thread; every closed speech segment (16 kHz mono) is
    // moved into `on_utterance`, which runs on that feeder thread. One stream
    // at a time; do not use listen() while streaming. Returns false when the
    // models fail to load or the mic cannot be opened.
    bool start_stream(int device_index,
                      std::function<void(std::vector<float>)> on_utterance);

    // Stops the feeder thread and closes the mic (no-op when not streaming).
    void stop_stream();

    bool streaming() const;

    // Copies the most recent `n` mic samples (16 kHz mono) captured while the
    // mic was open into `dst`, newest last. Returns the number copied (0 when
    // the mic never ran). Feeds the shader spectrum analyzer; safe to call
    // from the render thread while any mic operation is active.
    size_t latest(float* dst, size_t n) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace krastor
