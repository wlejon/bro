# Screen capture: a PNG of what is actually on screen

For verifying a desktop running under `bro --drm`: client windows, the shell
document and its overlays, exactly as composited and scanned out. The capture
reads the KMS scanout buffer of the last composited frame back to the CPU, so
it shows what the display was given, not a re-render.

## From the shell (or a test)

```js
bro.window.captureScreen('/tmp/desktop.png');   // -> '/tmp/desktop.png', or null
bro.window.captureScreen();                     // -> '$XDG_RUNTIME_DIR/bro-screen.png'
```

Synchronous. Outside DRM (a window, headless) it captures the app's own
composite (the same frame `screenshot()` writes headless).

## From outside the process

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

## Limits

- Nothing before the first composited frame.
- While a fullscreen client buffer is scanned out directly (no composite), the
  composited slot is not what is on screen, so the capture fails and says so.
- Scanout is XRGB: the PNG is opaque.
