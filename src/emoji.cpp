#include "emoji.h"

#include <glad/glad.h>

#include "stb_image.h"  // implementation lives in clipboard.cpp

#include <fstream>

#include <nlohmann/json.hpp>

#include "config.h"

namespace krastor {

bool EmojiAtlas::load(const std::string& png_path, const std::string& json_path)
{
    ok_ = false;

    std::ifstream mf(json_path);
    if (!mf)
        return false;
    nlohmann::json m;
    try {
        m = nlohmann::json::parse(mf);
    } catch (const nlohmann::json::exception&) {
        return false;
    }
    if (!m.is_object() || !m.contains("_cell") || !m.contains("_cols"))
        return false;
    const int cell = m["_cell"].get<int>();
    const int cols = m["_cols"].get<int>();
    const int px = m.contains("_px") ? m["_px"].get<int>() : cell;

    int w = 0, h = 0, n = 0;
    unsigned char* img = stbi_load(png_path.c_str(), &w, &h, &n, 4);
    if (!img)
        return false;

    if (tex_)
        glDeleteTextures(1, &tex_);
    glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, img);
    stbi_image_free(img);
    tex_w_ = w;
    tex_h_ = h;

    rects_.clear();
    for (const auto& [key, val] : m.items()) {
        if (key.size() < 2 || key[0] != 'U' || key[1] != '+' || !val.is_array() ||
            val.size() != 2)
            continue;  // skip _cell/_cols/_px and malformed entries
        char32_t cp = 0;
        for (char c : key.substr(2))
            cp = cp * 16 + (c <= '9' ? c - '0'
                                     : (c | 0x20) - 'a' + 10);
        const int col = val[0].get<int>();
        const int row = val[1].get<int>();
        rects_[cp] = Rect{col * cell, row * cell, cell, cell};
    }
    ok_ = !rects_.empty();
    return ok_;
}

bool EmojiAtlas::load_default()
{
    const std::string dir_exe = Config::exe_dir() + "/data/fonts";
    if (load(dir_exe + "/emoji_atlas.png", dir_exe + "/emoji_atlas.json"))
        return true;
    return load("data/fonts/emoji_atlas.png", "data/fonts/emoji_atlas.json");
}

const EmojiAtlas::Rect* EmojiAtlas::find(char32_t cp) const
{
    if (cp == 0xFE0F)
        return nullptr;
    const auto it = rects_.find(cp);
    return it == rects_.end() ? nullptr : &it->second;
}

} // namespace krastor
