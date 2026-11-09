// Krastor — inline media implementation (see media.h).
// Token parsing is pure string code (unit-tested by --media-test); the sound
// player wraps miniaudio (implementation compiled once in libs/miniaudio_impl.c).

#include "media.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>

#include <miniaudio.h>

#include "clipboard.h"
#include "permissions.h"  // workspace_dir, expand_home (relative-path fallback)
#include "session_store.h"  // base64_decode

namespace krastor {

namespace {

std::string trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Last path component, '/' and '\\' both handled (display label only).
std::string file_name(const std::string& src)
{
    const size_t p = src.find_last_of("/\\");
    return p == std::string::npos ? src : src.substr(p + 1);
}

std::vector<std::string> split_pipe(const std::string& s)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        const size_t p = s.find('|', start);
        if (p == std::string::npos) {
            out.push_back(trim(s.substr(start)));
            break;
        }
        out.push_back(trim(s.substr(start, p - start)));
        start = p + 1;
    }
    return out;
}

} // namespace

int parse_media_size(const std::string& raw)
{
    const std::string s = trim(raw);
    if (s.empty() || s == "-" || s == "*" || s == "auto")
        return 0;  // auto
    if (std::all_of(s.begin(), s.end(), [](char c) {
            return std::isdigit(static_cast<unsigned char>(c)) != 0;
        }))
        return std::atoi(s.c_str());
    return -1;  // invalid
}

bool parse_media_token(const std::string& line, MediaBlock& out)
{
    const std::string s = trim(line);
    auto finish = [&](MediaBlock::Kind kind, const std::string& inner) {
        const auto parts = split_pipe(inner);
        if (parts.empty() || parts[0].empty())
            return false;
        out = MediaBlock{};
        out.kind = kind;
        out.src = parts[0];
        if (kind == MediaBlock::Kind::Image) {
            if (parts.size() > 2) {
                out.req_w = parse_media_size(parts[1]);
                out.req_h = parse_media_size(parts[2]);
                if (out.req_w < 0 || out.req_h < 0)
                    return false;
            } else if (parts.size() == 2) {
                out.req_w = parse_media_size(parts[1]);
                if (out.req_w < 0)
                    return false;
            }
        }
        out.label = out.src.rfind("base64:", 0) == 0
                        ? "base64 image"
                        : file_name(out.src);
        return true;
    };
    if (s.rfind("[image:", 0) == 0 && s.size() > 8 && s.back() == ']')
        return finish(MediaBlock::Kind::Image, s.substr(7, s.size() - 8));
    if (s.rfind("[sound:", 0) == 0 && s.size() > 8 && s.back() == ']')
        return finish(MediaBlock::Kind::Sound, s.substr(7, s.size() - 8));
    return false;
}

std::vector<MediaSegment> split_media_tokens(const std::string& text)
{
    std::vector<MediaSegment> segs;
    std::string pending;
    size_t start = 0;
    auto flush = [&] {
        if (!pending.empty()) {
            segs.push_back({std::move(pending), nullptr});
            pending.clear();
        }
    };
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        const std::string line =
            text.substr(start, nl == std::string::npos ? std::string::npos
                                                      : nl - start);
        MediaBlock mb;
        if (parse_media_token(line, mb)) {
            flush();
            // Player control tokens: an action segment, not a block.
            if (mb.kind == MediaBlock::Kind::Sound && mb.src == "stop")
                segs.push_back({"", nullptr, MediaSegment::StopSound});
            else if (mb.kind == MediaBlock::Kind::Sound && mb.src == "pause")
                segs.push_back({"", nullptr, MediaSegment::PauseSound});
            else
                segs.push_back({"", std::make_shared<MediaBlock>(std::move(mb))});
        } else {
            if (!pending.empty())
                pending += '\n';
            pending += line;
        }
        if (nl == std::string::npos)
            break;
        start = nl + 1;
    }
    flush();
    return segs;
}

bool resolve_media_bytes(const std::string& src,
                         std::vector<unsigned char>& bytes, std::string& err)
{
    if (src.rfind("base64:", 0) == 0) {
        bytes = base64_decode(src.substr(7));
        if (bytes.empty()) {
            err = "empty base64 payload";
            return false;
        }
        return true;
    }
    if (src == "clipboard") {
        const PastedImage img = clipboard_get_image();
        if (!img.ok()) {
            err = "clipboard holds no image";
            return false;
        }
        bytes = img.png;
        return true;
    }
    std::filesystem::path path(src);
    std::ifstream f(path, std::ios::binary);
    if (!f && path.is_relative()) {
        // The app's CWD is its install folder, so a bare file name from the
        // model ("suonami ding.mp3") never resolves. Try the two places a
        // user would actually put the file: the workspace, then the home dir.
        for (const std::string& base : {workspace_dir(), expand_home("~")}) {
            std::filesystem::path cand = std::filesystem::path(base) / path;
            std::ifstream g(cand, std::ios::binary);
            if (g) {
                path = cand;
                f = std::move(g);
                break;
            }
        }
    }
    if (!f) {
        err = "file not found: " + src;
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>(f),
                 std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        err = "empty file: " + src;
        return false;
    }
    return true;
}

