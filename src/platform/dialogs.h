#pragma once

#include <optional>
#include <string>
#include <vector>

struct SDL_Window;

namespace bro::platform {

class Dialogs {
public:
    static void setWindow(SDL_Window* window);
    static void setInteractive(bool interactive);
    static void setPickedFiles(std::vector<std::string> paths);
    static void setAutoDialogAnswer(bool accept);

    static void showAlert(const std::string& message);
    static bool showConfirm(const std::string& message);
    static std::optional<std::string> showPrompt(const std::string& message,
                                                 const std::string& defaultText = "");
    static std::vector<std::string> pickFiles(const std::string& accept,
                                              bool allowMultiple);
};

} // namespace bro::platform
