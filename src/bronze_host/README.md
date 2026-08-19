# bronze_host — running bronze-compiled JS apps in bro

The host layer for [bronze](../../../bronze)-compiled (AOT) JavaScript:
registers a browser-shaped set of host globals (`document`, `window`,
`requestAnimationFrame`, the timers, `Image`, `XMLHttpRequest`, `fetch`, `Blob`
and friends, the observers, Web Audio, `Physics`, `AI`, `bro.net`/`WebSocket`)
backed by the engine, wraps `webgl::WebGL2RenderingContext` as a bronze object covering the
WebGL2 surface three.js r160's renderer drives, and owns the per-frame seam that
advances the clock, delivers completions, fires callbacks and performs the
microtask checkpoint. `src/js/webgl2_bindings*` is the reference for the GL
surface and for every constant value; this layer mirrors it, QuickJS values
swapped for bronze embed Values.

Off by default; nothing here is in the default build.

## The files

| File | What it owns |
|---|---|
| `app_module.cpp` | **the compiled app a FOLDER carries**: finding it, verifying its ABI, running it |
| `dom_globals.cpp` | `document`, canvas, `window`, rAF, `performance`, **and the frame seam** (`hostFrame`) |
| `host_element.cpp` | the element surface an app *builds*: the identity registry, `style`, `classList`, geometry, form controls, computed style, the element-only tree views |
| `host_node.cpp` | the nodes that are not elements — text, comments, fragments — and the tree surface every node shares (`childNodes`, the mutators, `cloneNode`) |
| `host_platform.cpp` | `btoa`/`atob`, `queueMicrotask`, `screen`, `alert`/`confirm`/`prompt`, and the DOM interface names libraries sniff for |
| `host_internal.h` | the non-GL shared surface: error funnel, clock, task queue, handle tags |
| `host_events.cpp` | `on<type>` + `addEventListener` for the objects that fire events |
| `host_dom_events.cpp` | canvas / document / window listeners, wired to the **engine's** dispatch |
| `host_timers.cpp` | `setTimeout`/`setInterval` and the main-thread task queue |
| `host_image.cpp` | the image DECODE behind `.src`, and the lookup that finds a decoded image behind a value |
| `host_element_image.cpp` | `Image` / `HTMLImageElement` as an element CLASS: `new Image()` and `createElement('img')` are one `<img>` node, born on a prototype that chains to `Element`'s |
| `host_xhr.cpp` | `XMLHttpRequest` over the app asset path and http(s); `''`/`text`/`arraybuffer`/`blob`/`json` (see its header) |
| `host_file.cpp` | `Blob`, `File`, `FileReader`, and `URL` — bytes an app holds, and the object URLs that name them |
| `host_abort.cpp` | `AbortController` / `AbortSignal`, and the cancellation `fetch` obeys |
| `host_observers.cpp` | `MutationObserver`, over the DOM layer's own mutation notices; `ResizeObserver`, over a per-frame poll of the layout box |
| `host_parser.cpp` | `DOMParser`: HTML text into a second `dom::Document`, and the lifetime policy for it |
| `host_video.cpp` | `VideoEncoder` / `GifEncoder`: RGBA frames, a 2D canvas or the composited viewport in; a `.webm` or `.gif` file out |
| `host_fetch.cpp` | `fetch()` over the engine's asset mounts, into a real bronze Promise |
| `host_class.cpp` | `HostClass`: the ctor/prototype/handle shape every wrapper family is built from |
| `host_proxy.cpp` | `makeHostProxy`: the property trap behind `style`, computed style, `dataset`, `localStorage`, and behind a bridged interpreted object |
| `host_interp.cpp` | **the interpreter bridge**: compiled `new Function` compiled by the page's QuickJS realm, and values crossing between the two heaps in both directions |
| `host_vendor_globals.cpp` | the names a page loads with plain `<script>` tags — `signals`, `CodeMirror`, `acorn`, `tern`, `esprima`, `jsonlint` — registered as the page's own objects, through that bridge |
| `host_audio_*.cpp`, `host_audio_internal.h` | the Web Audio SURFACE ? `AudioContext` and the node/param objects ? over broaudio. `connect()` routes nothing and most node state is inaudible; see the header of `host_audio_core.cpp` for what actually reaches the engine. `_core` context + globals install, `_param` AudioParam, `_buffer` AudioBuffer + decode, `_nodes` oscillator/filter/analyser/source, `_spatial` Panner + StereoPanner, `_dsp` Delay/Compressor/WaveShaper/Convolver/Splitter/Merger |
| `host_physics_*.cpp`, `host_physics_internal.h` | the `Physics` namespace, `PhysicsCharacter` and `PhysicsSoftBody`, over Jolt. `_core` bodies + globals, `_constraints` joints/motors/limits, `_character`, `_softbody`, `_queries` raycast/overlap |
| `host_ai_*.cpp`, `host_ai_internal.h` | the `AI` namespace, over brogameagent. `_core` globals + aim math, `_navgrid`, `_navmesh`, `_agent` |
| `host_net.cpp` | `bro.net` over GameNetworkingSockets, and the `WebSocket` client |
| `gl_*.cpp`, `gl_internal.h` | the WebGL2 binding, one file per call family |
| `web_host.globals` | the manifest of global names bronze admits; every one must be registered ? an unregistered name is `fatal()`, not a miss |

