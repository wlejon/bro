// Dialog policy: who answers (a user, or the headless auto-answer), what a
// file filter may say, and the `<input accept>` mapping. The native dialogs
// themselves are the active WindowSystem's DialogBackend.
#include "platform/dialogs.h"
#include "platform/window_system.h"
#include "util/log.h"

#include <cctype>
#include <cstring>
#include <utility>

namespace bro::platform {

namespace {

Window* s_window = nullptr;
bool s_interactive = true;
bool s_autoAccept = true;
Dialogs::TickCallback s_tickCb;
std::vector<std::string> s_queuedPicks;
std::string s_lastFileFilter;

std::string normalizeSeparators(std::string s) {
#ifdef _WIN32
    for (char& c : s) {
        if (c == '/') c = '\\';
    }
#endif
    return s;
}

// A filter string as a filter list. `"Images|png;jpg"` is one filter and
// `"Documents|json|Media|mp4;mkv|All files|*"` is three: names and patterns
// alternating. A string with no `|` at all is the pattern of a filter called
// "Files". A trailing name with no pattern is dropped rather than guessed at.
std::vector<DialogBackend::FileFilter> filtersFrom(const std::string& filterStr) {
    std::vector<DialogBackend::FileFilter> list;
    if (filterStr.empty()) return list;

    std::vector<std::string> parts;  // name, pattern, name, pattern, …
    for (size_t start = 0;;) {
        const size_t bar = filterStr.find('|', start);
        if (bar == std::string::npos) {
            parts.push_back(filterStr.substr(start));
            break;
        }
        parts.push_back(filterStr.substr(start, bar - start));
        start = bar + 1;
    }
    if (parts.size() == 1) parts.insert(parts.begin(), "Files");
    if (parts.size() % 2 != 0) parts.pop_back();

    list.reserve(parts.size() / 2);
    for (size_t i = 0; i + 1 < parts.size(); i += 2) list.push_back({parts[i], parts[i + 1]});
    return list;
}

// The one rule every backend's file dialog shares (it is SDL's), applied
// here so a bad pattern is refused with a sentence even on a backend that
// would answer it with a silent cancel.
const char* validateFilterPattern(const char* list) {
    if (!list || !*list) return "Empty pattern not allowed";
    if (std::strcmp(list, "*") == 0) return nullptr;
    for (const char* c = list; *c; ++c) {
        if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
            *c == '-' || *c == '_' || *c == '.') {
            continue;
        } else if (*c == ';') {
            if (c == list || c[-1] == ';') return "Empty pattern not allowed";
        } else {
            return "Invalid character in pattern (Only [a-zA-Z0-9_.-] allowed, or a single *)";
        }
    }
    if (list[std::strlen(list) - 1] == ';') return "Empty pattern not allowed";
    return nullptr;
}

bool refusedFilters(const std::vector<DialogBackend::FileFilter>& filters, std::string& refusal) {
    for (const auto& filter : filters) {
        if (const char* err = validateFilterPattern(filter.pattern.c_str())) {
            refusal = std::string("file dialog refused: Invalid dialog file filters: ") + err;
            return true;
        }
    }
    return false;
}

std::vector<std::string> takeQueuedPicks() {
    std::vector<std::string> picked;
    picked.swap(s_queuedPicks);
    return picked;
}

bool showMessageBox(const std::string& message, bool withCancel) {
    const std::optional<bool> ok = windowSystem().dialogs().messageBox(s_window, message, withCancel);
    // No message box at all: an alert has been "seen", a question declined.
    return ok ? *ok : !withCancel;
}

bool showFileDialog(const DialogBackend::FileDialogRequest& request,
                    std::vector<std::string>& picked, std::string& refusal) {
    return windowSystem().dialogs().fileDialog(s_window, request, s_tickCb, picked, refusal);
}

// `<input accept>`'s comma list of `.ext` and `type/*` tokens as one
// `;`-separated pattern.
std::string patternFromAccept(const std::string& accept) {
    std::string pattern;
    auto add = [&pattern](const std::string& ext) {
        if (ext.empty()) return;
        if (!pattern.empty()) pattern += ';';
        pattern += ext;
    };
    size_t start = 0;
    while (start <= accept.size() && !accept.empty()) {
        size_t comma = accept.find(',', start);
        std::string tok = accept.substr(start, comma == std::string::npos ? std::string::npos
                                                                          : comma - start);
        while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.front()))) tok.erase(tok.begin());
        while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.back()))) tok.pop_back();
        if (!tok.empty()) {
            if (tok[0] == '.') {
                add(tok.substr(1));
            } else if (tok == "image/*") {
                add("png"); add("jpg"); add("jpeg"); add("gif"); add("webp"); add("bmp");
            } else if (tok == "audio/*") {
                // What bro decodes (bro.media.canDecode): M4A/M4B too, which
                // the platform's AAC decoder plays where there is one.
                add("wav"); add("mp3"); add("ogg"); add("oga"); add("opus"); add("flac");
                add("m4a"); add("m4b");
            } else if (tok == "video/*") {
                add("webm"); add("mp4");
            } else if (auto slash = tok.find('/');
                       slash != std::string::npos && tok.substr(slash + 1) != "*") {
                add(tok.substr(slash + 1));
            }
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return pattern;
}

} // namespace

