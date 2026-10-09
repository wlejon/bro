#pragma once
// Desktop facts and services that belong to no window: power, the light/dark
// theme, opening a URL in the user's handler.

#include <string>

namespace bro::platform {

enum class SystemTheme { Unknown, Light, Dark };

struct PowerInfo {
    enum class State {
        Unknown,    // cannot tell (or the query failed)
        NoBattery,  // a desktop on mains power
        OnBattery,
        Charging,
        Charged,
    } state = State::Unknown;
    int secondsLeft = -1;  // battery time remaining; -1 unknown
    int percent = -1;      // charge level 0..100; -1 unknown
};

class SystemInfo {
public:
    virtual ~SystemInfo() = default;
    virtual PowerInfo power() = 0;
    /// The OS light/dark preference. Changes arrive as
    /// EventLoop::onSystemThemeChanged.
    virtual SystemTheme theme() = 0;
    /// Open `url` (http(s), mailto, file, ...) with the user's handler. On
    /// failure returns false and, when given, sets `error`.
    virtual bool openUrl(const std::string& url, std::string* error = nullptr) = 0;
};

/// The active WindowSystem's system services.
SystemInfo& systemInfo();

} // namespace bro::platform