## How a compiled app gets in

The app directory carries `app.dll` / `app.so` / `app.dylib` beside its
`index.html`, and `bro <folder>` or `bro-headless <folder>` finds it, checks its
ABI stamp, and runs it (`app_module.h`). The stock binaries do this. Nothing
about bro's build knows the app exists, which is the point: "another app" is
another folder, not another build of bro.

There used to be a second way — `host_main.cpp` linked one `bro-bronze-host`
executable per app, with the app named as an object file at bro's configure time
(`BRO_BRONZE_APP_OBJ`, `BRO_BRONZE_APPS`). It is gone. It existed only because
every bronze module was a **static** library while the runtime's state is
process-wide (`rt_state.h`: heap, arena, root shapes, key registry, host-global
registry, "owned by ONE translation unit") — so a loaded module would have got
its own heap and its own registry, two collectors neither tracing the other's
roots. bronze answering that with `bronze_runtime_shared` and `--emit-shared` is
what let the link-time path be deleted rather than maintained.

Which is why this layer links `bronze::runtime_shared` and must never also link
`bronze::embed` + `bronze::runtime`: the static archives would put that second
heap right back, and the failure would not be a link error but a value quietly
collected out from under the module. `embed` lives inside the shared image, so
one target supplies both halves.

What the loader already does *not* need from bronze is the safety check, because
bronze built it first: the ABI fingerprint is the first 32 bits of
`bronze_abi.h`'s SHA-256, codegen stamps it into every emitted object as
`bronze_object_abi_fingerprint`, and the loader compares it before resolving the
entry point. That matters more here than for a linked object: a stale linked
object at least forced a relink, whereas a stale module loads happily and then
reads arguments nobody passed — half-minute stalls at nondeterministic points
rather than a crash, which is the failure `bronze_abi.h` names as its own
motivation. The one difference is what happens next. bronze's guard calls
`fatal()`, correctly, because a linked object's mismatch means the process is
malformed; the loader *refuses* instead, because a loaded module is data the
folder supplied and a bad app must not take the runtime down with it.

## The frame seam, which is the thing to understand first

`installWebHostGlobals` registers one `Engine::onFrame` callback. The engine
fires it at the point its own `requestAnimationFrame` fires — `engine_frame.cpp`
step 3a windowed, the equivalent point in each `advanceTime` step headless, and
after the timer dispatch in the server tick — under rAF's pause gate, with the
scaled-clock delta. Inside it, in this order:

1. **a leftover microtask drain**, if anything is queued;
2. **the clock**, advanced before anything reads it;
3. **host tasks** — image loads, XHR completions — *before* rAF, which is where
   the web runs a load event relative to the rendering steps;
4. **timers**;
5. **rAF callbacks**;
6. **the microtask checkpoint** — `embed::drainMicrotasks()`.

Step 6 is after rAF and not before, because an rAF callback is the main producer
of promise jobs in a render loop. Draining first would run every frame's
continuations one frame late, against the wrong state, and would mean the last
rAF before shutdown never reaches quiescence — which is where an unhandled
rejection is reported, so that rejection would never be reported at all.

Step 1 exists because bro drains QuickJS **twice** per frame (once right after
rAF, once after the late pumps that can resolve a promise) and there is only one
host hook, at the first of those points. A producer that enqueues a bronze job
after we return this frame is therefore seen at the top of the next one, rather
than whenever something else happens to drain.

`dom_globals.cpp` carries the same explanation at the code.

## Events, and the one dispatch walk they arrive on

A listener a compiled app registers on its canvas, on `document` or on `window`
fires from a real click. It does so because it is not this layer's listener at
all: `canvas.addEventListener` calls `dom::Element::addEventListener`, the
engine's own C++ registration, and `js::dispatchDomEvent` already walks the
event path ONCE with both listener kinds merged on a shared registration
sequence (`dom/event_target.h`). So a compiled handler gets the capture /
at-target / bubble phases, the shadow retargeting, and its place in
registration order beside the page's own handlers — for the same reason an
interpreted one does, not by a parallel arrangement that agrees with it.

`document.addEventListener` is `documentElement.addEventListener`, exactly the
delegation `src/js/document_bindings.cpp` performs for the interpreted side and
for its reason: the event path is built from Elements. The visible consequence
is the one the interpreted side already lives with — `currentTarget` inside a
document handler is `<html>`, not the document.

### The boundary rule

**Engine objects are shared. Event data is copied. Heap values never cross.**

Both worlds run in one Engine, on one thread, against one DOM, interleaved and
never concurrent. What they share is the *engine's* objects: the same
`dom::Element`, the same document, the same clock, the same GL context. What
crosses the language boundary is a copy:

- A listener is handed a fresh bronze object holding **copies** of the fields
  its event kind carries — type, coordinates, key, button, deltas, modifiers —
  never a QuickJS value and never a pointer into either heap.
