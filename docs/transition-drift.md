# QuickJS → bronze transition: behavioral drift log

Running list of places where the bronze-era rewrite diverged from the QuickJS
logic that worked. Oracle = the last QuickJS tree, checked out at
`../bro-quickjs-oracle` (commit `aea453b6`, the parent of the removal commit
`7448f830`); a matching pre-transition **binary** still exists at
`build-firsttime/Release/bro-headless.exe` (Aug 24, full profile).

Tooling: `scripts/binding-audit/` — `surface_dump.js` (runtime member dump
that runs on both engines), `gen_probe.mjs` (name-driven probe of the old
surface against the new runtime), `surface_diff.mjs` (report),
`handler_shape.mjs` (static old-vs-new handler body comparison, no build
needed; see section H).

**Evidence key** (how the row was established): **confirmed** = reproduced or
read in both trees · **survey** = reported by a subagent read, spot-checked
where noted.

**Status key** (where the row stands now):

- **fixed (`<commit>`)** — repaired, and the fix was read back in the current
  source. A commit with no prefix is in this repo; a sibling's is prefixed with
  its repo name (`broaudio 0a35f1d`). Several rows are fixed by the very commit
  that added this log (`0ec18a85`), which both recorded and repaired them.
- **fixed (uncommitted, this chunk)** — repaired in the working tree right now
  by a parallel agent; no commit to cite yet.
- **kept-new (<why>)** — the bronze behavior was deliberately kept; the row
  stays so nobody re-opens it.
- **open** — still broken.

Status pass of 2026-09-19 (runtime probes against `build/Release/bro-headless.exe`
built at 16:17, plus source reads in bro and every sibling working tree):
**64 fixed · 2 kept-new · 3 open**. The three open rows are C1, C3 and C4 (bronze runtime contract).

## A. Engine glue reimplemented instead of routed

| # | Where | Old behavior | New behavior | Effect | Evidence | Status |
|---|-------|--------------|--------------|--------|----------|--------|
| A1 | `src/engine/engine_init.cpp` `Engine::createCanvasContext` (commit `7056ee3f`) | `getContext('2d')` factory registered the scene via `addCanvasScene` → `init(gl_)` + `bindRasterThread` (windowed) / `setGrContext` (headless) | copies the factory body, ends with `init(nullptr)` + direct `canvasScenes_.push_back`; only `Engine::run()`'s one-time sweep binds scenes that exist at boot | every 2D canvas whose `getContext` runs after boot never rasterizes in windowed mode (blank); headless unaffected, so tests pass | confirmed (code); windowed pixels not captured | fixed (`0ec18a85`) — `engine_init.cpp:546` routes through `addCanvasScene` again |

## B. Sibling `<name>_api` ports transcribed from docs instead of the old C++

Root pattern: the port followed `docs/<name>-api.js`; commit `c4652b13` then
regenerated the docs from the port, so the docs now bless the regression.
`broworkshop/bin/docs/*.js` is a pre-transition copy of the docs.

Every row below is repaired by one broaudio commit, **`broaudio 0a35f1d`**
("api: restore QuickJS-era binding semantics dropped in the bronze port"),
which also adds `tests/test_audio_api_drift.cpp` covering each row.
`broaudio dc31cd6` is its follow-up (`connect`/`disconnect` stay one method on
`AudioNode.prototype` and dispatch by node kind). The docs half of the root
pattern is being repaired separately: `docs/audio-api.js` in `0ec18a85`, and
the tensor / vision / mesh / image / diffusion / ai-game docs in this chunk.

