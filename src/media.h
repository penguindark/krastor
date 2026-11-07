// Krastor — inline media in the chat (images + mini sound player).
// A ChatLine can carry a MediaBlock: an image or a sound shown as a block in
// the chat flow. Sources are a file path, the literal "clipboard" (images
// only) or "base64:<data>" (self-contained, survives session restore because
// the token itself lives in the message text).
//
// The LLM triggers blocks with a dedicated line in its reply:
//   [image: <src> | <width> | <height>]   (sizes optional, "-" = auto)
//   [sound: <src>]
// and the user with the /image and /play slash commands (see gui.cpp).
//
// SoundPlayer decodes audio files (WAV/MP3/FLAC/OGG via miniaudio's decoder)
// or raw PCM buffers and plays them on a dedicated output device, separate
// from the TTS device so the two never fight over the sink.

#pragma once

#include <memory>
#include <string>
#include <vector>

namespace krastor {

struct MediaBlock {
    enum class Kind { Image, Sound };
    Kind kind = Kind::Image;
    std::string src;       // path | "clipboard" | "base64:..."
    int req_w = 0;         // requested size in px; 0 = auto
    int req_h = 0;
    // Runtime state (main/render thread only):
    bool loaded = false;   // image: texture uploaded
    bool failed = false;   // source could not be resolved/decoded
    std::string error;     // reason when failed
    unsigned int tex = 0;  // GL texture id (images, set lazily on first draw)
    int nat_w = 0, nat_h = 0;  // native pixel size (images)
    std::string label;     // short display name (file name)
    // Encoded bytes kept only while the texture is not uploaded yet, and
    // permanently for volatile sources ("clipboard"); files/base64 are
    // re-resolvable from src after texture eviction. (gui.cpp owns this.)
    std::vector<unsigned char> bytes;
};
using MediaRef = std::shared_ptr<MediaBlock>;

// Appended to the system prompt (GUI + CLI): tells the model the preferred
// way to show images/sounds is the chat tokens above.
inline constexpr const char* kMediaUsagePrompt =
    "\nYou can show media in the chat: put a token on a line of its own - "
    "[image: <file path | base64:...> | <width> | <height>] shows an image, "
    "[sound: <file path | base64:...>] shows a playable sound. Sizes are "
    "optional pixels ('-' or empty = automatic); the media is always scaled "
    "to fit the chat area. To show an image to the user ALWAYS prefer the "
    "[image: ...] token over merely describing or naming the file, unless "
    "the user asked for something else. Images the user attaches to the chat "
    "are saved to disk and their file paths are listed in the user message "
    "([attached files saved at: ...]) - look there before saying an "
    "attachment is unavailable.";

// Parse one size field: "" | "-" | "*" | "auto" -> 0 (auto); digits -> value;
// anything else -> -1 (invalid, caller reports usage).
int parse_media_size(const std::string& s);

// Recognize a full-line media token (leading/trailing spaces allowed):
//   [image: src | w | h]   [sound: src]
// On success fills `out` (kind/src/req_w/req_h/label) and returns true.
bool parse_media_token(const std::string& line, MediaBlock& out);

// One piece of a reply: either plain text or a media block.
struct MediaSegment {
    std::string text;   // set when media == nullptr
    MediaRef media;
};

// Split reply text into ordered segments: every line that is exactly a media
// token becomes its own segment and is removed from the text (so markdown
// formatting and TTS never see it). Empty text segments are dropped.
std::vector<MediaSegment> split_media_tokens(const std::string& text);

// Resolve a source to raw bytes: "base64:..." decoded, "clipboard" grabbed,
// anything else read as a file. Returns false + error on failure.
bool resolve_media_bytes(const std::string& src, std::vector<unsigned char>& bytes,
                         std::string& err);

// Final layout pass for an image block: start from the requested size (or the
// native size when auto, scaling the missing axis by the native aspect), then
// ALWAYS contain the result inside (box_w, box_h) preserving the aspect ratio
// - an image can never overflow the chat text area, whatever the parameters.
void media_fit(const MediaBlock& m, float box_w, float box_h, float& w, float& h);

// ---------------------------------------------------------------- sound
// Decode an audio file's bytes to interleaved float PCM (rate/channels from
// the file, channels forced to 2). Shared by the player and --media-test.
bool decode_audio_bytes(const std::vector<unsigned char>& bytes,
                        std::vector<float>& pcm, int& rate, int& channels,
                        std::string& err);

// Generate a short test tone (mono float PCM) - the /play beep buffer source.
std::vector<float> make_beep_pcm(int& rate);

// One-shot mini player: plays a decoded file or PCM buffer; a new play()
// replaces the current one. Thread-safe (the miniaudio callback thread reads
// the state under a mutex, like the TTS device does).
class SoundPlayer {
public:
    SoundPlayer();
    ~SoundPlayer();
    SoundPlayer(const SoundPlayer&) = delete;
    SoundPlayer& operator=(const SoundPlayer&) = delete;

    // Resolve `src` (file path or "base64:..."), decode, play. On failure
    // returns false and fills `err`; nothing starts playing.
    bool play(const std::string& src, std::string& err);
    // Play an in-memory mono buffer (the beep test).
    bool play_pcm(const std::vector<float>& pcm, int rate, const std::string& src);
    void stop();

    bool playing() const;
    bool playing_src(const std::string& src) const;  // this exact block?
    double position_s() const;
    double duration_s() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace krastor