- `event.target` is the exception that proves it: a canvas this layer created
  answers as **itself**, the very value the program holds, because identity is
  the whole use of a target. Anything else answers a `{tagName, id, nodeId}`
  descriptor.
- `preventDefault()` / `stopPropagation()` / `stopImmediatePropagation()` write
  through to the `dom::Event` dispatch is walking with, so a compiled listener
  cancels an event for the interpreted listeners after it, and vice versa. The
  event object is **live only for the duration of the listener call**: calling
  one of the three on a stored event object afterwards is a named `TypeError`,
  not a silent no-op and not a write through a dangling pointer.

### The exception, and it is one exception: the interpreter bridge

`host_interp.cpp` is where heap values DO cross, and it exists because one
thing an AOT compiler cannot do is compile a string the program builds at run
time. `new Function(source)` is not a corner: it is how the three.js editor's
Play button runs a user's script (`editor/js/libs/app.js`), and refusing it
refuses the feature.

So a compiled `new Function` is compiled by **the page's QuickJS realm** — the
one already in the process, running the page's own `<script>` tags — and the
function it produces comes back wrapped. From there values cross in both
directions: primitives by value, objects and functions by a wrapper that
forwards property reads, writes, calls, `in`, `delete` and enumeration to the
other heap, typed arrays by copy (embed's pointer contract makes a shared
buffer a dangling read at the next allocation), and thrown values as throws
rather than as silent undefined.

Two things make that safe to say rather than merely to hope:

- **Identity is a table, not an address.** A value that has crossed once is
  wrapped once, so a round trip returns the object that went in. bronze's
  collector MOVES, so the bronze side of that table is compared by walking it
  rather than by hashing raw bits — the bits name a pre-collection address.
  QuickJS is refcounted and does not move, so its side is a hash.
- **The table owns both halves, and neither finalizer touches the other
  collector.** A wrapper holds an index into the table and nothing else.

Engine objects still do not cross — they are *shared*, which is the rule above
and not an exception to it. A `<div>` handed through the bridge in either
direction resolves to the same `dom::Element`, so `appendChild` gets a node
rather than a wrapper that merely answers `nodeType`.

`host_vendor_globals.cpp` is what the bridge bought: `CodeMirror`, `esprima`,
`acorn`, `tern`, `signals` and `jsonlint` used to be four hundred and fifty
lines of hollow C++ reimplementation — a `CodeMirror` you could not type in, an
`esprima.parse` that approved every program. They are now the page's real
libraries. A page that did not load one gets `undefined` for it, which is what
the same page gets in a browser.

### CustomEvent, which is the sanctioned channel between the two worlds

`dispatchEvent` from compiled code takes a plain descriptor —
`{type, bubbles, cancelable, detail}` — rather than a `new CustomEvent(...)`.
That was forced when nothing here could be built on a chosen prototype, and is
now merely unwritten: see **Host classes** below. The descriptor is the
documented channel and compiled code already speaks it, so it stays until
someone needs the constructor. `bubbles` and `cancelable` default to true.

```js
// compiled → interpreted
document.dispatchEvent({ type: 'app:ready', detail: 'v2' });

// interpreted → compiled  (an ordinary page script)
document.dispatchEvent(new CustomEvent('page:reset', { detail: 'hard' }));
```

**`detail` is a string and only a string.** A `detail` is an arbitrary JS value
on the web, and an arbitrary JS value belongs to exactly one heap; a string is
the one shape both heaps can copy without agreeing on a type system. It is
carried by `dom::CustomEvent` (`src/dom/event.h`), which is what makes it
survive the trip in either direction. A compiled `dispatchEvent` whose `detail`
is an object is a `TypeError` naming the reason, not a stringification — an
interpreted listener receiving `"[object Object]"` would be worse than being
told it cannot go. In the other direction an interpreted dispatch with a
non-string detail still reaches the interpreted listeners with the real value;
only the compiled ones see no payload.

`tests/bronze_host/` pins a round trip in both directions.

### Not supported, precisely

- **`once` and `capture`** are accepted (`addEventListener(type, fn, true)` or
  `{capture, once}`) and honoured by the engine's own list — but a `once`
  listener the engine reaps is not removed from this layer's
  `removeEventListener` bookkeeping, so removing it afterwards is a no-op
  rather than an error.
- **`on<type>` properties** (`el.onclick = fn`) ARE wired for DOM elements, on
  `Element.prototype`, one per event type the engine dispatches — the mouse and
  pointer families, `wheel`, `contextmenu`, the key family, `input`, `change`,
  `submit`, `scroll`, `focus`, `blur`. Assigning replaces the previous handler,
  assigning `null` or a non-callable clears it, and the getter answers the
  function or `null`, as the web does. Two divergences: the handler is
  registered as an ordinary listener, so reassigning it moves it to the END of
  the listener order rather than keeping its original position; and `document`
  and `window` have no `on<type>` slots at all — use `addEventListener` there.
  (`Image` and `XMLHttpRequest` keep their own `on<type>` slots — different
  objects, different file: `host_events.cpp`.)