| # | Method | Old behavior | New behavior | Evidence | Status |
|---|--------|--------------|--------------|----------|--------|
| B1 | `AudioContext#playClip` | 4th numeric arg `when` → `playClipAt` (commit `884cc50a`) | 3 args, 4th ignored; streaming chunks all start immediately | confirmed (runtime: old `length` 4, new 3) | fixed (`broaudio 0a35f1d`) — `host_audio_clips.cpp:99` |
| B2 | `AudioContext#createClip` | 3rd arg `srcRate` resamples to the engine rate | reads 2 args | confirmed (code) | fixed (`broaudio 0a35f1d`) — `host_audio_clips.cpp:60-64` |
| B3 | broaudio file paths (`decodeAudioFile`, `createClipFromFile`, `createStreamFromFile`, `saveWav`, presets) | resolved through app dir + mount table (`resolveAssetPath`) | passed verbatim; broaudio has no `setPathResolver`, bro installs none (bromesh/brosoundml do) | confirmed (code) | fixed (`broaudio 0a35f1d` + `0ec18a85`) — `broaudio/src/api/api.h:41` declares `setPathResolver`, `host_sibling_apis.cpp:257` installs it |
| B4 | `createClipFromFileAsync` | decoded on a worker, promise settled later | decodes synchronously on the JS thread | survey | fixed (`broaudio 0a35f1d`) — `host_audio_clips.cpp:174-179` |
| B5 | `createStream(channels, ringFrames)` | `ringFrames` default 0 (engine picks) | default 44100 | survey | fixed (`broaudio 0a35f1d`) — `host_audio_playback.cpp:148-155` |
| B6 | `createStreamFromFile` opts | `ringFrames, prebufferFrames, loop, gain` | `prebufferFrames`, `gain` dropped; strict type checks | survey | fixed (`broaudio 0a35f1d`) — `host_audio_playback.cpp:177-207` |
| B7 | `getClipWaveform` | always a `Float32Array(numBins*2)` | `null` when empty | survey | fixed (`broaudio 0a35f1d`) — `host_audio_playback.cpp:19-23` |
| B8 | `renderBlock(n, out?)` | returns the mono mixdown, optional in-place buffer | 1 arg, returns undefined | survey | fixed (`broaudio 0a35f1d`) — `host_audio_synth_ext.cpp:92-98` |
| B9 | `createWavetable(typeString)` | `"saw"/"square"/"triangle"` → id | expects `(Float32Array, sampleRate)`; string → -1 | survey (runtime `length` 1 → 2) | fixed (`broaudio 0a35f1d`) — `host_audio_synth_ext.cpp:32-34` |
| B10 | `createWavetableFromWaveform` | engine sample rate | `(Float32Array, count, sampleRate)` default 44100 | survey (runtime `length` 1 → 3) | fixed (`broaudio 0a35f1d`) — `host_audio_synth_ext.cpp:47-49` |
| B11 | `setVoiceWavetable` | also set the voice waveform to Wavetable | only sets the bank | survey | fixed (`broaudio 0a35f1d`) — `host_audio_synth_ext.cpp` sets `Waveform::Wavetable` again |
| B12 | `*PresetToJson` / `apply*Preset` | took JS preset objects | take JSON strings; an object yields defaults | survey | fixed (`broaudio 0a35f1d`) — `host_audio_presets.cpp:424-448` (`jsonArg` accepts either) |
| B13 | `voicePresetFromJson`, `busPresetFromJson`, `modPresetFromJson`, `enginePresetFromJson` | present | not registered | confirmed (runtime dump) | fixed (`broaudio 0a35f1d`) — `host_audio_presets.cpp:455-469` |
| B14 | `setChorus*` (6), `setCompressor*` (5), `setBusChorusBaseDelay`, `setPlaybackSend` | present | not registered | confirmed (runtime dump) | fixed (`broaudio 0a35f1d`) — `host_audio_context.cpp:493-523` and on |
| B15 | `pushStreamSamples`, `saveWav`, `createMediaStreamSource`, `createSequence` | threw TypeError on wrong arg | silent 0/false/null-allocator | survey | fixed (`broaudio 0a35f1d`) — TypeErrors restored across `host_audio_clips.cpp` / `host_audio_playback.cpp` |
| B16 | `setBusEffectOrder` | capped at 7, unknown name = identity | uncapped, unknown name = Filter | survey | fixed (`broaudio 0a35f1d`) — `host_audio_bus_fx.cpp:138-143` |
| B17 | `getSpectrum` | rejected numBins > 8192 | unbounded | survey | fixed (`broaudio 0a35f1d`) — `host_audio_synth_ext.cpp:82-84` |

## C. Runtime contract changes (bronze itself)

The only section with open rows. bro pins bronze at `8bf92b7` (bronze HEAD),
so these were probed against the real runtime, not inferred.

| # | What | Old | New | Effect | Evidence | Status |
|---|------|-----|-----|--------|----------|--------|
| C1 | Module identity between the page's `<script type="module">` and a headless driver script | one module map per context: a test's `import "/app/lib/x.js"` got the page's instance | `evalScriptFileJit` compiles the driver as its own unit; `/app/...` modules evaluate a second time | 62 broworkshop tests that import `/app/` observe their own copy, not the app | confirmed (repro: `counter.js` evaluated twice, different ids) | **open** — `eval_jit.cpp:194-217` still hands `bronze::eval::evalFile` its own `EvalOptions`; nothing shares the page's module map |
| C2 | `performance.now()` under headless `advanceTime` | — | virtual; the smokes' "wait 8 s wall" loops return instantly | tests never actually waited for async `onReady` | confirmed | kept-new (virtual time *is* the headless contract — `host_performance.cpp:5-13` puts User Timing on the same clock on purpose, so a `measure` across `advanceTime` answers the virtual span; tests must use `Date.now()`) |
| C3 | `Object.getPrototypeOf(Atomics)` | `Object.prototype` | `bronze::fatal` in `runtime::objectGetPrototypeOf` (process abort, exit 3) | any reflective walk over globals kills the process | confirmed (backtrace in probe run) | **open** — still aborts, with a new message: `internal: a plain object whose root shape names no prototype` (`builtin_object.cpp:333`). bronze's real-prototype work (`bronze 88b9aa2`, `43e3fa6`, `77475bd`) closed the sibling `unsupported:` branch but not this one |
| C4 | `Object.getOwnPropertyNames(globalThis)` / builtin prototypes | lists host globals and `String.prototype` methods | host globals and builtin members resolve by name but do not enumerate (`String.prototype` enumerates 1 name) | feature-detection code that enumerates breaks; `globalThis['advanceTime']` is `undefined` while bare `advanceTime` works | confirmed | **open** (partly improved) — `globalThis` now enumerates 580 names including `document` and `bro`, but the headless driver globals still do not (`n.includes('advanceTime')` is `false`, `globalThis['advanceTime']` is `undefined` while bare `advanceTime` is a function) and `String.prototype` still enumerates 1 name |

