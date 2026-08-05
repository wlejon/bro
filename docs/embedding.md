# Embedding bro in your own executable

bro builds as static libraries with three thin `main()`s on top. Nothing stops a
fourth: link `bro_engine`, add your own JS bindings and media backends, and ship
one binary. This is how [ffmpeg-bro](https://github.com/wlejon/ffmpeg-bro) links
GPL libav\* without ffmpeg ever entering bro's MIT tree.

## Why you would

Two reasons, both licensing-shaped or dependency-shaped:

- A library you want is under a license bro cannot take (ffmpeg's GPL encoders),
  or is too heavy to ask every bro user to build.
- You want a real application binary — its own name, icon, settings file and
  command line — not `bro.exe path/to/app`.

## The CMake side

```cmake
set(BRO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../bro" CACHE PATH "bro engine source")
add_subdirectory("${BRO_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/bro")

add_executable(myapp WIN32 src/main.cpp)
target_link_libraries(myapp PRIVATE bro_engine)
target_include_directories(myapp PRIVATE "${BRO_DIR}/src")

if(MSVC)
    # QuickJS's interpreter frame is large; deep JS recursion overflows the
    # default 1 MB stack. Same reason bro.exe needs it.
    target_link_options(myapp PRIVATE /STACK:8388608 /ENTRY:mainCRTStartup)
endif()
```

`BRO_BUILD_EXECUTABLES` defaults OFF when bro is not the top-level project, so
this does not also produce a `bro.exe` you did not ask for. Pass
`-DBRO_BUILD_EXECUTABLES=ON` if you want them anyway.

## Finding your UI and starting the engine

`engine/launcher.h` holds the launch plumbing all three bro binaries share, so
you resolve an app directory the same way they do:

```cpp
#include "engine/engine.h"
#include "engine/launcher.h"

bro::engine::EngineConfig config;
config.title = "myapp";
config.displayMode = bro::engine::DisplayMode::Windowed;
config.settingsPath = bro::engine::executableDir() + "/.bro_settings.json";

// resolveLaunchTarget accepts an app directory, a project directory, or a
// bro.json of either kind, and returns false when the path is neither — so
// trying candidate locations in order actually works.
const std::string exe = bro::engine::executableDir();
bool found = false;
for (const char* rel : { "/ui", "/../../ui" }) {      // packaged, then build tree
    if (bro::engine::resolveLaunchTarget(exe + rel, config)) { found = true; break; }
}
if (!found) return 1;

bro::engine::publishLaunchEnv(config);   // BRO_EXE_DIR / BRO_APP_DIR / BRO_PROJECT_ROOT

bro::engine::Engine engine(config);
engine.run();
```

## Adding your own `bro.*` namespace

`EngineConfig::installHostBindings` runs during JS init, after every built-in
binding and before any app script — once per realm, so reloads and `<iframe>`
sub-documents get it too.

```cpp
config.installHostBindings = [](JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");

    JSValue ns = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ns, "available", JS_TRUE);
    JS_SetPropertyStr(ctx, ns, "doThing", JS_NewCFunction(ctx, js_doThing, "doThing", 1));
    JS_SetPropertyStr(ctx, broObj, "myapp", ns);

    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
};
```

Keep it to installing bindings — the Engine is still mid-init when this runs.

## Getting the `Engine*` from a binding

The installer hook is `void(JSContext*)`, and in headless the host never sees
the Engine at all (`runHeadless` builds it internally). To get from a realm to
the engine that owns it:

```cpp
#include "engine/engine.h"

bro::engine::Engine* engine = bro::engine::engineForContext(ctx);
if (!engine) return JS_ThrowInternalError(ctx, "no Engine for this realm");
```

Main thread only. Answers for the app realm — including inside the installer
itself — and `nullptr` for sub-document and system-panel realms, so check it.
Don't cache it across a `location.reload()`: the realm is replaced and the
installer runs again on the new one.

## Building the page from C++

An app whose JS was compiled away (an AOT-compiled app has no `app.js` to run)
has to do from C++ what `ui/app.js` would have done. The DOM half is ordinary
public API:

```cpp
auto* doc = engine->document();
auto* canvas = doc->createElement("canvas");
canvas->setAttribute("width", "1280");
canvas->setAttribute("height", "720");
canvas->style().setProperty("width", "1280px");
doc->body()->appendChild(canvas);          // enters layout on its own
```

`dom::Node::appendChild` / `insertBefore` / `removeChild` invalidate layout
themselves, so nothing else is needed to make the element render. They do not
run script: custom-element `connectedCallback` and MutationObserver delivery
belong to the JS bindings, as does cross-document adoption. Host C++ inserting
nodes it created in the same document needs none of the three.

For a 3D scene, `canvas.getContext('scene')` has a C++ equivalent:

```cpp
bro::scene::SceneGraph* scene = engine->createSceneContext(canvas);
if (!scene) { /* no GPU, or BRO_WITH_3D off — same null getContext returns */ }
```

This is the *same function* the `getContext('scene')` factory calls, so the two
cannot drift, and it is idempotent per canvas — asking twice, or asking from
both sides, yields the one SceneGraph.

Do not build a `scene::SceneGraph` yourself. A graph created with `make_unique`
is not registered with the engine, so the frame loop never renders it and the
compositor never sees it; the symptom is a blank canvas that looks like a
renderer bug. `createSceneContext` is what does the registration.

## Listening for events from C++

The other half of a page `ui/app.js` would have built is the listeners on it.
`window.addEventListener` and `element.addEventListener` both have a C++ form
that reaches the *same* dispatch the JS ones reach — same event path, same
capture/bubble phases, same shadow retargeting, same cancellation.

```cpp
#include "dom/event_target.h"   // EventCallback, ListenerOptions, ListenerHandle
#include "dom/event.h"          // Event, MouseEvent, KeyboardEvent, …

// window
dom::ListenerHandle Engine::addWindowEventListener(const std::string& type,
                                                   dom::EventCallback cb,
                                                   dom::ListenerOptions opts = {});
bool Engine::removeWindowEventListener(dom::ListenerHandle handle);

// element
dom::ListenerHandle dom::Element::addEventListener(const std::string& type,
                                                   dom::EventCallback cb,
                                                   dom::ListenerOptions opts = {});
bool dom::Element::removeEventListener(dom::ListenerHandle handle);
```

where

```cpp
using dom::EventCallback = std::function<void(dom::Event&)>;
struct dom::ListenerOptions { bool capture = false; bool once = false; };
struct dom::ListenerHandle  { uint64_t id = 0; explicit operator bool() const; };
```

The compiled-away app's

```js
window.addEventListener('resize', () => {
    camera.aspect = window.innerWidth / window.innerHeight;
    camera.updateProjectionMatrix();
});
canvas.addEventListener('click', onClick);
```

becomes

```cpp
resizeListener_ = engine->addWindowEventListener("resize", [this](dom::Event&) {
    camera_->setAspect(float(engine_->contentWidth()) / engine_->contentHeight());
});
clickListener_ = canvas->addEventListener("click", [this](dom::Event& e) {
    auto& me = static_cast<dom::MouseEvent&>(e);
    onClick(me.clientX(), me.clientY());
});
```

**Keep the handle.** It is the only way to detach, and a listener attached
inside a function that runs more than once has to detach first or it stacks up.
`removeEventListener` returns whether the handle named a live listener, so a
double-remove is detectable rather than silent. A default-constructed handle is
falsy and safe to pass.

### Ordering against JS listeners

**Registration order, across both kinds.** Every registration on either side —
`el.addEventListener(...)` from script, `el->addEventListener(...)` from C++,
`window.addEventListener` either way — takes a number from one shared counter,
and dispatch merges the two lists on it. A C++ listener registered between two
JS ones runs between them:

```js
el.addEventListener('click', first);   // 1
```
```cpp
el->addEventListener("click", middle); // 2
```
```js
el.addEventListener('click', last);    // 3   →  first, middle, last
```

The DOM's own ordering rules still apply on top of that, unchanged: capture
listeners on the way down, bubble listeners on the way up, both at the target,
and inline `on*` attributes / `el.onclick` properties last on their element.

### The event object

The callback gets a real `dom::Event&`, the same object the JS listeners' event
mirrors, so:

- `type()`, `timeStamp()`, `bubbles()`, `cancelable()`, `isTrusted()`,
  `eventPhase()`, `defaultPrevented()` all read what you would expect.
- `target()` is retargeted per scope exactly as `event.target` is in JS: a
  listener outside a shadow tree sees the host, not the node that was hit.
  `currentTarget()` is the element the listener is on.
- `preventDefault()`, `stopPropagation()` and `stopImmediatePropagation()`
  affect the rest of dispatch identically to the JS calls — JS listeners
  later in the same dispatch see `event.defaultPrevented === true`, the walk
  up the tree stops, and the engine's default actions (form submission,
  `<details>` toggling, label activation) are suppressed.
- `dynamic_cast` to `MouseEvent` / `KeyboardEvent` / `WheelEvent` / `DragEvent`
  / … for the typed payload, the same way `js/event_dispatch.cpp` does. Cast
  defensively: `click` is a `MouseEvent`, but a `CustomEvent` dispatched from
  script arrives as a plain `Event`.

Window events are the one place with a caveat. `window` is not an `Element`, so
for an event fired *at* the window — `resize`, `gamepadconnected`, `message` —
`target()` and `currentTarget()` are null; an event that reaches the window by
*bubbling out of the tree* keeps the target it had. And `resize` carries no
dimensions on either side of the boundary: read the new size from the engine
(`contentWidth()` / `contentHeight()` / `viewportWidth()`), which is what
`window.innerWidth` is reporting to the JS listener beside you.

Payload a *script* hung on an event object does not cross into C++.
`window.dispatchEvent(new CustomEvent('x', { detail }))` reaches a C++ window
listener with the right `type` and cancellation wired up in both directions,
but `detail` is a JS value with no C++ representation and is not on the
`dom::Event`. If the host needs structured data, dispatch the event from the
host (`js::dispatchWindowEvent`) with an `Event` subclass of your own, or pass
it through a binding.

### Which realm

`Engine::addWindowEventListener` is the **app realm**. `<iframe>`
sub-documents, `bro.window.open()` secondary windows and system panels are
separate realms with separate window listeners; reach those through their own
document:

```cpp
doc->windowListeners().add("resize", cb, opts);   // dom::Document
doc->windowListeners().remove(handle);
```

Element listeners have no such question — they live on the element.

Listeners are owned by the DOM object they are on and die with it. A
`location.reload()` builds a new `Document`, so window listeners registered on
the old one are gone; re-register from `installHostBindings`, which runs again
on the new realm.

### Without a JS realm

Element and window dispatch both run their C++ listeners with no `JSContext` at
all (`js::dispatchDomEvent(nullptr, …)`, `js::dispatchWindowEvent(nullptr, doc,
…)`): same path building, same phases, same retargeting. Only the JS-defined
parts are skipped, because they cannot exist — registered JS listeners, inline
`on*` attributes, and `el.onclick` properties. In practice every realm bro
builds today has a context; this matters only if you drive a `dom::Document`
yourself.

## Playing formats bro doesn't ship

`video/media_backend.h` is the seam. Register a demuxer and its decoders and
every path that plays media picks them up, `<video>` included:

```cpp
#include "video/media_backend.h"

bro::video::MediaBackend backend;
backend.name = "mine";
backend.priority = 100;          // bro's built-in WebM backend is 0
backend.open = [](const std::string& path) -> std::unique_ptr<bro::video::MediaSource> {
    auto src = std::make_unique<MySource>();
    if (!src->open(path)) return nullptr;   // not our format — stay quiet
    return src;
};
backend.makeVideoDecoder = [](const bro::video::TrackInfo& t) { ... };
backend.makeAudioDecoder = [](const bro::video::TrackInfo& t) { ... };
bro::video::registerMediaBackend(std::move(backend));
```

Register **before** constructing the Engine, so the first document's first
`<video>` already sees it.

Rules worth knowing:

- Backends are tried highest priority first, and one wins only if it both opens
  the container *and* can decode something in it. A backend that opens an `.mkv`
  it cannot decode falls through to the next instead of failing the load.
- `open` returns `nullptr` **without logging** when the format simply isn't
  yours. Reserve logging for a file you recognised but could not read.
- A source is adopted if **either** its picture or its sound can be decoded. A
  file with no video track plays as sound with no picture (`videoWidth` and
  `videoHeight` read 0), and a file whose audio codec you do not handle plays
  silently. Only a source where neither works is handed on to the next
  backend.
- Decoders come from the same backend that opened the source: `codecPrivate` is
  written by that demuxer and packets carry its framing.
- Implement `VideoDecoder::drain()` if your codec reorders. The pipeline calls
  it once the demuxer runs out of packets, and then keeps pulling `nextFrame()`
  until the buffer is empty. H.264 and HEVC hold their whole DPB back — sixteen
  pictures — waiting for a packet that decides the order, so a decoder that
  ignores `drain()` never shows the last second of any file it plays. For
  libavcodec it is `avcodec_send_packet(ctx, nullptr)`. `flush()` must clear
  the drained state too, or a seek away from the end will not decode.
- `TrackInfo::rotationDegrees` is how far the decoded picture has to be turned
  **clockwise** to be the right way up: 0, 90, 180 or 270, and 0 — the default —
  when the container says nothing. Fill it in if you can read one; a phone
  records landscape frames and writes the correction into the container, and
  without this a portrait clip plays on its side. `width`/`height` stay the size
  of the frames you decode: the swap is presentation, and `<video>` does it
  (`videoWidth`/`videoHeight` report the shown size, and the picture is turned
  by a transform on the quad rather than by a pass over the pixels). Anything
  that is not a quarter turn is refused and read as 0, because a size can be
  swapped or not and there is no third answer.
- `TrackInfo::backendPrivate` is a `shared_ptr<void>` your source can hang
  anything on for your decoder to read back. `codecPrivate` is a flat blob,
  which is all WebM needs; a richer decoder usually wants a whole parameter
  struct, and flattening it loses the details that make the stream decodable.
- Implement `AudioDecoder::setOutputFormat(rate, channels)` if you can resample.
  That is what decides whether `<video>` **streams** its audio or decodes the
  whole track into memory at load — see `layout/el_video.cpp`.
- Implement `MediaSource::setActiveTracks()` if your demuxer can skip streams.
  A player opens the file twice (once for video, once for audio) and without it
  each instance reads and discards the other's packets.

## Scripting it headlessly

`engine/headless_driver.h` is bro-headless itself, callable. You get the same
`screenshot()` / `advanceTime()` / `flush()` / `assert()` globals, the same REPL
and the same exit-code behaviour, with your bindings and backends installed:

```cpp
#include "engine/headless_driver.h"

int main(int argc, char* argv[]) {
    bro::engine::HeadlessHooks hooks;
    hooks.programName = "myapp-headless";
    hooks.beforeEngine = [] { registerMyBackend(); };
    hooks.installHostBindings = installMyBindings;
    return bro::engine::runHeadless(argc, argv, hooks);
}
```

This is usually how you test the app at all — a windowed binary has no way to
tell you what it drew.

## Pitfalls

- **`CMAKE_SOURCE_DIR` is the top-level project, not yours.** In an embedded
  build it points at the *host* application. bro uses `bro_SOURCE_DIR`
  throughout for this reason; if you vendor other libraries, check theirs.
- **`process.cwd()` is where the user ran the binary**, not where the app lives,
  and `__dirname` is undefined in app scripts. Use `bro.appDir` and
  `bro.resolvePath()`.
- **The QuickJS context must outlive every DOM element** — elements hold JS
  function references. Let `~Engine()` do the teardown; don't destroy services
  early.
