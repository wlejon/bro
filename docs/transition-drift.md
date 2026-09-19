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

Status key: **confirmed** = reproduced or read in both trees · **survey** =
reported by a subagent read, spot-checked where noted · **open** = not yet
examined.

## A. Engine glue reimplemented instead of routed

| # | Where | Old behavior | New behavior | Effect | Status |
|---|-------|--------------|--------------|--------|--------|
| A1 | `src/engine/engine_init.cpp` `Engine::createCanvasContext` (commit `7056ee3f`) | `getContext('2d')` factory registered the scene via `addCanvasScene` → `init(gl_)` + `bindRasterThread` (windowed) / `setGrContext` (headless) | copies the factory body, ends with `init(nullptr)` + direct `canvasScenes_.push_back`; only `Engine::run()`'s one-time sweep binds scenes that exist at boot | every 2D canvas whose `getContext` runs after boot never rasterizes in windowed mode (blank); headless unaffected, so tests pass | confirmed (code); windowed pixels not captured |

## B. Sibling `<name>_api` ports transcribed from docs instead of the old C++

Root pattern: the port followed `docs/<name>-api.js`; commit `c4652b13` then
regenerated the docs from the port, so the docs now bless the regression.
`broworkshop/bin/docs/*.js` is a pre-transition copy of the docs.

| # | Method | Old behavior | New behavior | Status |
|---|--------|--------------|--------------|--------|
| B1 | `AudioContext#playClip` | 4th numeric arg `when` → `playClipAt` (commit `884cc50a`) | 3 args, 4th ignored; streaming chunks all start immediately | confirmed (runtime: old `length` 4, new 3) |
| B2 | `AudioContext#createClip` | 3rd arg `srcRate` resamples to the engine rate | reads 2 args | confirmed (code) |
| B3 | broaudio file paths (`decodeAudioFile`, `createClipFromFile`, `createStreamFromFile`, `saveWav`, presets) | resolved through app dir + mount table (`resolveAssetPath`) | passed verbatim; broaudio has no `setPathResolver`, bro installs none (bromesh/brosoundml do) | confirmed (code) |
| B4 | `createClipFromFileAsync` | decoded on a worker, promise settled later | decodes synchronously on the JS thread | survey |
| B5 | `createStream(channels, ringFrames)` | `ringFrames` default 0 (engine picks) | default 44100 | survey |
| B6 | `createStreamFromFile` opts | `ringFrames, prebufferFrames, loop, gain` | `prebufferFrames`, `gain` dropped; strict type checks | survey |
| B7 | `getClipWaveform` | always a `Float32Array(numBins*2)` | `null` when empty | survey |
| B8 | `renderBlock(n, out?)` | returns the mono mixdown, optional in-place buffer | 1 arg, returns undefined | survey |
| B9 | `createWavetable(typeString)` | `"saw"/"square"/"triangle"` → id | expects `(Float32Array, sampleRate)`; string → -1 | survey (runtime `length` 1 → 2) |
| B10 | `createWavetableFromWaveform` | engine sample rate | `(Float32Array, count, sampleRate)` default 44100 | survey (runtime `length` 1 → 3) |
| B11 | `setVoiceWavetable` | also set the voice waveform to Wavetable | only sets the bank | survey |
| B12 | `*PresetToJson` / `apply*Preset` | took JS preset objects | take JSON strings; an object yields defaults | survey |
| B13 | `voicePresetFromJson`, `busPresetFromJson`, `modPresetFromJson`, `enginePresetFromJson` | present | not registered | confirmed (runtime dump) |
| B14 | `setChorus*` (6), `setCompressor*` (5), `setBusChorusBaseDelay`, `setPlaybackSend` | present | not registered | confirmed (runtime dump) |
| B15 | `pushStreamSamples`, `saveWav`, `createMediaStreamSource`, `createSequence` | threw TypeError on wrong arg | silent 0/false/null-allocator | survey |
| B16 | `setBusEffectOrder` | capped at 7, unknown name = identity | uncapped, unknown name = Filter | survey |
| B17 | `getSpectrum` | rejected numBins > 8192 | unbounded | survey |

