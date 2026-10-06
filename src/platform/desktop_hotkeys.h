#pragma once

#include <cstdint>
#include <functional>
#include <string>

struct SDL_Window;

namespace bro::platform::desktop {

/// Registers a system-wide global hotkey. Returns a non-zero hotkey ID on success, or 0 on failure.
uint32_t registerGlobalHotkey(
    SDL_Window* window,
    const std::string& accelerator,
    std::function<void()> callback
);

/// Unregisters a global hotkey by its ID. Returns true if found and removed.
bool unregisterGlobalHotkey(uint32_t id);

/// Unregisters all global hotkeys.
void unregisterAllGlobalHotkeys();

/// Simulates hotkey press by accelerator string (useful for headless testing).
bool simulateGlobalHotkey(const std::string& accelerator);

/// Simulates hotkey press by ID.
bool simulateGlobalHotkeyId(uint32_t id);

/// Dispatches any queued hotkey events on the main thread.
void pumpHotkeyEvents();

} // namespace bro::platform::desktop