## E. Web-platform surface gaps (runtime-confirmed on both binaries, 2026-09-19)

Found by `scripts/binding-audit` (old = `build-firsttime`, new = `build/Release`);
each row was re-checked with a direct `-e` probe on both.

Every row is repaired by **`0ec18a85`** and was re-probed on the current
binary; the probe readings are quoted in the Status column.

| # | Surface | Old | New | Evidence | Status |
|---|---------|-----|-----|----------|--------|
| E1 | Element: `isConnected`, `innerText` (get), `outerHTML`, `scrollWidth`, `clientLeft`, `clientTop`, `getClientRects()`, `scrollBy()`, `slot`, `assignedSlot`/`assignedNodes`/`assignedElements` | present on every element | `undefined` on every element | confirmed | fixed (`0ec18a85`, `c6b0a728`) — probe: all ten answer (`host_element_geometry.cpp`) |
| E2 | `<form>`: `elements`, `submit()`, `reset()`, `requestSubmit()`; `<input>`: `form`, `maxLength`, `minLength`, `pattern`, `size`; `<img>`: `decode()` | present | `undefined` | confirmed | fixed (`0ec18a85`, `c6b0a728`) — probe: all present (`host_element_forms.cpp:230-302`, `host_element_image.cpp:143`) |
| E3 | `bro.<ns>.available` for tts, lm, stt, diar, net, ai, gesture, gizmo, impostor, kws, listen, motion, rave, sense, triposplat, vision, wake, diffusion | `true` when compiled in | `undefined` (only `flora`, `gpu`, `tensor`, `steam`, `media` kept it) | confirmed; workshop gates on `bro.net.available`, `bro.motion.available` | fixed (`0ec18a85`) — `host_bro_root.cpp:331-333`; probe: `net`/`vision`/`lm` all `true` |
| E4 | `bro.image.*` CPU kernels: 24 functions (`resizeU8`, `cropU8`, `padU8`, `letterboxU8`, `flip*U8`, `rotate90U8`, `hwcToChw`, `chwToHwc`, `nhwcToNchwF32`, `u8NhwcToF32Nchw`, `normalizeNchw`, `decodeF32/U16/Oriented`, `probeDimensions`, `readExifOrientation`, `applyColorMatrix3x3/3x4`, `premultiplyAlpha`, `stencilHwc`, `accumulateTile`, ...) and `bro.image.presets` | 61 keys | 37 keys | confirmed (runtime) | fixed (`broimage 23c3d35`) — probe: **73 keys**, `presets` an object, `resizeU8`/`letterboxU8`/`accumulateTile` all functions |
| E5 | `performance.mark/measure/getEntries*/clearMarks/clearMeasures`, `performance.timeOrigin` | present | `undefined` | confirmed | fixed (`0ec18a85`) — new `host_performance.cpp`; probe: `mark` a function, `timeOrigin` a number |
| E6 | `navigator.hardwareConcurrency/languages/mediaDevices/onLine/cookieEnabled/product/productSub/vendor` | present | `undefined` | confirmed | fixed (`0ec18a85`) — `host_navigator.cpp:124-167`; probe: `hardwareConcurrency` = 32 |
| E7 | `document.hidden/visibilityState/location/URL/documentURI/implementation`, `document.createEvent()` | present | `undefined` | confirmed | fixed (`0ec18a85`) — `dom_document.cpp:280-323`; probe: `document.hidden` = `false`, `createEvent` a function |
| E8 | `DataTransfer`, `CanvasRenderingContext2D`, `AudioDestinationNode`, `SVGElement`, `MathMLElement` globals; `HTMLDetailsElement` and ~40 other per-tag `HTML*Element` constructors | constructible / usable for `instanceof` | not defined | confirmed | fixed (`0ec18a85`) — `host_html_interfaces.cpp:68-272`; probe: `DataTransfer` and `HTMLDetailsElement` are functions |
| E9 | `new TouchEvent(type)` | constructible | throws "not constructible" | confirmed | fixed (`c6b0a728`) — `host_touch.cpp:16`; probe: constructs |
| E10 | `DOMException.<CODE>_ERR` constants (25) | present | `undefined` | confirmed | fixed (`c6b0a728`) — `js/events.js` (see `host_js_modules.cpp:94`); probe: `NOT_FOUND_ERR` = 8 |
| E11 | `requestIdleCallback`/`cancelIdleCallback`, `screenX`/`screenY` | present | `undefined` | confirmed | fixed (`0ec18a85`) — `host_timers.cpp:120-348`, `host_platform.cpp:302`; probe: both present |
| E12 | `Intl.DateTimeFormat#formatToParts`, `File#path` | present | `undefined` | confirmed | fixed (`c6b0a728`) — `host_intl.cpp:281`; probe: `formatToParts` a function |
| E13 | `AudioContext#playClip` 4→3, `createClip` 1→3, `createWavetable` 1→2, `createWavetableFromWaveform` 1→3, `decodeAudioData` 1→3 (arity) | | | see B | fixed (`broaudio 0a35f1d`) — see B1, B2, B9, B10 |