## C. Runtime contract changes (bronze itself)

| # | What | Old | New | Effect | Status |
|---|------|-----|-----|--------|--------|
| C1 | Module identity between the page's `<script type="module">` and a headless driver script | one module map per context: a test's `import "/app/lib/x.js"` got the page's instance | `evalScriptFileJit` compiles the driver as its own unit; `/app/...` modules evaluate a second time | 62 broworkshop tests that import `/app/` observe their own copy, not the app | confirmed (repro: `counter.js` evaluated twice, different ids) |
| C2 | `performance.now()` under headless `advanceTime` | — | virtual; the smokes' "wait 8 s wall" loops return instantly | tests never actually waited for async `onReady` | confirmed |
| C3 | `Object.getPrototypeOf(Atomics)` | `Object.prototype` | `bronze::fatal` in `runtime::objectGetPrototypeOf` (process abort, exit 3) | any reflective walk over globals kills the process | confirmed (backtrace in probe run) |
| C4 | `Object.getOwnPropertyNames(globalThis)` / builtin prototypes | lists host globals and `String.prototype` methods | host globals and builtin members resolve by name but do not enumerate (`String.prototype` enumerates 1 name) | feature-detection code that enumerates breaks; `globalThis['advanceTime']` is `undefined` while bare `advanceTime` works | confirmed |

## E. Web-platform surface gaps (runtime-confirmed on both binaries, 2026-09-19)

Found by `scripts/binding-audit` (old = `build-firsttime`, new = `build/Release`);
each row was re-checked with a direct `-e` probe on both.

| # | Surface | Old | New | Status |
|---|---------|-----|-----|--------|
| E1 | Element: `isConnected`, `innerText` (get), `outerHTML`, `scrollWidth`, `clientLeft`, `clientTop`, `getClientRects()`, `scrollBy()`, `slot`, `assignedSlot`/`assignedNodes`/`assignedElements` | present on every element | `undefined` on every element | confirmed |
| E2 | `<form>`: `elements`, `submit()`, `reset()`, `requestSubmit()`; `<input>`: `form`, `maxLength`, `minLength`, `pattern`, `size`; `<img>`: `decode()` | present | `undefined` | confirmed |
| E3 | `bro.<ns>.available` for tts, lm, stt, diar, net, ai, gesture, gizmo, impostor, kws, listen, motion, rave, sense, triposplat, vision, wake, diffusion | `true` when compiled in | `undefined` (only `flora`, `gpu`, `tensor`, `steam`, `media` kept it) | confirmed; workshop gates on `bro.net.available`, `bro.motion.available` |
| E4 | `bro.image.*` CPU kernels: 24 functions (`resizeU8`, `cropU8`, `padU8`, `letterboxU8`, `flip*U8`, `rotate90U8`, `hwcToChw`, `chwToHwc`, `nhwcToNchwF32`, `u8NhwcToF32Nchw`, `normalizeNchw`, `decodeF32/U16/Oriented`, `probeDimensions`, `readExifOrientation`, `applyColorMatrix3x3/3x4`, `premultiplyAlpha`, `stencilHwc`, `accumulateTile`, ...) and `bro.image.presets` | 61 keys | 37 keys | confirmed (runtime) |
| E5 | `performance.mark/measure/getEntries*/clearMarks/clearMeasures`, `performance.timeOrigin` | present | `undefined` | confirmed |
| E6 | `navigator.hardwareConcurrency/languages/mediaDevices/onLine/cookieEnabled/product/productSub/vendor` | present | `undefined` | confirmed |
| E7 | `document.hidden/visibilityState/location/URL/documentURI/implementation`, `document.createEvent()` | present | `undefined` | confirmed |
| E8 | `DataTransfer`, `CanvasRenderingContext2D`, `AudioDestinationNode`, `SVGElement`, `MathMLElement` globals; `HTMLDetailsElement` and ~40 other per-tag `HTML*Element` constructors | constructible / usable for `instanceof` | not defined | confirmed |
| E9 | `new TouchEvent(type)` | constructible | throws "not constructible" | confirmed |
| E10 | `DOMException.<CODE>_ERR` constants (25) | present | `undefined` | confirmed |
| E11 | `requestIdleCallback`/`cancelIdleCallback`, `screenX`/`screenY` | present | `undefined` | confirmed |
| E12 | `Intl.DateTimeFormat#formatToParts`, `File#path` | present | `undefined` | confirmed |
| E13 | `AudioContext#playClip` 4→3, `createClip` 1→3, `createWavetable` 1→2, `createWavetableFromWaveform` 1→3, `decodeAudioData` 1→3 (arity) | | | see B |

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

