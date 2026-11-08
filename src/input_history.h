#pragma once
// Krastor - console input history (bash-style Up/Down recall).
// Header-only and UI-free so --gui-test can exercise it: push() records
// submitted lines (trimmed; empty and consecutive duplicates ignored,
// oldest evicted past the cap), prev()/next() walk the entries while
// "browsing", and leave() ends browsing when the user edits a recalled
// line. The GUI (gui.cpp) owns the key handling and the caret placement.

#include <cstddef>
#include <string>
#include <vector>

namespace krastor {

namespace input_detail {
inline std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && static_cast<unsigned char>(s[a]) <= ' ')
        ++a;
    while (b > a && static_cast<unsigned char>(s[b - 1]) <= ' ')
        --b;
    return s.substr(a, b - a);
}
}  // namespace input_detail

class InputHistory {
public:
    // Max stored lines; 0 disables the history entirely (push ignores
    // everything). From config.jsonc "gui"."history_size".
    void set_max(size_t n) { max_ = n; }
    size_t max() const { return max_; }
    size_t size() const { return items_.size(); }

    // Records a submitted line (before the input field is cleared).
    // Whitespace-only lines and duplicates of the newest entry are
    // skipped (bash behavior); the oldest entry is evicted past the cap.
    void push(const std::string& text)
    {
        const std::string t = input_detail::trim(text);
        if (max_ == 0 || t.empty())
            return;
        if (!items_.empty() && items_.back() == t) {
            pos_ = -1;  // a new send restarts browsing
            return;
        }
        items_.push_back(t);
        while (items_.size() > max_)
            items_.erase(items_.begin());
        pos_ = -1;
    }

    // True while Up/Down navigation is active (the field shows an entry).
    bool browsing() const { return pos_ >= 0; }

    // Up: from rest starts at the newest entry (the caller only allows
    // that from an empty field); at the oldest entry there is no more.
    bool can_prev() const { return !items_.empty() && pos_ != 0; }
    // Down: while browsing there is always a way out (past the newest
    // comes the draft, which ends browsing).
    bool can_next() const { return pos_ >= 0; }

    // The text to show in the field, or nullptr when the move is invalid.
    const std::string* prev()
    {
        if (!can_prev())
            return nullptr;
        pos_ = pos_ < 0 ? static_cast<int>(items_.size()) - 1 : pos_ - 1;
        return &items_[pos_];
    }
    const std::string* next()
    {
        if (!can_next())
            return nullptr;
        if (pos_ >= static_cast<int>(items_.size()) - 1) {
            pos_ = -1;
            return &draft_;  // what was typed before browsing (usually "")
        }
        ++pos_;
        return &items_[pos_];
    }

    // The user edited the recalled text: stop browsing so the next Up
    // starts fresh from the newest entry.
    void leave() { pos_ = -1; }

private:
    std::vector<std::string> items_;
    std::string draft_;  // returned when Down passes the newest entry
    size_t max_ = 100;
    int pos_ = -1;  // -1 = not browsing
};

}  // namespace krastor
