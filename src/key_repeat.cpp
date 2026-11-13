#include "key_repeat.h"

#include <algorithm>

#include <GLFW/glfw3.h>   // key codes only (the header stays GLFW-free)

namespace krastor {

namespace {

constexpr double kDelay = 0.4;              // s before the first repeat
constexpr double kBaseInterval = 1.0 / 30.0;  // s between repeats at 1x

// Special keys that repeat as key events (the edit widgets consume them).
// HOME/END are polled as held-state by the backend, so they never repeat.
bool repeatable_key(int key)
{
    switch (key) {
    case GLFW_KEY_BACKSPACE: case GLFW_KEY_DELETE:
    case GLFW_KEY_LEFT:      case GLFW_KEY_RIGHT:
    case GLFW_KEY_UP:        case GLFW_KEY_DOWN:
        return true;
    default:
        return false;
    }
}

} // namespace

void KeyRepeater::set_speed(double s)
{
    speed_ = (std::clamp)(s, 0.25, 4.0);
}

void KeyRepeater::on_key(int key, int action, int mods, double now)
{
    if (action == GLFW_PRESS) {
        // Ctrl/Alt chords must never repeat (Ctrl+C would spam the char).
        held_ = (mods & (GLFW_MOD_CONTROL | GLFW_MOD_ALT)) ? -1 : key;
        press_t_ = now;
        fire_t_ = now;
        cp_ = 0;
        allow_char_ = true;
    } else if (action == GLFW_REPEAT) {
        // Swallowed upstream; it owns the char stream from now on.
        allow_char_ = false;
    } else if (key == held_) {  // GLFW_RELEASE
        held_ = -1;
        allow_char_ = true;
    }
}

void KeyRepeater::on_char(unsigned int cp)
{
    if (allow_char_)
        cp_ = cp;
}

KeyRepeater::Fire KeyRepeater::tick(double now, int* key, unsigned int* cp)
{
    if (held_ < 0)
        return Fire::None;
    if (cp_ == 0 && !repeatable_key(held_))
        return Fire::None;
    if (now - press_t_ < kDelay || now - fire_t_ < kBaseInterval / speed_)
        return Fire::None;
    fire_t_ = now;
    if (cp_ != 0) {
        *cp = cp_;
        return Fire::Char;
    }
    *key = held_;
    return Fire::Key;
}

} // namespace krastor
