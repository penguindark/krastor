// Krastor — monochrome UI icon sprite atlas.
// Rasterizes every data/icons/*.svg into one GL_RGBA texture using LunaSVG
// (git submodule libs/lunasvg). This is the ONLY module that includes
// lunasvg.h: the rest of Krastor sees just this class, so the SVG library
// stays a fully isolated, swappable dependency.
//
// Icons are rasterized white-on-transparent (the SVG "currentColor" is
// rewritten to #ffffff before parsing), so callers tint them at draw time
// via the nk_draw_image color. Same atlas pattern as the emoji atlas
// (src/emoji.h): one texture + name -> pixel-rect map, drawn as
// nk_image.region quads, no backend patch needed.

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace krastor {

class IconAtlas {
public:
    struct Rect {
        int x, y, w, h;  // icon cell in atlas pixel coords (nk_image region)
    };

    // Rasterizes every *.svg in dir (name = file stem) into a CPU-side
    // RGBA atlas of cell x cell icons. No GL — usable headless for tests.
    // Returns false when the folder is missing or no icon parsed.
    bool rasterize(const std::string& dir, int cell = 64);

    // rasterize() + GL texture upload — needs a current GL context.
    // Tries exe_dir()/data/icons, then ./data/icons (like EmojiAtlas).
    bool load_default(int cell = 64);
    bool load_dir(const std::string& dir, int cell = 64);

    // Pixel rect for an icon name, or nullptr.
    const Rect* find(const std::string& name) const;

    // Sorted icon names (test/diagnostics helper).
    std::vector<std::string> names() const;

    bool ok() const { return ok_; }
    unsigned texture() const { return tex_; }
    int tex_w() const { return tex_w_; }
    int tex_h() const { return tex_h_; }

    // --- test hooks (no GL required) ---

    // Writes the rasterized atlas to a PNG file (stb_image_write).
    bool dump_png(const std::string& path) const;
    // Fraction [0..1] of non-transparent pixels inside an icon's cell:
    // lets a test assert every icon actually drew something (and not,
    // say, a full black square). -1 when the name is unknown.
    double coverage(const std::string& name) const;

private:
    std::unordered_map<std::string, Rect> rects_;
    std::vector<unsigned char> pixels_;  // RGBA atlas, tex_w_ * tex_h_ * 4
    unsigned tex_ = 0;
    int tex_w_ = 0, tex_h_ = 0;
    bool ok_ = false;
};

} // namespace krastor
