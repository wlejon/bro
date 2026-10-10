# src/platform

Everything bro needs from the OS window system goes through the
backend-neutral interfaces in this directory (`window_system.h`, `window.h`,
`event_loop.h`, `keys.h`, `keyboard.h`, `clipboard.h`, `displays.h`,
`dialogs.h`, `system_info.h`, `gamepads.h`, `desktop_*.h`). The rest of bro
names only those. **SDL may not be included outside `src/platform/`.**

## The fence

`cmake/sdl_fence.cmake` generates `build/bro_sdl_fence/SDL3/*.h`, each an
`#error` pointing here, and puts that directory first on the include path of
every target under `src/` except this one. An `#include <SDL3/...>` in engine
code fails the build. Use the interfaces; add to them if something is missing.

## Keys

One key model for every backend: `Scancode` is a USB HID usage, `Keycode` a
code point or `kScancodeMask | scancode`, both numbered as SDL3 numbers them
(static_asserted in `sdl/sdl_event_loop.cpp`). Key tables (`engine/key_mapping.cpp`,
the terminal encoder, hotkeys) exist once.

## Backends

- **SDL** (`sdl/`): Windows, macOS, Linux desktops, and headless (a hidden window).
  Also used without video under the other backends for gamepads, power, URL
  opening and native dialogs. Audio is broaudio's own (PipeWire on Linux, SDL
  elsewhere and as fallback).
- **DRM** (`drm_*`): bro is the display server: KMS presentation, libinput
  input, no windows and no `EventLoop`. US keyboard layout, process-local
  clipboard, no native dialogs. `--drm` never starts SDL video.
- **Wayland** (`wayland/`, Linux): bro's own client, protocol code in
  [browl](https://github.com/wlejon/browl). Chosen when `$WAYLAND_DISPLAY` is set
  and the compositor offers xdg-shell and xdg-decoration, else SDL.
  `BRO_WINDOW_SYSTEM=wayland|sdl` forces either. Over SDL it adds fractional
  scale (renders at 1.5x on a 150% output), wp_presentation vblank stamps,
  xdg-activation (`$XDG_ACTIVATION_TOKEN`, forwarded on a single-instance
  hand-off), text-input-v3 IME, cursor-shape, pointer lock and touch. A hidden
  window (minimized, another workspace) reads as minimized and stops presenting.
  Not emulated: `setPosition` is a no-op and positions read 0,0, opacity and
  always-on-top only read back, and the work area is the whole output.