Prototype/instance placement moved (members are now own properties built per
object) — that is why `global.Class#member` probes read as missing; the
`inst.` rows above are the authoritative ones.

## F. DOM element handlers: engine side-effects dropped around the DOM call (survey, 2026-09-19)

Subagent read of old `src/js/element_bindings.cpp` / `shadowroot_bindings.cpp` /
`custom_elements.cpp` vs new `src/bronze_host/host_element*.cpp`,
`host_node.cpp`, `host_shadow_dom.cpp`, `host_custom_elements.cpp`. Pattern:
the new handler is a thin DOM call transcribed from the API shape; the old body
did engine work *around* the call (dirty marking, clamping, event firing,
stylesheet registration, activation behaviour). Note `ev::isObject(null)` is
false in bronze, so every `!isObject` guard lets `null` through as `"null"`.

Every row is repaired by **`c6b0a728`** ("dom+host: restore the element
semantics the bronze port dropped around its DOM calls"), which also adds
twelve headless tests — one per behaviour — under `tests/dom/`, `tests/events/`,
`tests/layout/`, `tests/intl/`, `tests/video/` and `tests/custom_elements/`.

| # | Member | Old | New | Evidence | Status |
|---|--------|-----|-----|----------|--------|
| F1 | `textContent = null` | `""` (`:377`) | literal `"null"` (`host_element.cpp:497`) | survey | fixed (`c6b0a728`) — `host_element.cpp:504` treats it as nullable DOMString; `tests/dom/test_drift_null_coercion.js` |
| F2 | `textContent=` / `innerHTML=` with element children | detached children, wrappers stay live (`:336-358`) | `Element::setTextContent` frees every child, wrappers go inert | survey; breaks jQuery `buildFragment` re-parenting | fixed (`c6b0a728`) — `dom/document_nodes.cpp:169` detaches instead of freeing; `tests/dom/test_drift_detached_children.js` |
| F3 | `textContent=` into an overflow container | `setScrollToBottom(true)` (`:403-414`) | no caller of `setScrollToBottom(true)` in `src/` though `engine_frame_render.cpp:56` still consumes it | survey; log panels stop following | fixed (`c6b0a728`) — `dom/element_serialize.cpp:88` |
| F4 | `scrollTop = v`, `scrollTo()` | clamp, deferred-to-bottom when layout pending, `markDirty`, trusted `scroll` event (`:3505-3543`) | bare field write (`host_element.cpp:856-892`) | survey | fixed (`c6b0a728`) — `host_element_geometry.cpp:116`; `tests/layout/test_drift_scroll_metrics.js` |
| F5 | `clientWidth/Height`, `scrollHeight` | content+padding, inline → 0, attr fallback (`:3389-3405`, `:3473-3488`) | `contentRect` only, no padding, no inline rule (`:802-819`, `:869-879`) | survey | fixed (`c6b0a728`) — `host_element_geometry.cpp:181-331`; same test |
| F6 | `el.click()` | focus move; submit/reset buttons submit/reset the form; `<summary>` toggles `<details>`; label forwarding honours disabled `<fieldset>` (`:3856-3960`) | label→control, anchor download, radio/checkbox/file only (`host_element_forms.cpp:38-98`) | survey | fixed (`c6b0a728`) — `host_element_forms.cpp:436`; `tests/events/test_drift_click_activation.js` |
| F7 | `form.checkValidity()/reportValidity()` | form-wide over all controls, `invalid` per control (`:2437-2452`) | runs the element check on the `<form>` itself → always true (`host_element_forms.cpp:393-419`) | survey | fixed (`c6b0a728`) — new `host_element_validity.cpp:74-111`; `tests/dom/test_drift_form_api.js` |
| F8 | `validity` / `validationMessage` / `willValidate` | typeMismatch, badInput/range/step, tooShort/tooLong, radio-group/checkbox valueMissing (`:1995-2171`) | customError, required-empty, pattern only (`:421-467`) | survey | fixed (`c6b0a728`) — `host_element_validity.cpp:120` on; same test |
| F9 | `href/download/target/rel` on non-link tags | expando keyed by tag, value preserved (`:1576-1646`) | reflected to attribute; objects dropped (`:265-295`) | survey | fixed (`c6b0a728`) — `host_element_forms.cpp:42-67` (expando again) |
| F10 | `draggable` default | true for `<img>` and `<a href>` | `<img>` only (`:252`) | survey | fixed (`c6b0a728`) — `host_element_forms.cpp:177-185` |
| F11 | `getRootNode({composed:true})` | crosses shadow boundary (`:715-741`) | option ignored (`host_node.cpp:393-407`) | survey | fixed (`c6b0a728`) — `host_node.cpp:397-405` |
| F12 | `video.setAttribute('src', url)` | triggered `videoControl()->load` (`:2548-2556`) | only the `.src=` accessor loads (`host_element_video.cpp:134-142`) | survey | fixed (`c6b0a728`) — `dom/element.cpp:428`; `tests/video/test_drift_setattribute_src.js` |
| F13 | ShadowRoot `<style>` scoping | innerHTML/append/insert/remove registered sheets via `doc->addShadowStylesheet` + `markStructureDirty` (`shadowroot_bindings.cpp:78-205`) | `Document::addShadowStylesheet` has zero callers; appends go through generic `hostInsertNode` | survey; shadow CSS never reaches the cascade | fixed (`c6b0a728`) — `dom/shadow_root.cpp:143` calls it; `tests/dom/test_drift_shadow_surface.js` |
| F14 | `new MyElement()` for a defined custom element | resolved tag from `new_target`, created it (`custom_elements.cpp:58-89`) | throws `Illegal constructor` unless mid-upgrade (`host_custom_elements.cpp:220-232`) | survey | fixed (`c6b0a728`) — `host_custom_elements.cpp:235-261`; `tests/custom_elements/test_drift_direct_construct.js` |
| F15 | `dispatchEvent(obj)` | listeners receive the same object (`:3746-3752`) | copied through `EventSpec`; object `detail` is `JSON.stringify`'d (`host_event_spec.cpp:131-148`) | survey; functions/nodes in `detail` lost, identity broken | fixed (`c6b0a728`) — `host_dom_events.cpp:545-649`; `tests/events/test_drift_dispatch_identity.js` |
| F16 | `ShadowRoot.nodeName` | `"#document-fragment"` | `"#shadow-root"` (`host_shadow_dom.cpp:30`) | survey | fixed (`c6b0a728`) — `host_shadow_dom.cpp:37` |

Not drift there: `remove()`/`replaceWith()` no longer free the node (old made re-append impossible); `parentNode` returns non-element parents; `insertAdjacentHTML` upgrades custom elements; `addEventListener` dedups per spec.

## G. Canvas 2D: pixel-source layer rewritten without the old fallbacks (survey, 2026-09-19)

Context bodies (`host_canvas2d.cpp`) match the old `canvas_bindings.cpp`
closely; `js/image_gpu.js` is function-identical. What drifted is where
pixels come from and the element-level cache rule.

All rows but G7 are repaired by **`0ec18a85`**.

| # | Member | Old | New | Evidence | Status |
|---|--------|-----|-----|----------|--------|
| G1 | `getContext` second type on a live canvas | cache keyed by element, other type → `null` (`element_bindings.cpp:3095`) | `'webgl'` after `'2d'` creates both a CanvasScene and a WebGL context (`dom_canvas.cpp:162-201`) | survey; `getContext('webgl') \|\| getContext('2d')` double-backs the canvas | fixed (`0ec18a85`) — `dom_canvas.cpp:43-48` stores one `contextType` per canvas; a later other type answers `null` (`:168-175`) |
| G2 | `createImageBitmap(blob)` WebP / SVG | broimage → WebP fallback → SVG rasterize (`imagebitmap_bindings.cpp:97-121`) | broimage only (`host_imagebitmap.cpp:244-249`) | survey; rejects "Blob image decode failed" | fixed (`0ec18a85`) — `host_imagebitmap.cpp:243` runs the bitmap codecs, then WebP, then SVG |
| G3 | `drawImage(<img src=*.svg>)`, `createImageBitmap(<img>)` SVG | `rasterizeSvgMarkup` on demand (`image_bindings.cpp:1796-1800`) | `loadHostImage` has no SVG path (`host_image.cpp:123-145`) | survey; vector icons no-op / reject | fixed (`0ec18a85`) — `host_image.cpp:111-114` |
| G4 | `<img>` src resolution for canvas/bitmap | per-element `document()->basePath()` (`image_bindings.cpp:1768-1778`) | process-wide `util::resolveAssetPath` (`host_image.cpp:123`) | survey; wrong file inside system panels / iframes | fixed (`0ec18a85`) — `host_image.cpp:136-142` prefers the element's own `doc->basePath()` |
| G5 | `lineCap`/`lineJoin` unknown string | ignored (`canvas_bindings.cpp:473-498`) | resets to butt/miter (`host_canvas2d.cpp:116-138`) | survey | fixed (`0ec18a85`) — `host_canvas2d.cpp:116-127` keeps the value on an unknown string |
| G6 | `fillStyle` getter format | `rgba(r,g,b,1.00)` | `rgba(r,g,b,1)` / `0.501961` (`host_canvas2d.cpp:19-23`) | survey; string round-trips differ | fixed (`0ec18a85`) — `host_canvas2d.cpp:22` formats alpha `%.2f` again |
| G7 | `canvas.width/height` on a WebGL canvas | attribute or 300/150 | drawing-buffer size seeded from layout (`dom_canvas.cpp:51-59`) | survey; possibly deliberate | kept-new (a WebGL canvas answers its drawing-buffer size — `dom_canvas.cpp:78-87` — which is what a WebGL program reads and what the G8 zero-guard keeps stable; only the 2D bitmap follows the intrinsic size) |
| G8 | `canvas.width = 0` on a WebGL canvas | guarded `w > 0` (`element_bindings.cpp:3149`) | unconditional `resize` (`dom_canvas.cpp:81-153`) | survey | fixed (`0ec18a85`) — `dom_canvas.cpp:64-68` guards `w > 0` / `h > 0` again |
| G9 | `getLineDash()` after odd list | returns list as given; paint doubled | binding doubles before storing (`host_canvas2d.cpp:669-672`), paint doubles again | survey; getter only | fixed (`0ec18a85`) — `host_canvas2d.cpp:664-680` stores the list as given and lets `CanvasScene::applyStroke` do the doubling |

Same in both (do not chase): drawImage forms, arc/ellipse, putImageData, measureText, gradients, composite tables, toDataURL/toBlob; both ignore `fillText` maxWidth, fill/clip rules, `filter`; neither has `roundRect` or `createPattern`.

## H. Static handler-shape diff (`scripts/binding-audit/handler_shape.mjs`, 2026-09-19)

The script pairs every old registered method with its new handler by name
within the same sibling, and compares what the bodies *read* (option keys,
positional args, type dispatch, string enums) and *write* (result keys). It
reproduces every hand-confirmed audio row (B1, B2, B6, B8, B9, B13, B14, B16)
from text alone. Full output: `build/binding-audit/shape_all.txt` (300
differing pairs, 281 old names with no new handler). Rows below were then
verified by reading both bodies.

| # | Member | Old | New | Evidence | Status |
|---|--------|-----|-----|----------|--------|
| H1 | `bro.tensor.openSafetensors(path)` | opened the file; handle had `get(name, "compute"/"fp16", off, n)`, `names()`, `header()`, `close()` (`tensor_bindings_safetensors.cpp`) | `throw new Error("openSafetensors: cannot open " + path)` unconditionally (`brotensor/src/api/js/tensor.js:82-85`); `randn` likewise a stub | confirmed; safetensors loading from JS is gone | fixed (`brotensor 4f1a05f`) — `js/tensor.js:836-911` opens through the path resolver, with `get`/`names`/`header`/`close` plus a new `saveSafetensors`; `randn` and the RNG family are real |
| H2 | `GpuTensor#download(dst?)` | optional in-place `dst` Tensor (resized to fit) (`tensor_bindings.cpp:136-150`) | no argument (`tensor.js:795`) | confirmed | fixed (`brotensor 4f1a05f`) — `js/tensor.js:768-780` |
| H3 | `Physics.moveKinematic(tag,x,y,z,qx,qy,qz,qw,dt)` | 9-arg form carried rotation (`physics_bindings.cpp jsw_moveKinematic`, `argc >= 9`) | 5-arg `(tag,x,y,z,dt)` only (`js/physics.js:159`) | confirmed; kinematic bodies cannot rotate | fixed (`0ec18a85` + uncommitted, this chunk) — `js/physics.js:163` has the 9-arg form; the working tree removes the generated 5-arg override in `natives/physics/physics.js` so the hand-written one wins |
| H4 | `SceneNode#lookAt(x,y,z)` | scalar form and array form (`scene_bindings.cpp js_node_lookAt`) | `Float64Array.from(target)` — a number yields an empty array (`natives/scene/scene.js:283-285`) | confirmed | fixed — the compiled wrapper is `js/scene.js` (`:126-131` spreads three numbers into an array before `toF64`); `natives/scene/scene.js` is the brosurface template, which the build never compiles. `tests/scene/test_scene_node_extras.js` asserts the two forms agree. |
| H5 | `bro.net.disconnect(peer, reason)` | `reason` forwarded (`net_bindings.cpp:315-322`) | `reason` dropped (`js/net.js:64-67`) | confirmed | fixed (`0ec18a85`) — `js/net.js:68-70` forwards `reason` (0 when omitted); `docs/net-api.js` is being updated uncommitted in this chunk |
| H6 | `bro.window.getDisplays()` | `{bounds:{...}, workArea:{...}}` per display | flat `x/y/width/height/workX/...` (`natives/window/window.js:75-82`) | confirmed shape change; check callers | fixed (`8700efe8`) — `js/bro_core.js:75-99` reinstalls `getDisplays` carrying **both** the flat keys and the nested `bounds`/`workArea`; runtime probe confirms both on the live object |
| H7 | Names with no new handler, grep-confirmed absent: `document.createEvent`, `Event#initEvent`; SceneNode accessors `alphaCutoff/doubleSided/interior/priority/emissiveColor/fillColor/strokeColor/strokeWidth/billboard/...` + `setInstancedMesh/updateInstance(s)/setAtlasGrid`; `bro.mesh.computeTangents/simplifyWithAttributes/subdivideMidpoint/hasSelfIntersections/findSelfIntersections/intersectsMesh`; `bro.ai` NN class (`forward/backward/toArray/fromArray/copyFrom/sgdStep/adamStep/...`, 65 names), belief/grid/learn classes (~60 names); diffusion `sigmas/config/removeControlNet/clearControlNets/reloadTextEncoder/latent/setLatent` + control-vector and krea2 surfaces (36 names); `bro.vision.removeBackground/invert`; `rigging.addRigifySockets`; `net._sendUnframed` | | confirmed missing (see report for the full list) | fixed per group — `document.createEvent` / `Event#initEvent` (`0ec18a85`: `dom_document.cpp:280`, `js/events.js:40`); SceneNode accessors + instancing (`0ec18a85`: new `js/scene_extras.js`); mesh + `rigging.addRigifySockets` (`bromesh 3f09dfa`: `native_mesh_ops.cpp:51/136/188`, `native_mesh_analysis.cpp:280-300`, `native_rigging_core.cpp:477`); `bro.ai` NN / belief / grid / learn (`brogameagent 8c3dcc3`: `host_ai_nn*.cpp`, `host_ai_belief.cpp`, `host_ai_grid.cpp`, `host_ai_learn*.cpp`); diffusion (`brodiffusion 29f5982`: `native_diffusion_pipeline.cpp:434-435`, `native_diffusion_state.cpp:216-217`, `native_diffusion_control.cpp:141-187`, `native_diffusion_krea2.cpp`); vision (`brovisionml 0bc615e`: `native_vision_generative.cpp:334`). **Exception: `net._sendUnframed` is still absent** (oracle `net_bindings.cpp:298/404`) — no caller in `src/`, so it is the one name in this row left open |

Three of those groups moved while being restored; a probe looking under the
old spelling will still read `undefined`:

- the NN / belief / grid / learn classes hang off **`bro.ai.game.nn`**,
  `bro.ai.game.learn` and `bro.ai.game.grid`, not `bro.ai.*` (`bro.ai` itself
  has only `game` and `available`).
- `addRigifySockets` is on **`bro.rigging.Skeleton.prototype`**, not `Rig`
  (`bromesh/src/api/native_rigging_core.cpp:482`).
- BiRefNet is spelled **`bro.vision.Birefnet`**, and `invert` is a StyleGAN3
  method rather than a namespace function.

Static rows not yet verified (read both bodies before acting): tile
`distanceField`/`save`/`addObjectKind` args, terrain `setHeightSource`,
diffusion `stepOnce`/`decode` losing their options arg, StyleGAN3 `generate`
options, mesh `convexDecomposition` options, physics `setLayers`/`setLayer`
overloads, tokenizer `encode(text, addSpecial)` second arg, `menu.set` array
form. Known scanner noise: "result keys only in OLD" for handlers whose new
side is a brosurface JS wrapper (results come back through per-field natives),
and "OLD throws / NEW never throws" for the same reason.

*Status of that paragraph (spot-check, 2026-09-19):* these are leads, not rows.
Four resolve on a read and are **not drift**:

- diffusion `stepOnce`/`decode` — two different methods. `Pipeline.stepOnce(state)`
  genuinely takes no options and returns `hasMore`; the options live on
  `PipelineState` (`brodiffusion/src/api/native_diffusion_pipeline.cpp:347`,
  `:372-387`). `Pipeline.decode(state, opts)` *does* read `opts.includeFp32`
  (`:399`) despite a registered arity of 1.
- "StyleGAN3 `generate` options" — a scanner false positive against brodiffusion:
  StyleGAN3 lives in **brovisionml** (`native_vision_generative.cpp:419-437`),
  where `generate`/`synthesize` have their full option set. Note the real
  default `resolution` there is 256, not 1024.
- `Physics.setLayers` / `setLayer` (`js/physics.js:32`, `:150`) are present.
- `Terrain#setHeightSource` (`js/terrain.js:101`) is present.

The rest were read body-for-body and are not drift:
- tile `distanceField(sources, {blockMask, costs, conn})` returns Int32Array,
  or Float32Array when `costs` is given (`js/tile_world.js:276`), `save()`
  returns a Uint8Array, `addObjectKind(mesh, style)` — all as in
  `tile_bindings.cpp`.
- mesh `convexDecomposition` reads the same four keys (`maxHulls`,
  `maxVerticesPerHull`, `minVolumePerHull`, `resolution`) in both
  (`bromesh/src/api/native_mesh_ops.cpp:303`, oracle `mesh_bindings.cpp:2105`).
- tokenizer `encode(text, addSpecial=false)` is identical
  (`brolm/src/api/native_lm_tokenizer.cpp:88`, oracle `lm_bindings.cpp:504`).
- `bro.menu.set(array)` takes the array form (`host_menu.cpp:110`,
  `tests/engine/test_menu.js`). The one difference: a non-array argument now
  clears the menu instead of throwing `TypeError`; kept, since no app relies
  on the throw.

## I. Engine-side capabilities a standalone sibling cannot provide

The transition moved the model code out of `src/js/` and into sibling
libraries, which was the right move — but two things the old bindings did were
not *model* work at all. They were engine work done around the model call, and
a library that owns no DOM and no event loop simply cannot do them. The port
dropped both rather than leaving them behind in bro, and the siblings' own
comments say so ("a standalone sibling cannot mint one, so the pixel planes
come back as typed arrays and the caller rasterizes"). bro is that caller;
`src/bronze_host/host_vision*.cpp` is where it answers.

| # | What | Old | New | Effect | Evidence | Status |
|---|------|-----|-----|--------|----------|--------|
| I1 | `bro.vision` results carried ImageBitmaps (oracle `vision_bindings.cpp` `makeBitmap` / `makeMaskBitmap` / `makeGrayBitmap` / `makeBitmapRGB` / `makeNormalBitmap`) | `image` on every heavy op, plus `matte` on BiRefNet, as an `ImageBitmap` a program hands straight to `drawImage` / `texImage2D` / a ControlNet input | typed-array planes only (`data`, `matte`, `gray`, `edge`/`edges`, ...) | every caller that drew a depth map, a mask overlay or a cutout had to rasterize it in JS first | confirmed (both trees) | fixed (uncommitted, this chunk) — `host_vision_jobs.cpp` rasterizers, wired per op in `host_vision_ops.cpp` / `_sam.cpp` / `_generative.cpp` / `_annotators.cpp`; the typed-array planes all stay |
| I2 | Heavy vision ops ran on a worker thread (oracle `vision_bindings.cpp:367` `runVisionOp` over `async_job.cpp`) | `opts.onDone` → the model ran off the JS thread, the call returned an AsyncHandle with `cancel()`, the result was delivered on the JS thread from the engine's frame pump; a `busy` flag refused a second op on one model | every op synchronous inside the JS call | a 1024² BiRefNet or a SAM everything-sweep blocks the frame for as long as inference takes | confirmed (both trees) | fixed (uncommitted, this chunk) — `host_vision_jobs.cpp` `runVisionOp` + `tickVisionJobs` on `Engine::addFramePump`; sync and async share one compute and one builder, asserted by `tests/vision/test_vision_async.js` |

Not restored, and deliberately: `Dinov2`/`Dinov3` `encode()` stay synchronous
(they were async in the oracle, but they produce features rather than pixels
and no caller in `src/` or `broworkshop` drives them per frame).

## D. Not drift (noted so nobody chases them)

- Old `Element` was one mega-class: every element exposed video/img/form members. New per-tag objects dropping `play()` from a `<div>` is correct.
- bronze does not enumerate host globals or builtin prototype members via `Object.getOwnPropertyNames`, though `in`/`[]` resolve them; `String.prototype` enumerates 1 name. Enumeration-based diffs are noise; use the name-driven probe. (Partly superseded — see C4: `globalThis` does enumerate now.)
- `nodeName`/`tagName`/`nodeType` became data properties instead of getters.