void media_fit(const MediaBlock& m, float box_w, float box_h, float& w, float& h)
{
    const float aspect =
        m.nat_w > 0 && m.nat_h > 0 ? (float)m.nat_w / (float)m.nat_h : 1.0f;
    // Requested size; the missing axis follows the native aspect ratio.
    w = m.req_w > 0 ? (float)m.req_w
                    : (m.req_h > 0 ? (float)m.req_h * aspect
                                   : (m.nat_w > 0 ? (float)m.nat_w : box_w));
    h = m.req_h > 0 ? (float)m.req_h
                    : (m.req_w > 0 ? (float)m.req_w / aspect
                                   : (m.nat_h > 0 ? (float)m.nat_h : box_h));
    if (w < 1.0f) w = 1.0f;
    if (h < 1.0f) h = 1.0f;
    // ALWAYS contain inside the text area, ratio preserved: no combination
    // of parameters can make the image overflow the chat panel.
    const float s = (std::min)(box_w / w, box_h / h);
    if (s < 1.0f) {
        w *= s;
        h *= s;
    }
}

// ---------------------------------------------------------------- sound

bool decode_audio_bytes(const std::vector<unsigned char>& bytes,
                        std::vector<float>& pcm, int& rate, int& channels,
                        std::string& err)
{
    // f32 output, stereo, file's native sample rate (0 = keep).
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 2, 0);
    ma_decoder dec;
    if (ma_decoder_init_memory(bytes.data(), bytes.size(), &cfg, &dec) !=
        MA_SUCCESS) {
        err = "unsupported audio format";
        return false;
    }
    rate = (int)dec.outputSampleRate;
    channels = 2;
    pcm.clear();
    std::vector<float> buf(4096 * 2);
    ma_uint64 got = 0;
    do {
        if (ma_decoder_read_pcm_frames(&dec, buf.data(), 4096, &got) !=
            MA_SUCCESS)
            break;
        pcm.insert(pcm.end(), buf.begin(), buf.begin() + got * 2);
    } while (got == 4096);
    ma_decoder_uninit(&dec);
    if (pcm.empty()) {
        err = "no audio decoded";
        return false;
    }
    return true;
}

std::vector<float> make_beep_pcm(int& rate)
{
    rate = 48000;
    const int n = rate / 3;  // ~0.33 s
    std::vector<float> pcm;
    pcm.reserve(n);
    for (int i = 0; i < n; ++i) {
        const double t = (double)i / rate;
        // 10 ms attack / release ramps so the tone has no click at the edges.
        const double env =
            (std::min)({1.0, t * 100.0, ((double)n / rate - t) * 100.0});
        pcm.push_back((float)(0.35 * env *
                              std::sin(2.0 * 3.14159265358979 * 440.0 * t)));
    }
    return pcm;
}

// ---------------------------------------------------------------- player

struct SoundPlayer::Impl {
    mutable std::mutex mtx;
    std::vector<float> pcm;      // interleaved f32
    ma_uint32 channels = 2;
    ma_uint32 rate = 48000;
    size_t frame_pos = 0;
    std::string src;
    std::atomic<bool> playing{false};
    std::atomic<bool> paused{false};
    float volume = 1.0f;         // protected by mtx (read in data_cb)
    ma_device dev{};         // opened lazily, reopened on format change
    bool dev_open = false;
    bool dev_stereo = true;
    ma_uint32 dev_rate = 0;

    ~Impl()
    {
        if (dev_open)
            ma_device_uninit(&dev);
    }

    static void data_cb(ma_device* dev, void* out, const void*, ma_uint32 frames)
    {
        auto* self = static_cast<Impl*>(dev->pUserData);
        std::lock_guard<std::mutex> lk(self->mtx);
        float* o = static_cast<float*>(out);
        // Paused: keep the device running but feed silence, cursor frozen.
        if (self->paused.load()) {
            std::memset(o, 0, frames * self->channels * sizeof(float));
            return;
        }
        const size_t total = self->pcm.size() / self->channels;
        size_t avail = self->frame_pos < total ? total - self->frame_pos : 0;
        const ma_uint32 n = frames < avail ? frames : (ma_uint32)avail;
        if (n)
            std::memcpy(o, self->pcm.data() + self->frame_pos * self->channels,
                        n * self->channels * sizeof(float));
        if (n < frames)
            std::memset(o + n * self->channels, 0,
                        (frames - n) * self->channels * sizeof(float));
        if (self->volume != 1.0f)
            for (ma_uint32 i = 0; i < n * self->channels; ++i)
                o[i] *= self->volume;
        self->frame_pos += n;
        if (self->frame_pos >= total)
            self->playing.store(false);
    }

