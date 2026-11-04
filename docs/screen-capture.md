# Screen capture: what is actually on screen

Two forms of `bro.window.captureScreen` (reference: [window-api.js](window-api.js)):

- **The desktop**, `captureScreen({display, rect, path, pixels})`: every
  app's windows, from the OS, on every platform. Behind the `screen`
  capability. A Promise of the pixels (and a PNG when asked).
- **The frame bro shows**, `captureScreen(path)`: synchronous, a PNG. Under
  `bro --drm` that is the KMS scanout (client windows, the shell and its
  overlays); elsewhere the app's own composite.

## The desktop

```js
const displays = bro.window.captureDisplays();      // [{ index, name, x, y, width, height, scale, primary }]
const all = await bro.window.captureScreen({});     // the whole virtual desktop
const one = await bro.window.captureScreen({ display: 'primary' });
const part = await bro.window.captureScreen({ display: 1, rect: { x: 0, y: 0, width: 800, height: 600 },
                                              path: '/tmp/part.png', pixels: false });
// -> { x, y, width, height, display, backend, pixels: Uint8Array RGBA, path }
```

| Where | Backend | Notes |
|---|---|---|
| Windows | DXGI Desktop Duplication, per output (`dxgi`) | GDI `BitBlt` for an output it cannot duplicate (rotated, a remote session's adapter) (`gdi`). Physical pixels whatever the process's DPI awareness. Refused (`EPERM`) while the screen is locked: the secure desktop is no program's to read. |
| macOS 14+ | ScreenCaptureKit `SCScreenshotManager` | Needs the Screen Recording permission for the executable; without it the call is refused (`EPERM`, saying where to grant it) and macOS shows its prompt once. A program started over ssh is judged by sshd's permission. |
| Linux, Wayland | wlr-screencopy (`wlr-screencopy`), a connection of its own | helm, sway, Hyprland. GNOME and KDE do not offer it (`ENOTSUP`): their screenshots are the desktop portal's, which asks the user. |
| `bro --drm` | the KMS scanout (`kms`) | one display at 0,0 |

Coordinates are the virtual desktop's: Windows' own (physical pixels),
macOS's global display space times the largest display scale (a Retina
display at full resolution), each Wayland output's logical position times
the largest scale. The capture is opaque, with the pointer drawn in only for
`cursor: true` (not under DRM, where the pointer is a plane of its own). It shows what is
on screen, a screenshot app's own window included: hide it first.

Headless captures the real desktop as well (bro-headless on a desktop
machine); where there is none it is refused with `ENOTSUP` or `EPERM`.

## The frame bro shows

```js
bro.window.captureScreen('/tmp/desktop.png');   // -> '/tmp/desktop.png', or null
bro.window.captureScreen();                     // -> '$XDG_RUNTIME_DIR/bro-screen.png'
```

Synchronous. Under DRM it reads the KMS scanout buffer of the last
composited frame back to the CPU, so it shows what the display was given,
not a re-render. Outside DRM (a window, headless) it captures the app's own
composite (the same frame `screenshot()` writes headless).

### From outside the process (DRM)

No cooperation from the shell is needed. Either:

```sh
kill -USR2 $(pgrep -x helm)                       # -> $XDG_RUNTIME_DIR/bro-screen.png
echo /tmp/desktop.png > $XDG_RUNTIME_DIR/bro-capture  # -> /tmp/desktop.png
: > $XDG_RUNTIME_DIR/bro-capture                  # empty request -> the default path
```

The DRM frame loop picks the request up (the signal on the next frame, the
request file within about half a second), reads the scanout buffer between
frames and encodes the PNG on a background thread; `Screen captured to ...`
in the log says when the file is there. The startup log names the pid and both
paths (`Engine: screen capture: kill -USR2 ...`). Where `XDG_RUNTIME_DIR` is
unset, the system temp directory stands in for it.

### Limits (DRM)

- Nothing before the first composited frame.
- While a fullscreen client buffer is scanned out directly (no composite), the
  composited slot is not what is on screen, so the capture fails and says so.
- Scanout is XRGB: the PNG is opaque.
