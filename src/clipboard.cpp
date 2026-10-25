// Krastor — clipboard image capture implementation.

#include "clipboard.h"

#include <cstdint>
#include <cstring>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace krastor {

// ------------------------------------------------------------------ base64
std::string base64_encode(const unsigned char* data, size_t len)
{
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= len) {
        const uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += tbl[v & 63];
        i += 3;
    }
    if (i < len) {
        const uint32_t v = (data[i] << 16) |
                           ((i + 1 < len) ? (data[i + 1] << 8) : 0);
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += (i + 1 < len) ? tbl[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

// ------------------------------------------------------------------ PNG encode
static std::vector<unsigned char> encode_png(const unsigned char* rgba,
                                             int w, int h)
{
    std::vector<unsigned char> png;
    auto writer = [](void* ctx, void* data, int size) {
        auto* out = static_cast<std::vector<unsigned char>*>(ctx);
        const auto* p = static_cast<const unsigned char*>(data);
        out->insert(out->end(), p, p + size);
    };
    if (stbi_write_png_to_func(writer, &png, w, h, 4, rgba, w * 4))
        return png;
    return {};
}

// ------------------------------------------------------------------ file -> PNG
PastedImage load_image_file(const std::string& path)
{
    PastedImage img;
    int w = 0, h = 0, ch = 0;
    unsigned char* rgb = stbi_load(path.c_str(), &w, &h, &ch, 4);  // force RGBA
    if (!rgb)
        return img;
    img.width = w;
    img.height = h;
    img.png = encode_png(rgb, w, h);
    stbi_image_free(rgb);
    return img;
}

// ------------------------------------------------------------------ clipboard
#ifdef _WIN32

// Converts a CF_DIB memory block to RGBA. Returns empty on unsupported formats.
static PastedImage dib_to_image(const void* bits, SIZE_T size)
{
    PastedImage img;
    if (size < sizeof(BITMAPINFOHEADER))
        return img;
    BITMAPINFOHEADER hdr;
    std::memcpy(&hdr, bits, sizeof(hdr));
    if (hdr.biSizeImage == 0 && hdr.biWidth <= 0)
        return img;

    const int bpp = hdr.biBitCount;
    if (bpp != 24 && bpp != 32)
        return img;  // only truecolor DIBs

    int w = hdr.biWidth;
    int h = hdr.biHeight;
    const bool top_down = h < 0;
    if (h < 0) h = -h;
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384)
        return img;

    const int stride = (w * bpp + 31) / 32 * 4;  // rows padded to 4 bytes
    const SIZE_T pixels = (SIZE_T)stride * h;
    const SIZE_T offset = hdr.biSize;
    if (offset + pixels > size)
        return img;  // truncated buffer

    const auto* src = static_cast<const unsigned char*>(bits) + offset;
    std::vector<unsigned char> rgba((SIZE_T)w * h * 4);
    for (int y = 0; y < h; ++y) {
        // Bottom-up DIBs store the last row first.
        const int sy = top_down ? y : (h - 1 - y);
        const unsigned char* row = src + (SIZE_T)sy * stride;
        unsigned char* dst = rgba.data() + (SIZE_T)y * w * 4;
        for (int x = 0; x < w; ++x) {
            const unsigned char* px = row + (SIZE_T)x * (bpp / 8);
            *dst++ = px[2];  // R
            *dst++ = px[1];  // G
            *dst++ = px[0];  // B
            *dst++ = (bpp == 32) ? px[3] : 0xFF;
        }
    }
    img.width = w;
    img.height = h;
    img.png = encode_png(rgba.data(), w, h);
    return img;
}

PastedImage clipboard_get_image()
{
    PastedImage img;
    if (!IsClipboardFormatAvailable(CF_DIB))
        return img;
    if (!OpenClipboard(nullptr))
        return img;
    HANDLE handle = GetClipboardData(CF_DIB);
    if (handle) {
        const SIZE_T size = GlobalSize(handle);
        void* bits = GlobalLock(handle);
        if (bits) {
            img = dib_to_image(bits, size);
            GlobalUnlock(handle);
        }
    }
    CloseClipboard();
    return img;
}

#else  // non-Windows clipboard image access is not implemented yet

PastedImage clipboard_get_image() { return {}; }

#endif

} // namespace krastor
