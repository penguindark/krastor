// Krastor — system clipboard image access.
// Reads a bitmap from the OS clipboard, converts it to RGBA and encodes it as
// PNG so it can be attached to a chat message (vision models). On Windows the
// CF_DIB clipboard format is used; other platforms return an empty image.

#pragma once

#include <string>
#include <vector>

namespace krastor {

struct PastedImage {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> png;  // PNG-encoded bytes
    bool ok() const { return !png.empty(); }
};

// Returns the clipboard bitmap as PNG, or an empty PastedImage when the
// clipboard holds no image (or the platform is unsupported).
PastedImage clipboard_get_image();

// Loads an image file (png/jpg/bmp/gif/...) and re-encodes it as PNG so it can
// be attached to a chat message. Used by drag & drop. Empty on failure.
PastedImage load_image_file(const std::string& path);

// Standard base64 (no line breaks) for data: URLs.
std::string base64_encode(const unsigned char* data, size_t len);

// Decode image bytes (any stb-readable format) and place them on the
// clipboard as CF_DIB so any app can paste them as a picture (the reverse of
// clipboard_get_image). Used by click-to-copy on chat images.
bool copy_image_to_clipboard(const std::vector<unsigned char>& image_bytes,
                             std::string& err);

} // namespace krastor