- **`click`'s `offsetX` / `offsetY` are 0.** Not this layer: bro synthesizes the
  `click` event without `applyMouseOffset`, so every listener sees 0, compiled
  and interpreted alike. `mousedown`, `mouseup`, `mousemove` and `wheel` carry
  real offsets.
- **Listeners on arbitrary elements.** This layer creates `<canvas>` and
  `<img>` and nothing else, so the reachable targets are a canvas it made, the
  document (i.e. `documentElement`), and the window. There is no
  `querySelector`.
- A registration that cannot be delivered **throws**. A type that is not a
  string, a listener that is not a function, a target element that does not
  exist yet — each is a `TypeError` or an `Error` naming the object, never a
  registration that quietly never fires.

## Host classes

Every object this layer hands a compiled program used to be a bare cell with
its methods closed over PER INSTANCE. That cost two things, and the second one
is the one that shows: a copy of every method for every instance, and
`instanceof` answering false for all of them. A dozen comments in these files
used to say the same sentence — *bronze cannot build a value on a chosen
prototype* — and shaped real API around it. `new CustomEvent(...)` does not
exist because of it.

That is no longer true, and `Image` (`host_element_image.cpp`) is the worked
example.
The class story is three calls, none of them new:

1. `makeFunction` for the constructor, then **read** `prototype` off it with
   `getProperty`. Reading MINTS the slot-backed object 10.2.4 describes, as an
   ordinary plain object. (`setProperty` still refuses `prototype` by name —
   it is the read that gives you one, not a write.)
2. Decorate that prototype like any other object: `ObjectBuilder` over it, one
   copy of each method and accessor for the whole class.
3. Birth each instance with the 4-argument `makeHandle(data, dtor, when,
   prototype)`.

Instances then inherit the shared methods, answer `x instanceof Ctor`, and
share the memoized per-prototype root shape — so property reads keep their
inline caches. *Born on*, not swapped on: `Object.setPrototypeOf` after the
fact also works and keeps the payload, but it puts the cell in dictionary mode
for the rest of its life.

A method reached through the prototype still unwraps its receiver with
`handleData(thisValue)`, which is the part worth pinning rather than assuming:
the payload and the handle brand live in internal slots that the prototype does
not reach. `tests/bronze_host/run_checks.sh class` pins it, along with the shared
-methods and `instanceof` claims.

`Image` and `HTMLImageElement` are the SAME constructor here, as on the web.

`HostClass` (`host_class.cpp`) wraps the three calls, and every family in this
layer has been through it: Image, Element/HTMLElement, Blob/File/FileReader,
XMLHttpRequest, Headers/Request/Response, AbortController/AbortSignal,
MutationObserver/ResizeObserver, WebSocket, VideoEncoder/GifEncoder, the nine
Web Audio interfaces, PhysicsCharacter/PhysicsSoftBody and the three AI
handles. `HostClass::inherit` chains one prototype onto another through the
program's own `Object.setPrototypeOf`, which is what makes `file instanceof
Blob` and `gain instanceof AudioNode` true.

Converting forces the members to read their RECEIVER rather than close over the
payload, which fixed a real hazard on the way past: a detached method holding a
raw `HostBlob*` it did not root was a dangling read waiting for someone to
write `const slice = blob.slice`.

**What is deliberately NOT a class:**

- **The WebGL cells** (`gl_internal.h`): `WebGLBuffer`, `WebGLTexture` and
  friends carry no methods on the web either, so a prototype would save
  nothing, and the names are not in the manifest to be an instance of.
- **Text, Comment and DocumentFragment** wrappers: they share
  `installNodeTree`, which reads its receiver and so serves both, but they are
  not Elements and giving them that prototype would be a lie every tree walker
  would believe.
- **A few names that brand nothing** because this layer builds no instance of
  them: `HTMLCanvasElement`, `WebGLRenderingContext`, `PannerNode`,
  `StereoPannerNode`, and the interface table in `host_platform.cpp`. A canvas
  is an Element whose own properties shadow the shared ones, so it is an
  `HTMLElement` instance rather than an `HTMLCanvasElement` one.

## Configure

```bash
cmake -B build -DBRO_WITH_BRONZE=ON            # ../bronze, else third_party/bronze
```

Resolves bronze at `../bronze`, falling back to the `third_party/bronze`
submodule (`-DBRONZE_DIR=<path>` overrides both; with neither present it is a
configure error naming the path it looked at). The configure line says which
tree it took — `bronze: standalone tree (...)` or `bronze: submodule tree
(...)` — because the whole hazard of having a fallback is building one while
editing the other. CI and the nightly build the submodule, so the pointer is
what they ship; `scripts/repo-status.sh --sync` bumps it to your standalone
HEAD the same way it does for every other sibling. bronze's own configure
requires doctest, so the toolchain must provide it (bronze auto-detects a vcpkg
root when bro's configure didn't set one).

## Compile and run an app

```bash
# 1. compile the app to a MODULE, into the app directory that will carry it
bronze build src/bronze_host/fixtures/main_scenegraph.js     -o src/bronze_host/fixtures/appdir/app.dll     --emit-shared     --host-globals src/bronze_host/web_host.globals

