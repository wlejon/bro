#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bro::platform {

class Window;

/// The native half of a dialog, which the active WindowSystem provides.
/// Dialogs (below) owns the policy — who answers, filter validation — and
/// calls this only when a user is there to ask.
class DialogBackend {
public:
    virtual ~DialogBackend() = default;

    /// An OK (or OK / Cancel) message box over `parent` (may be null).
    /// Answers whether OK was chosen; nullopt when no message box could be
    /// shown at all.
    virtual std::optional<bool> messageBox(Window* parent, const std::string& message,
                                           bool withCancel) = 0;

    struct FileFilter {
        std::string name;
        std::string pattern;  // `png;jpg` or `*`, already validated
    };
    enum class FileDialogKind { OpenFile, OpenFolder, SaveFile };
    struct FileDialogRequest {
        FileDialogKind kind = FileDialogKind::OpenFile;
        std::vector<FileFilter> filters;  // OpenFile / SaveFile
        std::string defaultLocation;      // folder, or the save name; "" = none
        bool allowMultiple = false;       // OpenFile / OpenFolder
    };

    /// A file dialog that stays open while the app runs on.
    class PendingFileDialog {
    public:
        virtual ~PendingFileDialog() = default;
        /// Non-blocking, on the main thread: true once the dialog is
        /// answered, then `ok` with the chosen paths (none for a cancel), or
        /// !ok for a refusal with the reason in `refusal`.
        virtual bool poll(bool& ok, std::vector<std::string>& picked, std::string& refusal) = 0;
    };
    /// Open a file dialog and return at once; the answer comes through poll().
    virtual std::unique_ptr<PendingFileDialog> openFileDialog(Window* parent,
                                                              const FileDialogRequest& request) = 0;

    /// Show a file dialog and block until it is answered. True with the
    /// chosen paths (none for a cancel); false when the dialog was refused,
    /// with the reason in `refusal`.
    virtual bool fileDialog(Window* parent, const FileDialogRequest& request,
                            std::vector<std::string>& picked, std::string& refusal) = 0;
};

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
    /// The window dialogs are parented on (null for none).
    static void setWindow(Window* window);
    static void setInteractive(bool interactive);

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

    /// Called on the main thread with the paths every file, folder and save
    /// dialog (sync, async, `<input type=file>`) answered with, before the
    /// caller sees them: the engine grants them to the app (the powerbox,
    /// engine/app_permissions.h).
    using PickHook = std::function<void(const std::vector<std::string>& paths)>;
    static void setPickHook(PickHook hook);

    /// Ask the user whether the app may have a capability: OK / Cancel.
    /// nullopt when there is no answer: no message box could be shown, or a
    /// headless run whose script never chose one with setDialogAnswer() (an
    /// unanswered prompt is a deny the caller does not keep).
    static std::optional<bool> askPermission(const std::string& message);

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
    /// script as an exception carrying the refusal's sentence, because the
    /// script wrote the filter being objected to.
    static bool showOpenFileDialog(const std::string& filter, bool allowMultiple,
                                   std::vector<std::string>& picked, std::string& refusal);
    static bool showOpenFolderDialog(const std::string& defaultLocation, bool allowMultiple,
                                     std::vector<std::string>& picked, std::string& refusal);
    /// `saved` is nullopt for a cancel.
    static bool showSaveFileDialog(const std::string& filter, const std::string& defaultName,
                                   std::optional<std::string>& saved, std::string& refusal);

    /// The same three dialogs without blocking: the app keeps running its
    /// frames (animation, audio fed from script, timers) while the dialog is
    /// up, and `done` is called on the main thread from pumpAsync() once it
    /// is answered: (true, paths) — none for a cancel, a save's one path —
    /// or (false, {}, refusal). Headless answers as the blocking ones do, at
    /// the next pump, so a caller never sees its answer inside the call.
    using FileDialogDone = std::function<void(bool ok, std::vector<std::string> picked, std::string refusal)>;
    static void openFileDialogAsync(const std::string& filter, bool allowMultiple, FileDialogDone done);
    static void openFolderDialogAsync(const std::string& defaultLocation, bool allowMultiple, FileDialogDone done);
    static void saveFileDialogAsync(const std::string& filter, const std::string& defaultName, FileDialogDone done);
    /// Settle the async dialogs answered since the last call; once a frame.
    static void pumpAsync();

    /// `<input type=file>`'s picker: `accept` is the attribute's comma list of
    /// `.ext` / `image/*` tokens, turned into one filter. Empty on cancel or
    /// refusal (the attribute is the page's, not a script's, so there is no
    /// one to throw at).
    static std::vector<std::string> pickFiles(const std::string& accept,
                                              bool allowMultiple);

    /// The filter string the last open/save file dialog was asked for
    /// ("Accepted files|wav;mp3;..."), shown or not: what headless's
    /// lastFileDialogFilter() reports.
    static std::string lastFileFilter();
};

} // namespace bro::platform