| # | Member | Old | New | Status |
|---|--------|-----|-----|--------|
| F1 | `textContent = null` | `""` (`:377`) | literal `"null"` (`host_element.cpp:497`) | survey |
| F2 | `textContent=` / `innerHTML=` with element children | detached children, wrappers stay live (`:336-358`) | `Element::setTextContent` frees every child, wrappers go inert | survey; breaks jQuery `buildFragment` re-parenting |
| F3 | `textContent=` into an overflow container | `setScrollToBottom(true)` (`:403-414`) | no caller of `setScrollToBottom(true)` in `src/` though `engine_frame_render.cpp:56` still consumes it | survey; log panels stop following |
| F4 | `scrollTop = v`, `scrollTo()` | clamp, deferred-to-bottom when layout pending, `markDirty`, trusted `scroll` event (`:3505-3543`) | bare field write (`host_element.cpp:856-892`) | survey |
| F5 | `clientWidth/Height`, `scrollHeight` | content+padding, inline → 0, attr fallback (`:3389-3405`, `:3473-3488`) | `contentRect` only, no padding, no inline rule (`:802-819`, `:869-879`) | survey |
| F6 | `el.click()` | focus move; submit/reset buttons submit/reset the form; `<summary>` toggles `<details>`; label forwarding honours disabled `<fieldset>` (`:3856-3960`) | label→control, anchor download, radio/checkbox/file only (`host_element_forms.cpp:38-98`) | survey |
| F7 | `form.checkValidity()/reportValidity()` | form-wide over all controls, `invalid` per control (`:2437-2452`) | runs the element check on the `<form>` itself → always true (`host_element_forms.cpp:393-419`) | survey |
| F8 | `validity` / `validationMessage` / `willValidate` | typeMismatch, badInput/range/step, tooShort/tooLong, radio-group/checkbox valueMissing (`:1995-2171`) | customError, required-empty, pattern only (`:421-467`) | survey |
| F9 | `href/download/target/rel` on non-link tags | expando keyed by tag, value preserved (`:1576-1646`) | reflected to attribute; objects dropped (`:265-295`) | survey |
| F10 | `draggable` default | true for `<img>` and `<a href>` | `<img>` only (`:252`) | survey |
| F11 | `getRootNode({composed:true})` | crosses shadow boundary (`:715-741`) | option ignored (`host_node.cpp:393-407`) | survey |
| F12 | `video.setAttribute('src', url)` | triggered `videoControl()->load` (`:2548-2556`) | only the `.src=` accessor loads (`host_element_video.cpp:134-142`) | survey |
| F13 | ShadowRoot `<style>` scoping | innerHTML/append/insert/remove registered sheets via `doc->addShadowStylesheet` + `markStructureDirty` (`shadowroot_bindings.cpp:78-205`) | `Document::addShadowStylesheet` has zero callers; appends go through generic `hostInsertNode` | survey; shadow CSS never reaches the cascade |
| F14 | `new MyElement()` for a defined custom element | resolved tag from `new_target`, created it (`custom_elements.cpp:58-89`) | throws `Illegal constructor` unless mid-upgrade (`host_custom_elements.cpp:220-232`) | survey |
| F15 | `dispatchEvent(obj)` | listeners receive the same object (`:3746-3752`) | copied through `EventSpec`; object `detail` is `JSON.stringify`'d (`host_event_spec.cpp:131-148`) | survey; functions/nodes in `detail` lost, identity broken |
| F16 | `ShadowRoot.nodeName` | `"#document-fragment"` | `"#shadow-root"` (`host_shadow_dom.cpp:30`) | survey |

