# src/platform: the platform layer

Everything bro needs from the OS's window system goes through the small,
backend-neutral interfaces in this directory. A backend implements them, and
the rest of bro (engine/, layout/, bronze_host/, render/, terminal/) names
only the interfaces. **SDL is one backend and DRM is another.** Neither may
leak: an `#include <SDL3/...>` outside `src/platform/` fails to compile (see
*The fence* below).

## Interfaces

| Header | What |
|--------|------|
| `window_system.h` | `WindowSystem`, the backend: creates windows and the event loop, pumps events, lists the Vulkan instance extensions a surface needs, and owns the services below. `selectWindowSystem(Sdl\|Drm\|Wayland)` picks one before the first window; `selectDesktopWindowSystem()` applies the desktop policy (Wayland on a Wayland session, else SDL). `windowSystem()` returns the active one and defaults to SDL. |
| `window.h` | `Window` (size and position, style, state, displays, icon, `createVulkanSurface`, `presentPixels` for software frames, `nativeHandle()`), plus `WindowConfig` and `createWindow()`. Each window owns its `TextInput` (IME on/off, candidate area) and `Cursor` (shape, relative mode, warp). |
| `event_loop.h` | `EventLoop`: polls the backend and calls bro's handlers (`onKeyDown`, `onMouseMove`, `onResize`, ...) with bro's own types. Every event carries a window id. `setModalWindowEventHook` keeps timers alive while an OS modal loop (a Win32 live resize) owns the thread. |
| `keys.h` | The **one** key model: `Scancode` (a physical key, USB HID usage), `Keycode` (a code point, or `kScancodeMask \| scancode` for keys without one), `KeyMods`, named in `sc::`, `kc::` and `kmod::`. `defaultKeyFromScancode` / `defaultScancodeFromKey` give the US layout. |
| `keyboard.h` | `Keyboard`: modifier state and the active layout (`eventKeycode`, `layoutKeycode`, `scancodeFromKey`). |
| `clipboard.h` | `Clipboard`: text, typed data by MIME type, and the X11/Wayland primary selection where the OS has one. |
| `displays.h` | `Displays`: the attached displays (bounds, work area, refresh rate, scale) and their fullscreen modes. |
| `dialogs.h` | `Dialogs` (policy: message boxes, file pickers, headless answers) over a backend's `DialogBackend`. |
| `system_info.h` | `SystemInfo`: power/battery, light or dark theme, opening a URL in the OS handler. |
| `gamepads.h` | `Gamepads`: open, close, name and rumble by instance id. Device events arrive through `EventLoop`. |
| `wheel.h` | Wheel-detent-to-pixel helpers shared by every input path. |
| `evdev_keymap.h` | Linux evdev codes → `Scancode` / mouse buttons (libinput, bro.remote). |
| `desktop_*.h` | Desktop integration (taskbar progress, notifications, tray, global hotkeys, single instance, bell). These take a `const Window*` and use `Window::nativeHandle()` where they need the OS window. |

## The key model

