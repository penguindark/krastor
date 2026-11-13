// Krastor — in-app key repeat for every text field (GUI).
// The OS repeat rate is not something the app can own: the GLFW backend
// forwards every GLFW_REPEAT event (and its repeated char) straight to
// Nuklear. So gui.cpp swallows the repeats at the callback boundary and
// re-synthesizes them through this class at gui.key_repeat_speed: a fixed
// initial delay, then 30 repeats/s scaled by the speed. Pure logic (no
// GLFW types in the header) so --gui-test can drive it with a fake clock.

#pragma once

namespace krastor {

class KeyRepeater {
public:
    // gui.key_repeat_speed, clamped to [0.25, 4.0]; 1.0 ~ Windows default.
    void set_speed(double s);
    double speed() const { return speed_; }

    // GLFW key event feed: action is the raw GLFW action (0 release,
    // 1 press, 2 repeat), mods the raw GLFW modifier bitmask. A press
    // held with Ctrl/Alt never repeats (Ctrl+C/V spam guard).
    void on_key(int key, int action, int mods, double now);
    // The char produced by the current press (call after on_key press).
    void on_char(unsigned int cp);
    // False from the first swallowed repeat until the key is released:
    // while false, repeated chars must not reach Nuklear.
    bool char_allowed() const { return allow_char_; }

    // What to inject this frame (once per frame, before new_frame).
    // Fire::Char -> *cp; Fire::Key -> *key (raw GLFW key code).
    enum class Fire { None, Key, Char };
    Fire tick(double now, int* key, unsigned int* cp);

private:
    double speed_ = 1.0;
    int held_ = -1;              // key currently held (-1 = none)
    double press_t_ = 0.0;       // when held_ went down
    double fire_t_ = 0.0;        // last injected repeat
    unsigned int cp_ = 0;        // char produced by held_ (0 = special key)
    bool allow_char_ = true;     // false while a repeat owns the stream
};

} // namespace krastor
