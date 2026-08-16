#pragma once

extern "C" {
#include "quickjs.h"
}

#include <functional>

struct SDL_Window;

namespace bro::js {

class DialogBindings {
public:
    /// Install showOpenFileDialog() and showSaveFileDialog() on the global object.
    /// The optional tickCb is called repeatedly while a dialog is open, so that
    /// JS timers (e.g. sequencer) and pending jobs keep running.
    using TickCallback = std::function<void()>;
    /// `interactive` false (headless, server) makes the modal dialogs —
    /// alert/confirm/prompt — answer themselves instead of blocking forever on
    /// a window nobody is looking at. See setAutoDialogAnswer().
    static void install(JSContext* ctx, SDL_Window* window,
                        TickCallback tickCb = nullptr,
                        bool interactive = true);

    /// What alert/confirm/prompt do when there is no user: `accept` decides
    /// confirm's answer and whether prompt returns its default (true) or null
    /// (false). Defaults to accepting, so a script driving an app walks
    /// through its confirmation prompts rather than stopping at the first one.
    /// Headless exposes this as `setDialogAnswer()`.
    static void setAutoDialogAnswer(bool accept);
};

} // namespace bro::js
