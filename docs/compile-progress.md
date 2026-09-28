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

No script of the page runs from these frames: no timers, no
`requestAnimationFrame`, no input dispatch at boot. Headless runs compile on
the calling thread, which keeps tests deterministic, and never set the
attribute.

For an embedder, bronze exposes the pieces directly: `EvalOptions::onProgress`
reports `CompileProgress { phase, fraction }` from the compiling thread, and
`captureThreadInputs(options)` copies what a compile reads from the calling
thread's runtime (host globals, natives, the realm's module registry) so the
compile may run on any thread and still build the program that thread would
have (bronze `src/eval/eval.h`).

Where it lives: `compileWithPumping` in `src/bronze_host/eval_jit.cpp`, and
`Engine::pumpCompileFrame` / `setCompileProgress` in
`src/engine/engine_compile_frame.cpp`.
