#pragma once

// Where a text control's `change` event is decided.
//
// `input` is a keystroke and `change` is a departure, so neither the key
// handler nor the focus handler can decide one alone: what makes an edit
// reportable is that the value differs from what it was when focus arrived.
// That is HTML's own rule, and it is the reason typing a character and
// deleting it again fires nothing at all.
//
// One home for it because ElInput and ElTextarea both need it and a second
// copy would drift. A script writing `.value` arms it rather than reporting
// through it: the value changed, but nobody *changed* it in the sense this
// event is about, and a spurious change on the next click away is worse than
// none — it is an edit the application never received.

#include <string>

namespace bro::layout {

class ValueChange {
public:
    /// This value counts as already reported. Called when focus arrives and
    /// when a script rewrites the value.
    void arm(const std::string& value) { base_ = value; }

    /// Is there an edit to report? Answers once — the baseline moves to the
    /// value handed over, so an Enter that reported and the blur following it
    /// do not report the same edit twice.
    bool take(const std::string& value) {
        if (value == base_) return false;
        base_ = value;
        return true;
    }

private:
    std::string base_;
};

} // namespace bro::layout