    bool ensure_device(ma_uint32 rate, ma_uint32 channels)
    {
        if (dev_open && dev_rate == rate && dev_stereo == (channels == 2))
            return true;
        if (dev_open) {
            ma_device_uninit(&dev);
            dev_open = false;
        }
        ma_device_config dc = ma_device_config_init(ma_device_type_playback);
        dc.playback.format = ma_format_f32;
        dc.playback.channels = channels;
        dc.sampleRate = rate;
        dc.dataCallback = data_cb;
        dc.pUserData = this;
        if (ma_device_init(nullptr, &dc, &dev) != MA_SUCCESS ||
            ma_device_start(&dev) != MA_SUCCESS) {
            dev_open = false;
            return false;
        }
        dev_open = true;
        dev_rate = rate;
        dev_stereo = channels == 2;
        return true;
    }

    bool start(std::vector<float> pcm, ma_uint32 rate, ma_uint32 channels,
               const std::string& src)
    {
        if (!ensure_device(rate, channels))
            return false;
        std::lock_guard<std::mutex> lk(mtx);
        this->pcm = std::move(pcm);
        this->rate = rate;
        this->channels = channels;
        this->frame_pos = 0;
        this->src = src;
        paused.store(false);
        playing.store(true);
        return true;
    }
};

SoundPlayer::SoundPlayer() : impl_(std::make_unique<Impl>()) {}
SoundPlayer::~SoundPlayer() = default;

bool SoundPlayer::play(const std::string& src, std::string& err)
{
    std::vector<unsigned char> bytes;
    if (!resolve_media_bytes(src, bytes, err))
        return false;
    std::vector<float> pcm;
    int rate = 0, channels = 0;
    if (!decode_audio_bytes(bytes, pcm, rate, channels, err))
        return false;
    if (!impl_->start(std::move(pcm), (ma_uint32)rate, (ma_uint32)channels, src)) {
        err = "no audio output device";
        return false;
    }
    return true;
}

bool SoundPlayer::play_pcm(const std::vector<float>& pcm, int rate,
                           const std::string& src)
{
    return impl_->start(pcm, (ma_uint32)rate, 1, src);
}

void SoundPlayer::stop()
{
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->playing.store(false);
    impl_->paused.store(false);
    impl_->frame_pos = 0;
    impl_->src.clear();
}

void SoundPlayer::pause()
{
    std::lock_guard<std::mutex> lk(impl_->mtx);
    if (impl_->playing.load()) {
        impl_->playing.store(false);
        impl_->paused.store(true);
    }
}

void SoundPlayer::resume()
{
    std::lock_guard<std::mutex> lk(impl_->mtx);
    if (impl_->paused.load()) {
        impl_->paused.store(false);
        impl_->playing.store(true);
    }
}

void SoundPlayer::seek(double s)
{
    std::lock_guard<std::mutex> lk(impl_->mtx);
    if ((!impl_->playing.load() && !impl_->paused.load()) ||
        !impl_->rate || !impl_->channels)
        return;
    const double total = double(impl_->pcm.size() / impl_->channels);
    double pos = s * impl_->rate;
    if (pos < 0.0)
        pos = 0.0;
    if (pos > total)
        pos = total;
    impl_->frame_pos = (size_t)pos;
}

void SoundPlayer::set_volume(float v)
{
    if (v < 0.0f)
        v = 0.0f;
    if (v > 1.0f)
        v = 1.0f;
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->volume = v;
}

float SoundPlayer::volume() const
{
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->volume;
}

bool SoundPlayer::playing() const
{
    return impl_->playing.load();
}

bool SoundPlayer::playing_src(const std::string& src) const
{
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->playing.load() && impl_->src == src;
}

bool SoundPlayer::paused() const
{
    return impl_->paused.load();
}

bool SoundPlayer::paused_src(const std::string& src) const
{
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->paused.load() && impl_->src == src;
}

double SoundPlayer::position_s() const
{
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->rate ? (double)impl_->frame_pos / impl_->rate : 0.0;
}

double SoundPlayer::duration_s() const
{
    std::lock_guard<std::mutex> lk(impl_->mtx);
    if (!impl_->rate || !impl_->channels)
        return 0.0;
    return (double)(impl_->pcm.size() / impl_->channels) / impl_->rate;
}

} // namespace krastor