Every backend produces the same numbers, so the key tables (DOM `key`/`code`
in `engine/key_mapping.cpp`, the terminal's encoder, hotkeys) exist once.
Scancodes are USB HID usages, which evdev, Win32 and macOS keycodes all
translate to by table. Keycodes are what the key types unshifted in the active
layout. Both use the same numbering as SDL3 (static_asserted in
`sdl/sdl_event_loop.cpp`), which made the SDL backend a pass-through and the
switch a rename. Nothing else ties the model to SDL. A Wayland backend maps
xkbcommon keysyms to it, and a Win32 backend maps VK/scan codes.

## Backends

- **SDL** (`sdl/`): every interface on Windows, macOS and Linux desktops.
- **DRM** (`drm_window_system.cpp`, plus `drm_seat` / `drm_input`): bro is the
  display server. It presents through KMS and reads input through libinput,
  so it has no windows and no `EventLoop`. The keyboard is the US layout, the
  clipboard belongs to the process, and there are no native dialogs and no
  desktop display list (the engine reads KMS outputs through `drm_seat`).
  `--drm` never starts SDL video.

- **Wayland** (`wayland/`, Linux): bro as a Wayland client of its own, with
  the protocol code in [browl](https://github.com/wlejon/browl) and only the
  adapter here. The engine's windowed mode calls `selectDesktopWindowSystem()`,
  which picks it when `$WAYLAND_DISPLAY` is set and the compositor offers
  xdg-shell and xdg-decoration (without the latter a window would need
  client-side decorations, which SDL draws through libdecor and this backend
  does not), and falls back to SDL otherwise. `BRO_WINDOW_SYSTEM=wayland|sdl`
  forces either. What it does that SDL's Wayland backend did not:
  - **Scale**: the surface's preferred scale (fractional scale, else the
    preferred buffer scale) is the window's pixel density, and wp_viewporter
    shows the drawable at the logical size, so the engine renders at 1.5x on
    a 150% output instead of being upscaled.
  - **Presentation time**: the swapchain's present asks wp_presentation when
    the commit turned to light (`Window::beforePresent` /
    `takePresentedFrames`). The frame recorder gets real vblank stamps for
    windowed apps, and the engine's animation clock advances to the expected
    vblank rather than to the frame start.
  - **Activation**: the first window spends `$XDG_ACTIVATION_TOKEN`, and a
    single-instance hand-off forwards the second launch's token to the
    running instance (`WindowSystem::setActivationToken`), which raises with
    it; without one it asks xdg-activation for a token of its own.
  - **Input**: text-input-v3 for IME composition, cursor-shape-v1 (else the
    XCursor theme), pointer constraints + relative pointer for pointer lock,
    the data device and primary selection, drops of files and text, touch.
    Key repeat and the left/right modifier bits are generated here as SDL
    does, and key events carry the same Keycodes SDL3 reports with its default
    keycode options (`wayland_keys.cpp`).
  - Gamepads, power, URL opening and native dialogs stay SDL's, used as a
    library with no video. The light/dark theme is the desktop portal's
    `color-scheme` (where SDL read it), polled on a thread.
  - A window the compositor hides (minimized, another workspace) is
    "suspended", which reads as minimized: the page is hidden and the
    swapchain stops presenting, where a FIFO present would block until the
    window is shown again (and with it timers and a single-instance raise).
  - What Wayland does not let a client do is not emulated: windows are not
    placed (`setPosition` is a no-op, positions read 0,0), opacity and
    always-on-top only read back, and the work area is the whole output.

Headless uses the SDL backend with a hidden window, as before.

## What still uses SDL directly, and why

All of it is inside `src/platform/sdl/`:

- **Gamepads.** SDL's controller database is the reason to use SDL at all. It
  stays behind `Gamepads`. The DRM backend uses it too, because the gamepad
  subsystem does not need SDL video.
- **Power, theme, opening URLs.** Under DRM, `SystemInfo` is the SDL one,
  which uses SDL as a library with no video.
- **Audio** belongs to broaudio, a sibling library with its own
  backend-neutral device layer (`broaudio/device.h`): native PipeWire on
  Linux when a daemon is reachable, SDL audio on Windows and macOS and as
  the Linux fallback. SDL is private to broaudio there too (linked
  PRIVATE, named by no public header), so it does not put SDL's include
  directory on bro's targets.

## The fence

`cmake/sdl_fence.cmake` generates `build/bro_sdl_fence/SDL3/*.h`. Each file
shadows one SDL header and contains only an `#error` that points here.
`src/CMakeLists.txt` puts that directory first on the include path of every
target under `src/`, and `src/platform/CMakeLists.txt` removes it again for
this directory. A stray `#include <SDL3/SDL.h>` in engine code therefore fails
the build with a message, and does not quietly compile.
