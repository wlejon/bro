#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

struct SDL_Window;

namespace bro::platform {

/// Native dialogs as engine operations, with no scripting runtime in sight.
/// The bronze host layer's `alert`/`confirm`/`prompt`/`show*Dialog` globals and
/// the C-ABI dialogs bridge are thin wrappers over these, so a compiled app's
/// `confirm()` obeys the same headless auto-answer a scripted one does.
///
/// Only a windowed run has someone to answer a dialog. Headless and server
/// runs set `setInteractive(false)`: the modal dialogs answer themselves
/// (`setAutoDialogAnswer`), and the file dialogs return what `setPickedFiles`
/// queued, once. A dialog that blocks a window nobody is looking at is a hang.
class Dialogs {
public:
    using TickCallback = std::function<void()>;

    static void setWindow(SDL_Window* window);
    static void setInteractive(bool interactive);
    /// Called repeatedly while a native dialog is open so timers and pending
    /// jobs keep running behind it. Clear it before the object it captures dies.
    static void setTickCallback(TickCallback cb);

    /// Queue what the next file/folder/save dialog (or `<input type=file>`
    /// pick) returns when there is no user to ask. Headless exposes this as
    /// `setPickedFiles()`.
    static void setPickedFiles(std::vector<std::string> paths);
    /// What alert/confirm/prompt do when there is no user: `accept` decides
    /// confirm's answer and whether prompt returns its default (true) or
    /// nullopt (false). Defaults to accepting, so a script driving an app walks
    /// through its confirmation prompts rather than stopping at the first one.
    /// Headless exposes this as `setDialogAnswer()`.
    static void setAutoDialogAnswer(bool accept);

    static void showAlert(const std::string& message);
    static bool showConfirm(const std::string& message);
    /// Answers the default text, or nullopt for a cancel — prompt's two
    /// outcomes; there is no text entry in a message box.
    static std::optional<std::string> showPrompt(const std::string& message,
                                                 const std::string& defaultText = "");

    /// The three file dialogs. `filter` is name and pattern alternating
    /// (`"Images|png;jpg|All files|*"`; a lone pattern is a filter called
    /// "Files"), and a pattern is `[a-zA-Z0-9_.-]` extensions separated by `;`
    /// or a bare `*`. Anything else is refused BEFORE a dialog opens: the call
    /// returns false with `refusal` set, which a caller must not report as a
    /// cancel — a cancel is `true` with nothing picked. A refusal reaches the
    /// script as an exception carrying SDL's own sentence, because the script
    /// wrote the filter SDL is objecting to.
    static bool showOpenFileDialog(const std::string& filter, bool allowMultiple,
                                   std::vector<std::string>& picked, std::string& refusal);
    static bool showOpenFolderDialog(const std::string& defaultLocation, bool allowMultiple,
                                     std::vector<std::string>& picked, std::string& refusal);
    /// `saved` is nullopt for a cancel.
    static bool showSaveFileDialog(const std::string& filter, const std::string& defaultName,
                                   std::optional<std::string>& saved, std::string& refusal);

    /// `<input type=file>`'s picker: `accept` is the attribute's comma list of
    /// `.ext` / `image/*` tokens, turned into one filter. Empty on cancel or
    /// refusal (the attribute is the page's, not a script's, so there is no
    /// one to throw at).
    static std::vector<std::string> pickFiles(const std::string& accept,
                                              bool allowMultiple);
};

} // namespace bro::platform