void Dialogs::setWindow(Window* window) {
    s_window = window;
}

void Dialogs::setInteractive(bool interactive) {
    s_interactive = interactive;
}

void Dialogs::setTickCallback(TickCallback cb) {
    s_tickCb = std::move(cb);
}

void Dialogs::setPickedFiles(std::vector<std::string> paths) {
    s_queuedPicks = std::move(paths);
}

void Dialogs::setAutoDialogAnswer(bool accept) {
    s_autoAccept = accept;
}

void Dialogs::showAlert(const std::string& message) {
    if (!s_interactive) {
        LOG_INFO("[alert] %s", message.c_str());
        return;
    }
    showMessageBox(message, false);
}

bool Dialogs::showConfirm(const std::string& message) {
    if (!s_interactive) {
        LOG_INFO("[confirm] %s -> %s", message.c_str(), s_autoAccept ? "OK" : "Cancel");
        return s_autoAccept;
    }
    return showMessageBox(message, true);
}

std::optional<std::string> Dialogs::showPrompt(const std::string& message,
                                               const std::string& defaultText) {
    if (!s_interactive) {
        LOG_INFO("[prompt] %s -> %s", message.c_str(),
                 s_autoAccept ? defaultText.c_str() : "(cancelled)");
        if (!s_autoAccept) return std::nullopt;
        return defaultText;
    }
    std::string full = message;
    if (!defaultText.empty()) full += "\n\n[" + defaultText + "]";
    if (!showMessageBox(full, true)) return std::nullopt;
    return defaultText;
}

std::string Dialogs::lastFileFilter() {
    return s_lastFileFilter;
}

bool Dialogs::showOpenFileDialog(const std::string& filter, bool allowMultiple,
                                 std::vector<std::string>& picked, std::string& refusal) {
    s_lastFileFilter = filter;
    DialogBackend::FileDialogRequest request;
    request.kind = DialogBackend::FileDialogKind::OpenFile;
    request.filters = filtersFrom(filter);
    request.allowMultiple = allowMultiple;
    if (refusedFilters(request.filters, refusal)) return false;

    if (!s_interactive) {
        picked = takeQueuedPicks();
        return true;
    }
    return showFileDialog(request, picked, refusal);
}

bool Dialogs::showOpenFolderDialog(const std::string& defaultLocation, bool allowMultiple,
                                   std::vector<std::string>& picked, std::string& refusal) {
    if (!s_interactive) {
        picked = takeQueuedPicks();
        return true;
    }

    DialogBackend::FileDialogRequest request;
    request.kind = DialogBackend::FileDialogKind::OpenFolder;
    request.defaultLocation = normalizeSeparators(defaultLocation);
    request.allowMultiple = allowMultiple;
    return showFileDialog(request, picked, refusal);
}

bool Dialogs::showSaveFileDialog(const std::string& filter, const std::string& defaultName,
                                 std::optional<std::string>& saved, std::string& refusal) {
    s_lastFileFilter = filter;
    DialogBackend::FileDialogRequest request;
    request.kind = DialogBackend::FileDialogKind::SaveFile;
    request.filters = filtersFrom(filter);
    if (refusedFilters(request.filters, refusal)) return false;

    request.defaultLocation = normalizeSeparators(defaultName);
    if (!s_interactive) {
        if (!s_autoAccept) {
            saved = std::nullopt;
            return true;
        }
        if (!s_queuedPicks.empty()) {
            saved = s_queuedPicks.front();
            s_queuedPicks.erase(s_queuedPicks.begin());
            return true;
        }
        saved = request.defaultLocation.empty() ? std::string("untitled") : request.defaultLocation;
        return true;
    }

    std::vector<std::string> picked;
    if (!showFileDialog(request, picked, refusal)) return false;
    if (picked.empty()) {
        saved = std::nullopt;
    } else {
        saved = picked[0];
    }
    return true;
}

std::vector<std::string> Dialogs::pickFiles(const std::string& accept, bool allowMultiple) {
    const std::string pattern = patternFromAccept(accept);
    std::vector<std::string> picked;
    std::string refusal;
    if (!showOpenFileDialog(pattern.empty() ? std::string() : "Accepted files|" + pattern,
                            allowMultiple, picked, refusal)) {
        LOG_WARN("[dialog] <input type=file> picker: %s", refusal.c_str());
        return {};
    }
    return picked;
}

} // namespace bro::platform