# 2. there is no step 2 — the stock binaries load it
./build/Release/bro          src/bronze_host/fixtures/appdir
./build/Release/bro-headless src/bronze_host/fixtures/appdir -e "advanceTime(128)"
./build/Release/bro-headless src/bronze_host/fixtures/appdir drive.js
```

A tree configured with `-DBRONZE_WITH_LLVM=ON` (bro's default for a fresh cache)
builds that compiler itself, as `build/Release/bronze.exe` — `cmake --build
build --config Release --target bronze-cli` (`bronze-cli`, not `bronze`: the
Visual Studio generator leaves an `EXCLUDE_FROM_ALL` subdirectory's targets out
of the solution). Under a multi-config
generator the CLI cannot find the shared runtime's import library on its own —
it searches `shared/` beside and above itself, and MSBuild puts the library one
level deeper in `shared/<Config>/` — so pass it:

```bash
BRONZE_SHARED_RT_LIB=$PWD/build/shared/Release/bronze_runtime_shared.lib     ./build/Release/bronze.exe build ...
```

### More than one app in one tree

Two directories. There is nothing to configure, no target to add, and no limit:
bro's build does not enumerate applications any more than a browser's build
enumerates web pages. `tests/bronze_host/lib.sh` is worth reading as the worked
example — it compiles a probe into its app dir on demand and rebuilds it when
the module is older than either the probe or the compiler.

`--emit-shared` links the module against bronze's **shared** runtime, so host
and module share one heap; it exports exactly three names, all derived from the
entry: `bronze_main`, `bronze_object_abi_fingerprint` and
`bronze_main_host_globals`. `--emit-obj` still exists for a host that links an
app in, which bro no longer does.

`--emit-obj` is what makes step 1 stop before linking: the object is destined
for **bro's** toolchain, and linking belongs to whoever owns the final binary.
`--host-globals` is what makes the app's reads of `document` and friends resolve
to the host registry instead of throwing `ReferenceError` — the manifest and
`installWebHostGlobals` are two halves of one list and must stay identical.

The app object must export `bronze_main` (bronze's entry convention);
`installWebHostGlobals` runs before `bronze::embed::runMain()`, and the frame
loop then drives everything the app scheduled.

## Driving a compiled app from a script

There is no separate driver and no separate mode: bro-headless
(`engine/headless_driver.h`) loads a compiled app exactly as it loads an
interpreted one, with the same argument parsing, the same script / `-e` / REPL
modes and the same globals [docs/headless.md](../../docs/headless.md) documents.

```bash
bro-headless <appdir> script.js                      # run a script, then exit
bro-headless <appdir> -e "advanceTime(500)" -e "screenshot('out.png')"
bro-headless <appdir>                                # interactive REPL
```

The compiled app has no JS realm — but the **Engine** does, because it still
boots the app dir's page, and that realm is where the driver script runs.
Driver and app share the Engine, the document and the clock, which is the whole
mechanism:

- `advanceTime(ms)` steps the engine, and each step fires `Engine::onFrame` —
  this layer's frame seam. So one `advanceTime(16)` is one `APP frame=N` from
  the compiled app, rAF callbacks and microtask checkpoint included.
- `screenshot()` / `getPixel()` composite the real frame, the app's WebGL
  canvas in it: the app appended that `<canvas>` to the same document, so
  `document.querySelector('canvas')` in the script finds it and
  `getContext('webgl2')` on it hands back the very context the app is drawing
  through.
- `assert()` fails the run with a nonzero exit, as in bro-headless.

What the script does **not** get is the app's own JS objects — there are none;
its scene graph is machine code with no reflective surface. A driver observes
the app the way a user does: through the DOM, the frame, and the pixels. The
seam this rides on is `HeadlessHooks::afterEngine`, which runs the host-globals
install and `runMain()` at the point an interpreted app's own JS would have
just finished.

Frame counts come from the driver — `advanceTime(n)` over the virtual clock —
rather than from a `--frames` flag, because the retired per-app host owned its
own main loop and bro-headless is driven from JS.

## Test Fixtures vs Real Applications

> [!IMPORTANT]
> `fixtures/` contains **internal integration test fixtures** used for CI and CTest checks.
> Real applications (such as the Three.js Editor or custom tools) live in `broworkshop/tools/<name>`
> or in standalone app repositories with their own `bro.json` and `index.html`.

`fixtures/main_scenegraph.js` exercises everything below the renderer: scene graph,
matrix math, the host DOM, the WebGL2 context object, timers, rAF and the
microtask checkpoint. The renderer fixtures import `WebGLRenderer` from r160's
published `build/three.module.js`, vendored byte-for-byte in the bronze
checkout (`bronze/tests/oracle/threejs/three.module.js` — origin and sha256 in
the README beside it): `fixtures/main.js` is the basic cube, `fixtures/main_lit.js` adds
`MeshStandardMaterial` + lights, `fixtures/main_textured.js` a procedural
`DataTexture` checkerboard. Each prints `gl.readPixels` predicates after
`render()`, so a correct frame is checkable from stdout alone.
`fixtures/MISSING_MODULES.md` records why the bundle, not ~200 vendored modules.

`fixtures/appdir` is the minimal fixture directory the test executable boots from.

## The hybrid app dir: `"compiled": true`

An app dir a compiled host boots from declares itself in its `bro.json`:

```json
{ "title": "my app", "compiled": true }
```

It is a **declaration, not a switch** — nothing in the engine behaves
differently on it (`EngineConfig::compiledApp`). It exists so a mismatch
between an app dir and the binary opening it can be *reported* rather than
discovered:

- **plain `bro` / `bro-headless` on a `"compiled": true` dir** logs a warning
  naming the situation and runs anyway. Not a refusal, because the interpreted
  half of a hybrid dir is real and does run — the page, its styles, its own
  `<script>` tags. What is missing is the app's logic, and an app that runs its
  page and none of its logic is otherwise indistinguishable from one that is
  simply broken. The warning is the difference.
- **a compiled host on a dir that does not declare it** warns the other way:
  add the flag, because it is what tells any *other* binary that this dir needs
  one.

The mechanism is one `LOG_WARN` pair at engine init (`engine_init.cpp` step 6).
A host executable that has a compiled app linked in says so with
`EngineConfig::hostProvidesCompiledApp` (or `HeadlessHooks::providesCompiledApp`
in driver mode); that flag describes the *binary*, and is not a manifest key.

### Why an app dir can carry interpreted JS at all

The engine executes the page's `<script>` tags unconditionally
(`engine_init.cpp` step 10), before the host runs the compiled top level. So a
hybrid dir is not a special mode: it is an ordinary app dir whose page happens
to hold UI script, running in the Engine's QuickJS realm beside a compiled
program running as machine code. They share the DOM on one thread and talk
through it — see "The boundary rule" above, and
`tests/bronze_host/appdir_events/` for a working one.

`tests/bronze_host/` holds the integration check that runs the compiled app and
diffs its output against a committed expectation.

## Deliberately not covered (yet)

**GL**: samplers, sync, occlusion queries, transform feedback, PBO paths,
`mapBufferRange`, `getIndexedParameter`, 3D/array textures, non-square matrix
uniforms, `vertexAttrib*` default-value setters, and `getContext('2d')`.
`getParameter`'s array-shaped answers are pseudo-arrays (indexable, `length`, no
`Array.prototype`).

**Events**: the exact list is under "Not supported, precisely" above.

**Loading**: nothing outstanding. `fetch` and `XMLHttpRequest` read the
engine's asset mounts (`js/asset_path.h`) and take http(s) through
`util::fetchRemoteCached`, the same remote-asset path both share so they agree
about what a URL means and what is cached. `WebSocket` and `bro.net` are in
`host_net.cpp`, checked by `tests/bronze_host/run_checks.sh net`.

`Blob`, `File`, `FileReader` and object URLs are DONE — `host_file.cpp`,
checked by `tests/bronze_host/run_checks.sh file`. `blob:` and `data:` URLs
resolve in `fetch`, `XMLHttpRequest` and `Image.src`, out of the ENGINE's
object-URL table (`util/object_url.h`), so a URL minted by compiled code
resolves in the page's markup and vice versa.

`URL` is a CONSTRUCTOR and carries the statics: `new URL(href, base)`,
`URL.createObjectURL`, `URL.revokeObjectURL`, `URL.parse(href, base)` and
`x instanceof URL` all work. It was a bare namespace until `setProperty` took a
FUNCTION receiver — see **Host classes** — because before that a callable URL
had nowhere to hang `createObjectURL`. `URL.parse` stays beside the constructor
rather than behind it: it is a real 2024 addition to the web platform, and it
answers null where the constructor throws.

`url.searchParams` is a LIVE view, and the same object every read —
`u.searchParams === u.searchParams`, as on the web. `get`, `has`, `getAll`,
`set`, `append`, `delete` and `toString` are all there, and the mutators write
back through to `url.search` and `url.href`. Keys and values are
percent-encoded on the way out and decoded on the way in, so a value carrying
`&` or `=` survives the round trip instead of re-parsing as extra pairs. The
view holds the parse, not the URL object, so one kept past its URL
(`const p = new URL(s).searchParams`) still reads and writes coherently rather
than dangling. Not present: `sort`, `forEach`, and the iterator protocol.

`AbortController` and `AbortSignal` are DONE — `host_abort.cpp`, checked by
`tests/bronze_host/run_checks.sh abort`. `fetch(url, {signal})` rejects with the
signal's reason instead of reading the file, `AbortSignal.abort`, `.timeout` and
`.any` are all present, and `throwIfAborted()` throws the reason untouched.

Two shapes differ from the web, for the same reasons as `URL`. `AbortSignal`
is a NAMESPACE object rather than a constructor, which costs nothing real —
`new AbortSignal()` is a TypeError on the web too. And `reason` defaults to a
plain `{name, message}` where the web hands you a `DOMException`, because
bronze still cannot build a value on a chosen prototype; `e.name ===
'AbortError'` is what real code tests and it answers correctly.

`signal.aborted` is a writable property the app owns, and the host does not
believe it: cancellation is decided on the payload struct's own copy, so
assigning `signal.aborted = false` cannot talk a fetch into delivering a
response the program already cancelled. The abort test pins that.

A bronze `fetch` settles on the next host-task drain, so an abort rejects one
frame after `abort()` rather than at the moment of the call. Every abort a
program can express — from a listener, a microtask, a timer — lands inside that
window, and the outcome is the same rejection; what it buys is one settle path
instead of two.

**Images**: `ImageBitmap` and `createImageBitmap`. `Image` is a host object,
not a `dom::Element` — it has no layout box. It IS a real class, though: see
**Host classes** below.

`MutationObserver` is DONE — `host_observers.cpp`, checked by
`tests/bronze_host/run_checks.sh observer` — and it is built on a notice fired by
the DOM layer itself (`Document::notifyMutation`, new in `src/dom/document.h`)
rather than on this layer's own mutators. That is the difference between an
observer that sees every change to the tree and one that sees only the changes
compiled code made: the check's `page.*` assertion is a script in the page's
QuickJS realm setting an attribute, and the compiled observer hearing about it.

Records are delivered once per frame from the frame seam, after
requestAnimationFrame and before the closing microtask drain, rather than at the
microtask checkpoint that follows the mutation. So a mutation made in an rAF
callback or a timer is reported in the same frame and one made in an event
handler at the top of the next — the one-frame resolution everything
asynchronous in this layer has, because there is one host seam per frame.
Records queued from inside a callback wait for the following delivery, which is
what stops an observer that mutates what it observes from re-entering itself.

`addedNodes` and `removedNodes` carry at most one node, because the DOM's
mutators move one node at a time; on the web they are longer only for
`replaceChildren` and `innerHTML`, neither of which exists here. A comment
node's `data` is not observed: `dom::CommentNode` has no document notification
at all, where `TextNode` funnels all five of its mutators through one.

`ResizeObserver` is DONE too, in the same file and the same frame slot, and it
is a POLL rather than a notification — a box changes size because a window
resized, a font arrived or a sibling grew, and none of those is a mutation to
hang a notice on. bro's own JS ResizeObserver polls too, from the engine's
post-layout hook; this one polls from the frame seam and gets current geometry
through `Engine::flushLayoutForRead` like every other read here. The first pass
after `observe()` reports the current size unprompted, which is the behaviour
code actually reaches for one for. The web runs its observation loop until
sizes settle; this reports once per frame, so a callback that resizes its own
target is heard about on the next frame and cannot loop.

`DOMParser` is DONE — `host_parser.cpp`, checked by
`tests/bronze_host/run_checks.sh parser`. `parseFromString` builds a real
`dom::Document` through the same gumbo path the app document uses and hands
back the full document surface bound to it, so the queries, the node factories
and `body`/`documentElement` all answer from the parsed tree. The mime-type
argument is read and discarded: bro has no XML parser, and the interpreted side
made the same choice.

Two things about it are policy rather than plumbing.

**Parsed documents are never freed.** They are owned by a process-lived vector.
The obvious alternative — a handle finalizer that deletes the document when its
wrapper is collected, which is exactly what the QuickJS side does — does not
port. `~Document` severs wrappers through `nodeDestroyingCb_`, a single callback
slot the JS realm owns, and it visits elements only; the freed-node observer
LIST this layer's registry depends on is not fired from `~Document` at all, so a
finalizer would leave a live registry entry pointing into released storage for
every node of that document this layer had wrapped. And even with that fixed, a
node wrapper routinely outlives the document wrapper it came from
(`parser.parseFromString(s).body.firstChild` drops the document on the same
line) where the web keeps the document alive through the node — and since
registry entries are themselves never freed, rooting the document from a node
would pin it forever anyway. The cost is a `Document` husk per parse, which is
real for an app that parses every frame. Two things would make the finalizer
safe: `~Document` firing the freed-node observer list (a small bro fix — the
list exists in `dom/document.h` for exactly this kind of wrapper layer, and
nothing fires it, so every registry entry for one of that document's nodes
would be left pointing into released storage), and a registry entry that can be
released at all, which wants a finalizer able to make embed calls.

**Appending a parsed node into the live tree adopts it**, and that step moved
into `Node::appendChild` / `Node::insertBefore` (`src/dom/element.cpp`) to make
it true here. It used to sit in `element_bindings.cpp` under the heading of
things that "genuinely need a JS realm", which it never did — it is a document
pointer comparison and a call to `Document::adoptNode`. A compiled program
appends without passing through the JS bindings at all, so leaving the step
with the callers meant the live tree held nodes the parser document still owned
and would eventually destroy. Layout invalidation moved down for the same
reason and the file comment now says so.

A second document is also what turned two single-document assumptions in this
layer into bugs, both fixed with it: the mutation hook remembered a bool
("installed") rather than which documents carried it, so whichever document was
observed first silenced every later one; and the node registry's freed-node
observer did the same, which was the more dangerous of the two — if the first
node this layer ever wrapped came from a parsed document, the LIVE document was
left unwatched and every wrapper it handed out could outlive its node.

`VideoEncoder` and `GifEncoder` are DONE — `host_video.cpp`, checked by
`tests/bronze_host/run_checks.sh video`. Same class names, same methods, same
argument shapes and the same refusals as bro's own bindings
(`src/js/video_bindings.cpp`), because both wrap the same encoders in
`src/video`. Recording is worth having here for a reason none of the rest of
this layer has: an app can write its own observer or its own parser, and it
cannot write VP9 or read the composited framebuffer.

There is no `VideoFrame`, because bro has none — the name is WebCodecs', whose
model is a frame object you construct and close. bro's encoders take pixels
directly and own the copy, and inventing a frame object for the compiled side
alone would be a surface the interpreted side does not have.

**`addViewportFrame` is the capture that matters here**, and `addCanvasFrame`
is nearly unreachable from compiled code alone. This layer's canvas answers
only `webgl` and `webgl2` from `getContext`, and `addCanvasFrame` refuses a
canvas carrying WebGL or a 3D scene on purpose — such a canvas still has an
auxiliary `CanvasScene` for overlay compositing, so reading that surface would
silently encode a blank overlay instead of the render. So the 2D path reaches
compiled code exactly one way: the PAGE creates the canvas and the app finds it
by id, which is the mixed app this layer exists to make possible and is what
the check's `canvas.*` lines exercise. `GifEncoder.addViewportFrame` did not
exist on the interpreted side and was added there in the same change, since
without it "record this to a GIF" had no answer at all for a WebGL app.

**`finish()` is not optional here**, and it is the one place this layer's
behaviour differs from bro's JS rather than merely narrowing it. Both encoders
finish from their destructor, so on the QuickJS side a forgotten `finish()`
still yields a complete file when the context is torn down. bronze has no
teardown sweep: a handle's destructor runs from the post-collection hook of a
collection that reclaims it, and nothing collects at exit. An encoder dropped on
the floor is therefore finished only if a GC happens to reach it, and otherwise
the file keeps whatever the muxer wrote and no trailer.

### A name in `web_host.globals` must be registered in EVERY build

Compiled out is not the same as absent, and this is where the first
conditionally-compiled feature found the rule. Lowering admits every manifest
name as a global read. At run time `bronze_global_get` asks the builtins, then
the host registry, then `globalThis`, and then calls `fatal()`
(`runtime/rt_state.cpp`) — a miss is not a `ReferenceError` a program can catch
and not an `undefined` it can test, it aborts the process. So in a
`BRO_WITH_VIDEO=0` build `host_video.cpp` still registers both names, bound to
`undefined`. That is explicitly not a miss (`runtime/host_globals.h` says so),
so the lookup succeeds, `typeof VideoEncoder === 'undefined'` is true, and the
feature detection bro's own docs tell an app to write keeps working — which is
also exactly what the interpreted side of a video-less build looks like, where
the classes are simply not installed.

`dataset` is DONE, and so is the reach that blocked it — `host_proxy.cpp`,
checked by `tests/bronze_host/run_checks.sh proxy`. It was blocked rather than
merely unwritten: it is a live view whose keys are not known in advance, so it
needs a PROPERTY TRAP. Everything else about it could be faked;
`el.dataset.newKey = 'v'` could not, and a dataset that silently drops that
write is worse than no dataset at all.

bronze's `Proxy` implements the 10.5 essential invariants, and every one of
those checks reads the TARGET and never calls a trap — so a proxy over an empty
extensible object constrains nothing and may answer entirely from an element's
attributes. What was missing was only the REACH, and the general pair
`globalValue(name)` + `construct(fn, args)` supplied it: `globalValue("Proxy")`
finds the constructor on the same builtin ladder a compiled free read walks,
and `construct` builds the proxy with host functions as its traps.

`makeHostProxy` (`host_internal.h`) is that shape written once, and FOUR live
views are built on it. `el.style` and `getComputedStyle` reach all 363
properties plus custom `--*` ones, rather than the curated ~110-name list that
an accessor pair per property per element forced; `el.dataset` exists;
and `localStorage` answers named properties (`localStorage.token`) beside its
methods. The curated list survives in one place only — enumerating a computed
declaration, where the web lists every supported property and htmlayout has no
registry to ask for that list.

The per-element cost went the right way with it: a styled element used to build
an accessor PAIR for each of ~110 names in both spellings, and now builds four
methods and a trap pack.

Text nodes, comments, fragments and `cloneNode` are DONE — `host_node.cpp`,
checked by `tests/bronze_host/run_checks.sh node`. `childNodes`, `firstChild`,
`lastChild`, `nextSibling` and `previousSibling` walk NODES; `children`,
`firstElementChild` and `nextElementSibling` are the element-only views beside
them. CharacterData offsets are UTF-16, converted at the boundary, because the
DOM stores UTF-8 and JS string indices are not byte indices.
