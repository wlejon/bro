#include "platform/dialogs.h"
#include "util/log.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_messagebox.h>
#include <cctype>
#include <utility>

namespace bro::platform {

namespace {

SDL_Window* s_window = nullptr;
bool s_interactive = true;
bool s_autoAccept = true;
std::vector<std::string> s_queuedPicks;

struct DialogResult {
    bool done = false;
    std::vector<std::string> files;
};

void SDLCALL dialogCallback(void* userdata, const char* const* filelist, int /*filter*/) {
    auto* res = static_cast<DialogResult*>(userdata);
    if (filelist) {
        for (int i = 0; filelist[i]; ++i) {
            res->files.emplace_back(filelist[i]);
        }
    }
    res->done = true;
}

void waitForDialog(DialogResult& res) {
    while (!res.done) {
        SDL_PumpEvents();
        SDL_Delay(10);
    }
}

enum : int { kBtnCancel = 0, kBtnOk = 1 };

bool showMessageBox(const std::string& message, bool withCancel) {
    SDL_MessageBoxButtonData buttons[2];
    int nButtons = 0;
    if (withCancel) {
        buttons[nButtons++] = {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, kBtnCancel, "Cancel"};
    }
    buttons[nButtons++] = {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, kBtnOk, "OK"};

    SDL_MessageBoxData data{};
    data.flags = SDL_MESSAGEBOX_INFORMATION;
    data.window = s_window;
    data.title = "";
    data.message = message.c_str();
    data.numbuttons = nButtons;
    data.buttons = buttons;

    int pressed = kBtnCancel;
    if (!SDL_ShowMessageBox(&data, &pressed)) {
        LOG_WARN("[dialog] message box unavailable: %s", SDL_GetError());
        return !withCancel;
    }
    return pressed == kBtnOk;
}

} // namespace

void Dialogs::setWindow(SDL_Window* window) {
    s_window = window;
}

void Dialogs::setInteractive(bool interactive) {
    s_interactive = interactive;
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

std::vector<std::string> Dialogs::pickFiles(const std::string& accept,
                                            bool allowMultiple) {
    if (!s_interactive) {
        std::vector<std::string> picked;
        picked.swap(s_queuedPicks);
        return picked;
    }

    std::string pattern;
    auto add = [&pattern](const std::string& ext) {
        if (ext.empty()) return;
        if (!pattern.empty()) pattern += ';';
        pattern += ext;
    };
    size_t start = 0;
    while (start <= accept.size() && !accept.empty()) {
        size_t comma = accept.find(',', start);
        std::string tok = accept.substr(start, comma == std::string::npos
                                                   ? std::string::npos
                                                   : comma - start);
        while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.front()))) tok.erase(tok.begin());
        while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.back()))) tok.pop_back();
        if (!tok.empty()) {
            if (tok[0] == '.') {
                add(tok.substr(1));
            } else if (tok == "image/*") {
                add("png"); add("jpg"); add("jpeg"); add("gif"); add("webp"); add("bmp");
            } else if (tok == "audio/*") {
                add("wav"); add("mp3"); add("ogg"); add("flac");
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

    SDL_DialogFileFilter sdlFilter{};
    bool hasFilter = !pattern.empty();
    if (hasFilter) {
        sdlFilter.name = "Accepted files";
        sdlFilter.pattern = pattern.c_str();
    }

    DialogResult result;
    SDL_ShowOpenFileDialog(dialogCallback, &result, s_window,
                           hasFilter ? &sdlFilter : nullptr,
                           hasFilter ? 1 : 0,
                           nullptr, allowMultiple);
    waitForDialog(result);
    return std::move(result.files);
}

} // namespace bro::platform
