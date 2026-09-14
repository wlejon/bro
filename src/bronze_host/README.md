# bronze_host — running bronze-compiled JS apps in bro

The host layer for [bronze](../../../bronze)-compiled (AOT) JavaScript:
registers a browser-shaped set of host globals (`document`, `window`,
`requestAnimationFrame`, the timers, `Image`, `XMLHttpRequest`, `fetch`, `Blob`
and friends, the observers, Web Audio, `Physics`, `AI`, `bro.net`/`WebSocket`)
backed by the engine, wraps `webgl::WebGL2RenderingContext` as a bronze object covering the
WebGL2 surface three.js r160's renderer drives, and owns the per-frame seam that
advances the clock, delivers completions, fires callbacks and performs the
microtask checkpoint.

Enabled by default (`BRO_WITH_BRONZE=ON`).

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
| `host_parser.cpp` | `DOMParser`: HTML text into a second `dom::Document`, and the lifetime policy for it |
| `host_class.cpp` | `HostClass`: the ctor/prototype/handle shape every wrapper family is built from |
| `host_proxy.cpp` | `makeHostProxy`: the property trap behind `style`, computed style, `dataset`, and `localStorage` |
| `eval.cpp`, `eval.h` | in-process JS compilation via Bronze CLI, dynamic evaluation (`eval()`, `new Function()`) and script execution |
| `host_natives.h`, `host_bro_root.cpp` | the `bro` / `__bro` roots, the `__bro_native` root, and **the native convention** every `native_*.cpp` follows |
| `native_time.cpp`, `native_paths.cpp`, `native_window.cpp`, `native_settings.cpp`, `native_dunder_bro.cpp` | the C entry points behind `bro.time`, `bro.appDir`/`userDataDir`/`resolvePath`, `bro.window`, `bro.settings`, and the panels' `__bro.*`, registered through `embed::registerNative` |
| `js/bro_core.js` | the public shapes of those namespaces, JavaScript assembled over the natives |
| `native_manifest_tool.cpp`, `js_entry_stubs.cpp` | `bro-native-manifest`, the build-time tool that prints the manifest `bro_core.js` is compiled against, and the no-op entries it (and arm64 macOS) links |
| `host_vendor_globals.cpp` | vendor global declarations (`signals`, `CodeMirror`, `acorn`, etc.) |
| `gl_*.cpp`, `gl_internal.h` | the WebGL2 binding, one file per call family |

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

Step 1 drains any pending host tasks before advancing the frame.

`dom_globals.cpp` carries the explanation at the code.

## Events, and the dispatch walk they arrive on

A listener a compiled app registers on its canvas, on `document` or on `window`
fires from real user input. It does so because `canvas.addEventListener` calls
`dom::Element::addEventListener`, the engine's native C++ registration, and
`dom::dispatchDomEvent` walks the event path with capture / at-target / bubble
phases and shadow retargeting in registration order.

`document.addEventListener` delegates to `documentElement.addEventListener`,
because the event path is built from Elements.

### The boundary rule

**Engine objects are shared. Event data is copied. Heap values never cross.**

Engine objects are shared: the same `dom::Element`, the same document, the same
clock, the same GL context. What crosses the host boundary is a copy:

- A listener is handed a fresh bronze object holding **copies** of the fields
  its event kind carries — type, coordinates, key, button, deltas, modifiers —
  never an unmanaged pointer across heaps.
- `event.target` is the exception that proves it: a canvas this layer created
  answers as **itself**, the very value the program holds, because identity is
  the whole use of a target. Anything else answers a `{tagName, id, nodeId}`
  descriptor.
- `preventDefault()` / `stopPropagation()` / `stopImmediatePropagation()` write
  through to the `dom::Event` dispatch is walking with. The event object is
  **live only for the duration of the listener call**: calling one of the three
  on a stored event object afterwards is a named `TypeError`.

### CustomEvent

`dispatchEvent` from compiled code takes a plain descriptor —
`{type, bubbles, cancelable, detail}`:

```js
document.dispatchEvent({ type: 'app:ready', detail: 'v2' });
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
# 1. the two manifests: the registry of the binary that will run the app —
#    its host globals (stdout) and its natives (a JSON file), from one run
./build/Release/bro-headless src/bronze_host/fixtures/appdir --print-host-globals --print-native-manifest natives.json > host.globals

# 2. compile the app to a MODULE, into the app directory that will carry it
bronze build src/bronze_host/fixtures/main_scenegraph.js     -o src/bronze_host/fixtures/appdir/app.dll     --emit-shared     --host-globals host.globals     --native-manifest natives.json

# 3. there is no step 3 — the stock binaries load it
./build/Release/bro          src/bronze_host/fixtures/appdir
./build/Release/bro-headless src/bronze_host/fixtures/appdir -e "advanceTime(128)"
./build/Release/bro-headless src/bronze_host/fixtures/appdir drive.js
```

