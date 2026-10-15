#pragma once

// Between desktop_hotkeys.cpp (the registry and the key-routing state
// machine) and the Windows desktop shell's keyboard hook
// (desktop_shell_keyboard_win.cpp), which runs on a thread of its own.

#include "platform/desktop_hotkeys.h"

#include <cstddef>
#include <functional>

namespace bro::platform::desktop {

/// routeHotkeyKey, with the callback queued for pumpHotkeyEvents instead of
/// run (any thread).
HotkeyKeyResult routeHotkeyKeyDeferred(const HotkeyKey& key);
/// Queues a task for pumpHotkeyEvents (any thread).
void queueHotkeyTask(std::function<void()> task);
/// How many hotkeys are registered.
size_t registeredHotkeyCount();
/// The shell's key sink, or an empty function.
ShellKeySink shellKeySink();

/// Starts or stops the keyboard hook to match the shell's routing being on
/// and any hotkey registered (Windows; elsewhere nothing). Never call with
/// the registry's lock held.
void updateShellKeyboardHook();

}  // namespace bro::platform::desktop
