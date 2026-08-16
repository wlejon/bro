#pragma once

extern "C" {
#include "quickjs.h"
}

#include <functional>
#include <optional>
#include <string>
#include <vector>

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

    /// Run the native open-file picker and return the chosen paths (empty if
    /// cancelled). This is what an `<input type=file>` click opens, so the
    /// control behaves the way a page expects rather than doing nothing.
    ///
    /// `accept` takes the HTML attribute's spelling (".obj,.gltf,image/*");
    /// an empty string shows every file. With no user to ask — headless — the
    /// paths queued by setPickedFiles() are returned instead, once.
    static std::vector<std::string> pickFiles(const std::string& accept,
                                              bool allowMultiple);

    /// Queue what the next pickFiles() returns when there is no user to ask.
    /// Headless exposes this as `setPickedFiles()`.
    static void setPickedFiles(std::vector<std::string> paths);

    /// What alert/confirm/prompt do when there is no user: `accept` decides
    /// confirm's answer and whether prompt returns its default (true) or null
    /// (false). Defaults to accepting, so a script driving an app walks
    /// through its confirmation prompts rather than stopping at the first one.
    /// Headless exposes this as `setDialogAnswer()`.
    static void setAutoDialogAnswer(bool accept);

    /// alert / confirm / prompt as engine operations, without a JSContext.
    /// The QuickJS bindings are thin wrappers over these, and so is the bronze
    /// host layer's copy (src/bronze_host) — a compiled app's `confirm()` must
    /// obey the same headless auto-answer a scripted one does, or a test that
    /// drives a compiled app hangs on the first confirmation it reaches.
    static void showAlert(const std::string& message);
    static bool showConfirm(const std::string& message);
    /// Answers the default text, or nullopt for a cancel — prompt's two
    /// outcomes are "a string" and "null", and an empty string is the first.
    static std::optional<std::string> showPrompt(const std::string& message,
                                                 const std::string& defaultText);
};

} // namespace bro::js