The bronze compiler (which uses brass for codegen) is built as
`build/Release/bronze.exe` via `cmake --build build --config Release --target bronze-cli`
(`bronze-cli`, not `bronze`: the Visual Studio generator leaves an
`EXCLUDE_FROM_ALL` subdirectory's targets out of the solution). Under a multi-config
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
to the host registry instead of throwing `ReferenceError`. The manifest is not
a file kept in this tree: `bro-headless <appdir> --print-host-globals` installs
the host globals exactly as a run would and prints bronze's registry
(`registeredHostGlobals()`, over `bronze::embed::hostGlobalNames()`), and the
in-process compiles (`eval_jit.cpp`, `host_worker.cpp`) read the same registry
straight into `EvalOptions::hostGlobals`. One source, so "compiled against" and
"registered" cannot drift.

`--native-manifest` is the same contract for the natives. An app's
`bro.time.scale` compiles to a direct call of the C function behind it only
when the compiler knows that function's path and signature, and that list is
printed the same way the globals list is: `bro-headless <appdir>
--print-native-manifest <path>` writes `bronze::embed::writeNativeManifest`
over the live registry (`bro::bronze_host::writeNativeManifest`), and the
in-process ahead-of-time compile in `eval.cpp` writes the same file into its
temp dir beside the globals list and passes both to `runBuild`. The JIT path
reads the registry directly. Without the manifest the app still runs — a
`bro.time.scale` is then an ordinary property read of the accessor
`js/bro_core.js` defined — it is only slower.

The app object must export `bronze_main` (bronze's entry convention);
`installWebHostGlobals` runs before `bronze::embed::runMain()`, and the frame
loop then drives everything the app scheduled.

## The `bro` / `__bro` roots and the native convention

`bro`, `__bro` and every namespace under them (`bro.time`, `bro.settings`,
`__bro.perf`, ...) are PLAIN OBJECTS registered as host globals by
`host_bro_root.cpp`. Nothing on them is native. The public surface is
JavaScript, `js/bro_core.js`, compiled at build time like the other modules
under `js/` and entered by `installBroRoots` after the roots are registered.
So `const t = bro.time; t.scale`, `Object.keys(bro.settings)`,
`typeof __bro.perf.fps` all behave as a program expects — they are ordinary
accessors and functions on ordinary objects.

The natives all live under ONE internal root, `__bro_native`, one sub-object
per namespace: `__bro_native.time.scale` is a getter/setter pair,
`__bro_native.settings.get` a function, `__bro_native.perf.fps` a getter over
the engine's own 500 ms frame statistics. `bro_core.js` names each by its
FULL dotted path at the point of use, which is the spelling the compiler
lowers to a direct call; an alias (`const N = __bro_native`) would be an
ordinary property read that finds nothing, because the natives are not
properties of the object. `host_natives.h` states the whole convention:

- **scalars cross as scalars** (`f64`, `i32`, `bool`, `str`); a `str` result
  comes back through `natives::strResult`, a per-thread scratch the runtime
  copies out of during the call;
- **a fixed compound shape crosses as its pieces** and is assembled in the
  wrapper — a display is fourteen natives over a snapshot
  (`__bro_native.window.displaySnapshot()` then `displayName(i)`, ...), a
  window position is two;
- **a dynamic value crosses as JSON text** the wrapper parses: the menu tree,
  the inspector's node trees, a settings category, the action list;
- **a list of strings going in** crosses as one newline-joined `str` (an
  action's binding strings never contain a newline);
- **a callback is a `dynamic`** the C side keeps in a `Persistent` and calls
  through `embed::call` from a point the host owns — `bro.settings.onChange`
  is posted to the host task queue by the engine's settings observer and
  delivered at the top of the next frame seam, never from inside the `set()`
  that made the change.

Why the natives are not registered at the public paths: a native registered
as `bro.time.scale` is reached only by a compiled `bro.time.scale` spelled in
full; every other access lands on the plain `bro.time` object, which would
then need a second, hand-marshalled copy of the same member to answer. One
internal root and one JavaScript wrapper is one definition per member.

The manifest `bro_core.js` is compiled against is printed at BUILD time by
`bro-native-manifest` (`native_manifest_tool.cpp`), a tool that links this
library and calls the same `registerBroNatives` bro calls at run time — so
the two cannot drift. That is also why the compiled objects live in their
own static library, `bro_bronze_js`, linked by the executables after
`bro_bronze_host`: a library that both contained `bro_core.o` and was linked
by the tool that produces `bro_core.o`'s input would be a cycle.
`CMakeLists.txt` writes `generated/natives.json` through
`copy_if_different`, so `bro_core.o` is recompiled when a native moved and
not every time the tool relinked.

The settings store types nothing on the way across: a value is text in the
file, the engine types the keys it owns while applying them, and
`bro.settings.get` types the text by content (`true`/`false`, a number, JSON
for an object the wrapper stored, else a string). The one consequence worth
knowing is that a custom string that reads as a number comes back as one.

## Driving a compiled app from a script

There is no separate driver and no separate mode: bro-headless
(`engine/headless_driver.h`) loads a compiled app exactly as it loads a
script-driven one, with the same argument parsing, the same script / `-e`
modes and the same globals [docs/headless.md](../../docs/headless.md) documents.
The driver script is itself compiled in-process by bronze (`eval.cpp`) and
loaded as a module; there is no interpreter and no REPL.

```bash
bro-headless <appdir> script.js                      # run a script, then exit
bro-headless <appdir> -e "advanceTime(500)" -e "screenshot('out.png')"
```

The compiled app and the driver script are both bronze modules sharing one
runtime, and the **Engine** still boots the app dir's page, which is what
the driver script observes.
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
install and `runMain()` after the page's own `<script>` tags have run.

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
engine's asset mounts (`util/asset_path.h`) and take http(s) through
`util::fetchRemoteCached`, the same remote-asset path so they agree
about what a URL means and what is cached. `WebSocket` and `bro.net` are in
`host_net.cpp`, checked by `tests/bronze_host/run_checks.sh net`.

`Blob`, `File`, `FileReader` and object URLs are DONE — `host_file.cpp`,
checked by `tests/bronze_host/run_checks.sh file`. `blob:` and `data:` URLs
resolve in `fetch`, `XMLHttpRequest` and `Image.src`, out of the engine's
object-URL table (`util/object_url.h`), so a URL minted by compiled code
resolves in the page's markup and vice versa.

A DROPPED file is one of those `File`s — bytes read off disk, the MIME type its
extension implies, and the `.path` attribute (`makeFileFromPath`, used by `host_dom_events.cpp`).
descriptor with `size: 0` and no content, which passes every shape check a page
makes and fails every read. `dataTransfer.items` answers with the same Files,
and its `webkitGetAsEntry().file(cb)` calls back on the FRAME SEAM rather than
synchronously, because that call is asynchronous on the web and code written
against it counts on it — the three.js editor's `getFilesFromItemList`
increments its "handled" counter in the callback and its "total" on the next
line, so a synchronous callback makes it decide the batch is unfinished and
drop every dropped file.

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
the DOM layer itself (`Document::notifyMutation`, in `src/dom/document.h`)
rather than on this layer's own mutators.

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
hang a notice on. This polls from the frame seam and gets current geometry
through `Engine::flushLayoutForRead` like every other read here. The first pass
after `observe()` reports the current size unprompted, which is the behaviour
code actually reaches for one for. The web runs its observation loop until
sizes settle; this reports once per frame, so a callback that resizes its own
target is heard about on the next frame and cannot loop.

`DOMParser` is DONE — `host_parser.cpp`, checked by
`tests/bronze_host/run_checks.sh parser`. `parseFromString` builds a real
`dom::Document` through the same gumbo path the app document uses and hands
back the full document surface bound to it, so the queries, the node factories
and `body`/`documentElement` all answer from the parsed tree.

Two things about it are policy rather than plumbing.

**Parsed documents are never freed.** They are owned by a process-lived vector.
`~Document` severs wrappers through `nodeFreedObservers_`. Node wrappers routinely
outlive the document wrapper they came from (`parser.parseFromString(s).body.firstChild`
drops the document on the same line).
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
`tests/bronze_host/run_checks.sh video`. Class names, methods, argument
shapes and refusals wrap the encoders in `src/video`. Recording is worth having
here for a reason none of the rest of
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

**`finish()` is required to write complete output files.** Both encoders
write final trailers from `finish()`. An encoder dropped without calling
`finish()` will keep whatever was flushed prior.

### A host global must be registered in EVERY build

Compiled out is not the same as absent, and this is where the first
conditionally-compiled feature found the rule. The manifest a module is
compiled against is the registry of whichever build printed it, and a module
compiled on a full build must still run on a lean one. Lowering admits every
manifest name as a global read. At run time `bronze_global_get` asks the builtins, then
the host registry, then `globalThis`, and then calls `fatal()`
(`runtime/rt_state.cpp`) — a miss is not a `ReferenceError` a program can catch
and not an `undefined` it can test, it aborts the process. So in a
`BRO_WITH_VIDEO=0` build `host_video.cpp` still registers both names, bound to
`undefined`. That is explicitly not a miss (`runtime/host_globals.h` says so),
so the lookup succeeds, and `typeof VideoEncoder === 'undefined'` is true.

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
