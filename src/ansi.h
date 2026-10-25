// Krastor — minimal ANSI/SGR escape-sequence parser for terminal-style text.
// Splits a string containing ANSI escape codes (as produced by shells, CLI
// tools, diffs and some LLM output) into styled runs: text + foreground color +
// background color + bold. This is what makes diff views, syntax-highlighted
// code and colored tool output renderable in the chat panel.
// Unsupported sequences (cursor movement, OSC, ...) are stripped so they never
// appear as garbage. Header-only, no dependencies.
//
// Supported SGR: 0 reset, 1/22 bold, 30-37/90-97/39 foreground,
// 40-47/100-107/49 background, 38;5;n & 38;2;r;g;b fg, 48;5;n & 48;2;r;g;b bg.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace krastor {

struct AnsiRun {
    std::string text;
    uint8_t r, g, b;      // foreground
    bool bold;
    uint8_t br, bg, bb;   // background
    bool has_bg;
};

namespace ansi_detail {

// Standard xterm 16-color palette (normal + bright).
inline void palette16(int code, uint8_t& r, uint8_t& g, uint8_t& b)
{
    static const uint8_t k[16][3] = {
        {0, 0, 0},       {170, 0, 0},     {0, 170, 0},   {170, 85, 0},
        {0, 0, 170},     {170, 0, 170},   {0, 170, 170}, {170, 170, 170},
        {85, 85, 85},    {255, 85, 85},   {85, 255, 85}, {255, 255, 85},
        {85, 85, 255},   {255, 85, 255}, {85, 255, 255}, {255, 255, 255},
    };
    if (code >= 0 && code < 16) {
        r = k[code][0]; g = k[code][1]; b = k[code][2];
    }
}

// xterm 256-color cube + grayscale ramp (codes 16..255).
inline void palette256(int code, uint8_t& r, uint8_t& g, uint8_t& b)
{
    if (code < 16) { palette16(code, r, g, b); return; }
    if (code >= 232) {  // grayscale ramp: 8, 18, ... 238
        const uint8_t v = static_cast<uint8_t>(8 + (code - 232) * 10);
        r = g = b = v;
        return;
    }
    const int c = code - 16;
    const int ri = c / 36, gi = (c / 6) % 6, bi = c % 6;
    auto lvl = [](int i) { return static_cast<uint8_t>(i ? 55 + i * 40 : 0); };
    r = lvl(ri); g = lvl(gi); b = lvl(bi);
}

} // namespace ansi_detail

// Parses `in` into styled runs. Default foreground is (dr,dg,db), no background.
inline std::vector<AnsiRun> ansi_parse(const std::string& in,
                                       uint8_t dr, uint8_t dg, uint8_t db)
{
    std::vector<AnsiRun> runs;
    uint8_t cr = dr, cg = dg, cb = db;
    uint8_t br = 0, bg = 0, bb = 0;
    bool bold = false, has_bg = false;

    auto same_style = [&](const AnsiRun& a) {
        return a.r == cr && a.g == cg && a.b == cb && a.bold == bold &&
               a.has_bg == has_bg && a.br == br && a.bg == bg && a.bb == bb;
    };
    auto push = [&](char c) {
        if (!runs.empty() && same_style(runs.back()))
            runs.back().text += c;
        else
            runs.push_back({std::string(1, c), cr, cg, cb, bold, br, bg, bb, has_bg});
    };

    size_t i = 0;
    const size_t n = in.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c != 0x1b) {  // ordinary byte (UTF-8 sequences pass through)
            if (c != '\r')  // CR alone would overwrite the line
                push(in[i]);
            ++i;
            continue;
        }
        // --- escape sequence ---
        if (i + 1 >= n) break;
        const char next = in[i + 1];
        if (next == '[') {
            size_t j = i + 2;
            std::vector<int> params;
            int cur = -1;
            while (j < n) {
                const char d = in[j];
                if (d >= '0' && d <= '9') {
                    cur = (cur < 0 ? 0 : cur * 10) + (d - '0');
                    ++j;
                } else if (d == ';') {
                    params.push_back(cur < 0 ? 0 : cur);
                    cur = -1;
                    ++j;
                } else {
                    break;  // final byte
                }
            }
            params.push_back(cur < 0 ? 0 : cur);
            const char final = (j < n) ? in[j] : 0;
            if (final == 'm') {  // SGR
                for (size_t p = 0; p < params.size(); ++p) {
                    const int v = params[p];
                    if (v == 0) {
                        cr = dr; cg = dg; cb = db; bold = false; has_bg = false;
                    } else if (v == 1) bold = true;
                    else if (v == 22) bold = false;
                    else if (v >= 30 && v <= 37)
                        ansi_detail::palette16(v - 30, cr, cg, cb);
                    else if (v == 39) { cr = dr; cg = dg; cb = db; }
                    else if (v >= 90 && v <= 97)
                        ansi_detail::palette16(v - 90 + 8, cr, cg, cb);
                    else if (v >= 40 && v <= 47) {
                        ansi_detail::palette16(v - 40, br, bg, bb); has_bg = true;
                    } else if (v == 49) has_bg = false;
                    else if (v >= 100 && v <= 107) {
                        ansi_detail::palette16(v - 100 + 8, br, bg, bb); has_bg = true;
                    } else if (v == 38 && p + 1 < params.size()) {
                        if (params[p + 1] == 5 && p + 2 < params.size()) {
                            ansi_detail::palette256(params[p + 2], cr, cg, cb);
                            p += 2;
                        } else if (params[p + 1] == 2 && p + 4 < params.size()) {
                            cr = static_cast<uint8_t>(params[p + 2]);
                            cg = static_cast<uint8_t>(params[p + 3]);
                            cb = static_cast<uint8_t>(params[p + 4]);
                            p += 4;
                        }
                    } else if (v == 48 && p + 1 < params.size()) {
                        if (params[p + 1] == 5 && p + 2 < params.size()) {
                            ansi_detail::palette256(params[p + 2], br, bg, bb);
                            has_bg = true;
                            p += 2;
                        } else if (params[p + 1] == 2 && p + 4 < params.size()) {
                            br = static_cast<uint8_t>(params[p + 2]);
                            bg = static_cast<uint8_t>(params[p + 3]);
                            bb = static_cast<uint8_t>(params[p + 4]);
                            has_bg = true;
                            p += 4;
                        }
                    }
                    // other codes (italics, underline, ...) are ignored
                }
            }
            i = (j < n) ? j + 1 : n;  // skip past the final byte
        } else if (next == ']') {  // OSC: terminated by BEL or ESC-backslash
            size_t j = i + 2;
            while (j < n && in[j] != '\a' &&
                   !(in[j] == '\x1b' && j + 1 < n && in[j + 1] == '\\'))
                ++j;
            i = (j < n) ? (in[j] == '\a' ? j + 1 : j + 2) : n;
        } else {
            i += 2;  // other two-byte escapes: drop
        }
    }
    return runs;
}

} // namespace krastor
