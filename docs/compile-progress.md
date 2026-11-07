# A responsive window while scripts compile

In a windowed run, a page script is compiled on a background thread while the
main thread keeps the window alive: events are pumped (the window can be
moved, resized and closed), and the page's own **static markup** — its HTML
and CSS, parsed and laid out before any script compiles — is drawn about 60
times a second. When the engine's splash panel is up it covers the window and
animates as before.

So an app's loading screen is ordinary markup and CSS in `index.html`. While a
compile runs, bro publishes its state on the document element:

| On `<html>` | Meaning |
|---|---|
| `bro-compiling` attribute | present while a page script compiles |
| `--bro-compile-progress` custom property | 0 to 1, an estimate weighted by what each compile phase typically costs |

Both are removed when the compile finishes, before the script runs.

```html
<style>
  #boot { display: none; }
  html[bro-compiling] #boot { display: flex; }
  #boot .bar { width: calc(var(--bro-compile-progress, 0) * 100%); }
</style>
<div id="boot"><div class="bar"></div></div>
<script type="module" src="js/app.js"></script>
```

The app's own boot code decides when the rest of its UI replaces the static
markup; the attribute only covers the compile. A warm launch served from the
code cache (docs/code-cache.md) skips most of the compile, so the loading
state may show for only a frame or two.

When the splash is up, it shows the same estimate itself (`compiling scripts
42%`), and after the script, the scene's pipeline work: `building
pipelines N` while graphics pipelines are built. That count comes from
`scene::vk::pipeline_async::inFlight()`, as `scene.pendingWork()` reports it.
The splash waits up to 4 s past the compile for the pipelines asked for so far
(the first frames'); work left after that, or
started later, shows in the corner status panel until it is done. See
docs/system-panels.md "Background work".

No script of the page runs from these frames: no timers, no
`requestAnimationFrame`, no input dispatch at boot. Headless compiles on the
same background thread with the same wait, but its pump runs nothing, so tests
stay deterministic. It never sets the attribute.

## While the page's script runs

The same pump (`util/page_pump.h`) keeps the window answering after the
compile, while the script's top level runs:

- **Blocking natives** do their work on another thread, or wait in 16 ms
  steps, and pump in between. These are kernel and hook compiles
  (`bro.kernel.compile`, `setShader`), the scene's built-in stages, GPU
  pipeline builds and GPU waits (`VulkanQueue::wait`, `KernelBuffer.read`).
  A compile's wait may draw the page's markup. A GPU wait only lets the OS
  process the window's messages, since the frame it is recording holds frame
  resources that a drawn frame would recycle.
- **A long stretch of script** is interrupted at its next loop backedge or
  call, through bronze's host interrupt (`bronze_host/script_pump.h`), once it
  has gone 16 ms without a pump. The interrupt lets the OS process the
  window's messages and returns. Messages are not dispatched to the page:
  they stay queued for the first frame's poll.

The watchdog runs from window creation until the first frame. Headless takes
the same paths, with a pump that only notes it ran.

The log's first-frame line gives the boot's longest stretch without pumping,
the stretch a window system judges (Windows calls a window hung after 5 s):

```
first frame 3120 ms after start (page loaded at 2950 ms); longest stretch without pumping events 41 ms, ending 1880 ms after the window
```