Not drift there: `remove()`/`replaceWith()` no longer free the node (old made re-append impossible); `parentNode` returns non-element parents; `insertAdjacentHTML` upgrades custom elements; `addEventListener` dedups per spec.

## G. Canvas 2D: pixel-source layer rewritten without the old fallbacks (survey, 2026-09-19)

Context bodies (`host_canvas2d.cpp`) match the old `canvas_bindings.cpp`
closely; `js/image_gpu.js` is function-identical. What drifted is where
pixels come from and the element-level cache rule.

| # | Member | Old | New | Status |
|---|--------|-----|-----|--------|
| G1 | `getContext` second type on a live canvas | cache keyed by element, other type → `null` (`element_bindings.cpp:3095`) | `'webgl'` after `'2d'` creates both a CanvasScene and a WebGL context (`dom_canvas.cpp:162-201`) | survey; `getContext('webgl') \|\| getContext('2d')` double-backs the canvas |
| G2 | `createImageBitmap(blob)` WebP / SVG | broimage → WebP fallback → SVG rasterize (`imagebitmap_bindings.cpp:97-121`) | broimage only (`host_imagebitmap.cpp:244-249`) | survey; rejects "Blob image decode failed" |
| G3 | `drawImage(<img src=*.svg>)`, `createImageBitmap(<img>)` SVG | `rasterizeSvgMarkup` on demand (`image_bindings.cpp:1796-1800`) | `loadHostImage` has no SVG path (`host_image.cpp:123-145`) | survey; vector icons no-op / reject |
| G4 | `<img>` src resolution for canvas/bitmap | per-element `document()->basePath()` (`image_bindings.cpp:1768-1778`) | process-wide `util::resolveAssetPath` (`host_image.cpp:123`) | survey; wrong file inside system panels / iframes |
| G5 | `lineCap`/`lineJoin` unknown string | ignored (`canvas_bindings.cpp:473-498`) | resets to butt/miter (`host_canvas2d.cpp:116-138`) | survey |
| G6 | `fillStyle` getter format | `rgba(r,g,b,1.00)` | `rgba(r,g,b,1)` / `0.501961` (`host_canvas2d.cpp:19-23`) | survey; string round-trips differ |
| G7 | `canvas.width/height` on a WebGL canvas | attribute or 300/150 | drawing-buffer size seeded from layout (`dom_canvas.cpp:51-59`) | survey; possibly deliberate |
| G8 | `canvas.width = 0` on a WebGL canvas | guarded `w > 0` (`element_bindings.cpp:3149`) | unconditional `resize` (`dom_canvas.cpp:81-153`) | survey |
| G9 | `getLineDash()` after odd list | returns list as given; paint doubled | binding doubles before storing (`host_canvas2d.cpp:669-672`), paint doubles again | survey; getter only |

Same in both (do not chase): drawImage forms, arc/ellipse, putImageData, measureText, gradients, composite tables, toDataURL/toBlob; both ignore `fillText` maxWidth, fill/clip rules, `filter`; neither has `roundRect` or `createPattern`.

## H. Static handler-shape diff (`scripts/binding-audit/handler_shape.mjs`, 2026-09-19)

The script pairs every old registered method with its new handler by name
within the same sibling, and compares what the bodies *read* (option keys,
positional args, type dispatch, string enums) and *write* (result keys). It
reproduces every hand-confirmed audio row (B1, B2, B6, B8, B9, B13, B14, B16)
from text alone. Full output: `build/binding-audit/shape_all.txt` (300
differing pairs, 281 old names with no new handler). Rows below were then
verified by reading both bodies.

