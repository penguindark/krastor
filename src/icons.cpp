#include "icons.h"

#include <glad/glad.h>

#include "stb_image_write.h"  // implementation lives in clipboard.cpp

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

// The single Krastor translation unit that touches LunaSVG: the library is
// isolated behind this file (see src/icons.h for the rationale).
#include <lunasvg.h>

#include "config.h"

namespace krastor {

namespace {

// Reads a whole file into a string; false on any I/O error.
bool read_file(const std::string& path, std::string& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// Lucide icons use stroke="currentColor"; LunaSVG resolves currentColor to
// black. The atlas is tinted at draw time, so rewrite it to white and keep
// the asset files untouched.
void whiten_current_color(std::string& svg)
{
    static const std::string from = "currentColor";
    static const std::string to = "#ffffff";
    for (size_t pos = 0; (pos = svg.find(from, pos)) != std::string::npos;) {
        svg.replace(pos, from.size(), to);
        pos += to.size();
    }
}

} // namespace

bool IconAtlas::rasterize(const std::string& dir, int cell)
{
    ok_ = false;
    rects_.clear();
    pixels_.clear();
    tex_w_ = tex_h_ = 0;

    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec))
        return false;

    // Collect and sort so the atlas layout is deterministic across runs.
    std::vector<std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".svg")
            files.push_back(entry.path().string());
    }
    std::sort(files.begin(), files.end());
    if (files.empty())
        return false;

    const int cols = static_cast<int>(
        std::ceil(std::sqrt(static_cast<double>(files.size()))));
    const int rows = (static_cast<int>(files.size()) + cols - 1) / cols;
    tex_w_ = cols * cell;
    tex_h_ = rows * cell;
    pixels_.assign(static_cast<size_t>(tex_w_) * tex_h_ * 4, 0);

    int loaded = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        std::string svg;
        if (!read_file(files[i], svg))
            continue;
        whiten_current_color(svg);
        const auto doc = lunasvg::Document::loadFromData(svg);
        if (!doc)
            continue;
        const lunasvg::Bitmap bmp = doc->renderToBitmap(cell, cell);
        if (!bmp.data() || bmp.width() <= 0 || bmp.height() <= 0)
            continue;

        const int col = static_cast<int>(i) % cols;
        const int row = static_cast<int>(i) / cols;
        const int ox = col * cell;
        const int oy = row * cell;
        for (int y = 0; y < cell && y < bmp.height(); ++y) {
            const unsigned char* src =
                bmp.data() + static_cast<size_t>(y) * bmp.stride();
            unsigned char* dst =
                pixels_.data() +
                (static_cast<size_t>(oy + y) * tex_w_ + ox) * 4;
            std::copy(src, src + static_cast<size_t>(cell) * 4, dst);
        }
        const std::string name =
            std::filesystem::path(files[i]).stem().string();
        rects_[name] = Rect{ox, oy, cell, cell};
        ++loaded;
    }
    ok_ = loaded > 0;
    return ok_;
}

bool IconAtlas::load_dir(const std::string& dir, int cell)
{
    if (!rasterize(dir, cell))
        return false;

    if (tex_)
        glDeleteTextures(1, &tex_);
    glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex_w_, tex_h_, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, pixels_.data());
    return true;
}

bool IconAtlas::load_default(int cell)
{
    const std::string dir_exe = Config::exe_dir() + "/data/icons";
    if (load_dir(dir_exe, cell))
        return true;
    return load_dir("data/icons", cell);
}

const IconAtlas::Rect* IconAtlas::find(const std::string& name) const
{
    const auto it = rects_.find(name);
    return it == rects_.end() ? nullptr : &it->second;
}

std::vector<std::string> IconAtlas::names() const
{
    std::vector<std::string> out;
    out.reserve(rects_.size());
    for (const auto& [name, r] : rects_)
        out.push_back(name);
    std::sort(out.begin(), out.end());
    return out;
}

bool IconAtlas::dump_png(const std::string& path) const
{
    if (pixels_.empty())
        return false;
    return stbi_write_png(path.c_str(), tex_w_, tex_h_, 4, pixels_.data(),
                          tex_w_ * 4) != 0;
}

double IconAtlas::coverage(const std::string& name) const
{
    const Rect* r = find(name);
    if (!r || pixels_.empty())
        return -1.0;
    long painted = 0;
    for (int y = 0; y < r->h; ++y) {
        for (int x = 0; x < r->w; ++x) {
            const unsigned char* px =
                pixels_.data() +
                (static_cast<size_t>(r->y + y) * tex_w_ + r->x + x) * 4;
            if (px[3] > 0)
                ++painted;
        }
    }
    return static_cast<double>(painted) / (static_cast<double>(r->w) * r->h);
}

} // namespace krastor