| # | Member | Old | New | Status |
|---|--------|-----|-----|--------|
| H1 | `bro.tensor.openSafetensors(path)` | opened the file; handle had `get(name, "compute"/"fp16", off, n)`, `names()`, `header()`, `close()` (`tensor_bindings_safetensors.cpp`) | `throw new Error("openSafetensors: cannot open " + path)` unconditionally (`brotensor/src/api/js/tensor.js:82-85`); `randn` likewise a stub | confirmed; safetensors loading from JS is gone |
| H2 | `GpuTensor#download(dst?)` | optional in-place `dst` Tensor (resized to fit) (`tensor_bindings.cpp:136-150`) | no argument (`tensor.js:795`) | confirmed |
| H3 | `Physics.moveKinematic(tag,x,y,z,qx,qy,qz,qw,dt)` | 9-arg form carried rotation (`physics_bindings.cpp jsw_moveKinematic`, `argc >= 9`) | 5-arg `(tag,x,y,z,dt)` only (`js/physics.js:159`) | confirmed; kinematic bodies cannot rotate |
| H4 | `SceneNode#lookAt(x,y,z)` | scalar form and array form (`scene_bindings.cpp js_node_lookAt`) | `Float64Array.from(target)` — a number yields an empty array (`natives/scene/scene.js:283-285`) | confirmed |
| H5 | `bro.net.disconnect(peer, reason)` | `reason` forwarded (`net_bindings.cpp:315-322`) | `reason` dropped (`js/net.js:64-67`) | confirmed |
| H6 | `bro.window.getDisplays()` | `{bounds:{...}, workArea:{...}}` per display | flat `x/y/width/height/workX/...` (`natives/window/window.js:75-82`) | confirmed shape change; check callers |
| H7 | Names with no new handler, grep-confirmed absent: `document.createEvent`, `Event#initEvent`; SceneNode accessors `alphaCutoff/doubleSided/interior/priority/emissiveColor/fillColor/strokeColor/strokeWidth/billboard/...` + `setInstancedMesh/updateInstance(s)/setAtlasGrid`; `bro.mesh.computeTangents/simplifyWithAttributes/subdivideMidpoint/hasSelfIntersections/findSelfIntersections/intersectsMesh`; `bro.ai` NN class (`forward/backward/toArray/fromArray/copyFrom/sgdStep/adamStep/...`, 65 names), belief/grid/learn classes (~60 names); diffusion `sigmas/config/removeControlNet/clearControlNets/reloadTextEncoder/latent/setLatent` + control-vector and krea2 surfaces (36 names); `bro.vision.removeBackground/invert`; `rigging.addRigifySockets`; `net._sendUnframed` | | confirmed missing (see report for the full list) |

Static rows not yet verified (read both bodies before acting): tile
`distanceField`/`save`/`addObjectKind` args, terrain `setHeightSource`,
diffusion `stepOnce`/`decode` losing their options arg, StyleGAN3 `generate`
options, mesh `convexDecomposition` options, physics `setLayers`/`setLayer`
overloads, tokenizer `encode(text, addSpecial)` second arg, `menu.set` array
form. Known scanner noise: "result keys only in OLD" for handlers whose new
side is a brosurface JS wrapper (results come back through per-field natives),
and "OLD throws / NEW never throws" for the same reason.

## D. Not drift (noted so nobody chases them)

- Old `Element` was one mega-class: every element exposed video/img/form members. New per-tag objects dropping `play()` from a `<div>` is correct.
- bronze does not enumerate host globals or builtin prototype members via `Object.getOwnPropertyNames`, though `in`/`[]` resolve them; `String.prototype` enumerates 1 name. Enumeration-based diffs are noise; use the name-driven probe.
- `nodeName`/`tagName`/`nodeType` became data properties instead of getters.
