# Desktop sibling libraries: JS API plan

This page plans the bronze JS bindings (`<name>_api`) for the desktop-environment sibling libraries listed in [ecosystem.md](ecosystem.md). Without these bindings a bro app cannot reach the system services, files, displays, credentials or settings those libraries provide. This is the first foundational gap listed in [desktop-roadmap.md](desktop-roadmap.md).

---

## 1. Conventions every binding follows

These are the conventions the engine siblings already use (broimage, brotensor, broaudio and others). Copy from them; do not reinvent.

### Memory safety under bronze's moving GC
- **Raw values go stale.** Any allocating bronze call (`getProperty`, `createObject`, `fromUtf8`, throwing) can relocate heap cells. A raw `Value` held across one is stale.
- **Pin what you hold.** JS values held across allocating calls are pinned (`Rooted<>` within a call, `Persistent` slots for longer-lived references such as prototypes and callbacks).
- **Typed arrays.** A `TypedArrayView` roots the array. Its raw `data` pointer is filled by `resolveViews()` after the binding's last allocating call, immediately before the native work. See `broimage/src/api/host_image_internal.h`.
- **Native state wrapped in a JS object** is a handle created with `makeHandle(data, dtor, when)`. bronze calls its destructor at finalisation.

### Library layout
- **Static library.** Each sibling builds `<name>_api` from `src/api/`, linking the sibling's core library and `bronze_runtime_shared`. bronze and brass resolve like any other dependency, so a standalone build takes `../bronze` and `../brass` when present and their mains otherwise.
- **Public header.** `include/<name>/api.h` is a trampoline to `src/api/api.h`.
- **Shared helpers.** `host_class.{h,cpp}`, `object_builder.h` and `arg_reader.h` are sibling-local copies, because the siblings stay standalone. Copy them verbatim from broimage rather than rewriting them; `HostClass::inherit` keeps `instanceof` working.
- **Binding files** are `native_<area>.cpp`, each under 1000 lines.
- **Tests.** Each sibling's tests live in `tests/test_<name>_api*.cpp` and run an isolated bronze realm, as `broimage/tests/test_image_api*.cpp` do.

### Mounting in bro
- **One call site.** bro calls `install<Name>()` exactly once per realm, from `installSiblingApis` in `src/bronze_host/host_sibling_apis.cpp`, after `bro`, `__bro` and `__bro_native` exist. Never call it anywhere else.
- **One feature flag per library:** `BRO_WITH_<NAME>`, linked in `src/bronze_host/CMakeLists.txt`. Assign each new flag to a profile in [build-options.md](build-options.md).
- **Compiled out.** A namespace that is compiled out installs the `{ available: false }` stub, like every other optional bro feature.
- **Unsupported platform.** A namespace on a platform its library does not support is installed and reports `available: false` with a `reason` (broseat, browl and broportal on Windows, for example). It never fakes results.
- **Docs.** Each namespace gets an annotated `docs/<name>-api.js` and a row in the API table in CLAUDE.md, like the existing API docs.

### Async and events
- **Threads.** Blocking work (scans, copies, thumbnail generation, PAM, D-Bus round trips) runs off the JS thread.
- **Queues.** Results and events come back through each library's own queue of value snapshots. There is no single queue type yet:
  - brosys, brovfs and brothumb have `event_queue.h` with `set_wake`;
  - others have their own queues (brocompositor `events.h`, broseat, browl, brocred, brokeys);
  - broconf, broportal and brocas have none and need a small one.
- **Delivery.** The binding drains the queue on the JS thread from a frame pump registered with `Engine::addFramePump`. Callbacks fire and promises settle there.

---

## 2. Who may call what: a trust model comes first

Several of these libraries are dangerous in the hands of an arbitrary app. Examples:
- `brocred`: verifying passwords, reading secrets, answering polkit prompts;
- `broseat`: switching VTs, locking the session;
- `brodisplays`: changing modes;
- `brocompositor` and `browl`: controlling other apps' windows, capturing the screen;
- `broportal`: answering other apps' portal requests.

A bro app is currently fully trusted native code. The desktop needs two tiers before these bindings ship.

**Ordinary apps** get read-only or user-mediated surfaces. Examples:
- reading the theme and settings they own;
- launching an app;
- a thumbnail of a file the user opened;
- secrets stored under their own app id.

**Shell apps** (panel, launcher, settings, lock screen, file manager, portal backend) declare the privileged namespaces they need in `bro.json`. bro installs those namespaces only for shell apps that come from the trusted desktop install location.

Design this once, in bro, before the Group B and Group C bindings below.

**Already in bro; extend, don't duplicate:**
- `bro.window.getDisplays()` and `window.screen` already report displays. `bro.displays` is the settings and configuration surface; the existing read APIs stay and can be backed by brodisplays.
- `navigator.getBattery()` stays. It can read brosys power state.
- Global hotkeys, tray icons, notifications and taskbar progress for the app's own window belong in `bro.window` (an open item on the roadmap). `bro.sys.tray` and `bro.sys.notifications` below are the shell side: hosting other apps' tray items and showing other apps' notifications.

---

## 3. Library status

| Library | Binding | Namespace | Role in the desktop |
| :--- | :--- | :--- | :--- |
| brovfs | needed | `bro.vfs` | File operations: scan, copy/move/trash with undo, directory models, watching, MIME sniffing, volumes. |
| brosys | needed | `bro.sys` | System services: power, audio devices and volume, network and Wi-Fi, Bluetooth, the notification server, the tray host. |
| broapps | needed | `bro.apps` | App catalog, `.desktop` parsing, icon resolution, file associations (read; setting defaults is not implemented yet), recent files, scoped launch. |
| broconf | needed | `bro.conf` | Layered settings store with schemas, and file/D-Bus change watching. |
| brokeys | needed | `bro.keys` | Keybinding engine: chords, `when` clauses, VS Code-style keybindings.json. It is not global hotkey registration; that belongs to `bro.window` or the compositor. |
| brosearch | needed | `bro.search` | Fuzzy matching, file walking and grep, for the launcher and the file manager. |
| brothemes | needed | `bro.themes` | Colour schemes plus WCAG/APCA contrast. The terminal already uses it natively; desktop theming and the settings app need it from JS. |
| brodisplays | needed (shell) | `bro.displays` | Display configuration: modes, scale, test-then-revert, night light, brightness and backlight, EDID. |
| brocred | needed (shell) | `bro.cred` | Lock-screen authentication (PAM / LogonUser), biometrics, secret store, polkit agent. |
| brothumb | needed | `bro.thumb` | Thumbnails: XDG cache, image/PDF/native generators. |
| broa11y | engine first | `bro.a11y` | Mainly an engine job: bro's DOM must export its accessibility tree through broa11y automatically. A JS surface for live-region announcements and custom roles comes second. |
| broseat | needed (shell, Linux) | `bro.seat` | Session state, lock/unlock, VT switching, idle inhibition, autostart. |
| brocompositor | after bro is the compositor | `bro.compositor` | Window-management policy in JS: workspaces, placement and tiling, the task switcher. Meaningful once bro runs as the Linux compositor (or the Windows shell backend). |
| browl | after bro owns a Wayland surface | `bro.wl` | Panel, dock and lock surfaces under another compositor (layer-shell, foreign-toplevel, session-lock, screencopy). `setLayerRole(window)` needs bro's own Wayland platform, because SDL windows cannot take a layer-shell role. |
| broportal | needed (shell, Linux) | `bro.portal` | Portal backends implemented as bro dialogs (file chooser, screenshot, screencast). The library answers errors until the host supplies these callbacks. |
| brocas | later | `bro.cas` | Content-addressed storage (FastCDC, Merkle manifests, sync). No desktop app needs it yet. |
| brodmabuf | none | — | C++ only: GBM allocation, DMA-BUF import/export, KMS scanout for bro's presenter and brocompositor. Raw kernel file descriptors never reach JS. |

**Image results.** The siblings cannot construct bro's `ImageBitmap`, which is a bro type. `bro.thumb` and `bro.wl.captureOutput` return pixels (width, height, `Uint8Array`) or a file path, and bro-side JS wraps them, as `bro.image` results are wrapped today.

---

## 4. Proposed JS shapes

These are starting points for each binding's design, not contracts. The binding's `docs/<name>-api.js` becomes the contract.

### Group A: ordinary-app surfaces
```js
// bro.vfs
bro.vfs.scan(path, options) -> Promise<Entry[]>
bro.vfs.copy(src, dst, options) / move(src, dst, options) -> Promise<OpResult>
bro.vfs.trash(path) / restoreTrash(id) / listTrash() -> Promise<TrashEntry[]>
bro.vfs.undo() / redo() / canUndo() -> boolean
bro.vfs.watch(path, (events) => {}) -> WatcherHandle
new bro.vfs.DirectoryModel(path, options) // sorted/filtered model for file managers

// bro.apps
bro.apps.list() -> AppInfo[] / search(query) -> AppInfo[]
bro.apps.launch(appId, { args, env, cwd, files }) -> Promise<ProcessHandle>
bro.apps.resolveIcon(iconName, { size, theme }) -> string // image path
bro.apps.getDefaultApp(mimeType)
bro.apps.getRecent() -> RecentItem[]

// bro.conf (an app's own keys; desktop-wide keys are shell-only to write)
bro.conf.get("desktop.theme.mode") -> "dark"
bro.conf.set(key, value) -> Promise<void>
bro.conf.watch("desktop.theme", (key, newVal, oldVal) => {}) -> WatchHandle
bro.conf.registerSchema(schemaDefinition)

// bro.keys
const engine = new bro.keys.Engine();
engine.addBinding({ key: "Ctrl+Shift+P", command: "palette.show", when: "!inputFocus" });
engine.loadJson(keybindingsJsonString);
engine.setContext("inputFocus", true);
engine.feed(keyboardEvent) -> DispatchResult

// bro.search
bro.search.fuzzy(query, candidates, options) -> Match[]
bro.search.files(root, { query, ignore }) -> AsyncIterator<Path>
bro.search.grep(root, pattern, options) -> AsyncIterator<Hit>

// bro.themes
bro.themes.list() / get(name) -> Scheme
bro.themes.contrast(fg, bg, { method: "wcag" | "apca" }) -> number

// bro.thumb
bro.thumb.get(path, { size: "normal" }) -> Promise<{ width, height, pixels, source }>
bro.thumb.clearCache(options)
```

### Group B: shell surfaces (privileged)
```js
// bro.sys
bro.sys.power.getState() / on('change', fn) / inhibit(reason) -> InhibitHandle
bro.sys.audio.getDevices() / setVolume(id, vol) / setDefaultSink(id)
bro.sys.network.getState() / scanWifi() -> Promise<Wifi[]> / connectWifi(ssid, secret)
bro.sys.bluetooth.getDevices() / startDiscovery() / pair(addr)
bro.sys.notifications.on('notify', fn) / dismiss(id) / invokeAction(id, key) // the notification server
bro.sys.tray.getItems() / on('itemAdded', fn) // StatusNotifierItem host

// bro.displays
bro.displays.getSnapshot() -> DisplaysSnapshot
bro.displays.testConfig(change, { revertAfterMs: 10000 }) -> Promise<void>
bro.displays.confirmConfig() / revertConfig()
bro.displays.setNightLight({ enabled, temperature, schedule })
bro.displays.setBrightness(displayId, percent)

// bro.cred (the lock screen and polkit agent only; secrets are app-scoped for ordinary apps)
bro.cred.authenticate(username, password) -> Promise<boolean>
bro.cred.getBiometrics() -> BiometricCapabilities
bro.cred.onPolkitRequest((req) => { req.submit(password); /* or req.cancel() */ })
bro.cred.getSecret(service, account) / setSecret(service, account, secret, meta)

// bro.seat (Linux)
bro.seat.getSessionState() -> { active, locked, vt, id, user }
bro.seat.on('lock', fn) / on('unlock', fn)
bro.seat.switchVt(vt) / lock()
bro.seat.inhibit("idle", "Video playback") -> InhibitHandle
bro.seat.runAutostart() -> Promise<AutostartResult[]>

// bro.portal (Linux; bro hosts the portal dialogs)
bro.portal.onFileChooser(async (req) => req.respond({ uris: await pickFiles(req.options) }));
bro.portal.onScreenshot(async (req) => { ... });
```

### Group C: once bro is the compositor or owns a Wayland surface
```js
// bro.compositor
bro.compositor.getWindows() / getWorkspaces()
bro.compositor.switchWorkspace(id) / moveWindow(id, bounds) / focusWindow(id)
bro.compositor.setLayoutMode(workspaceId, "tiling" | "floating" | "columns")
bro.compositor.on('windowCreated', fn) / on('windowClosed', fn)

// bro.wl
bro.wl.getToplevels() / on('toplevelAdded', fn) // tl.activate(), tl.close()
bro.wl.setLayerRole(window, { layer: "top", anchor: ["top", "left", "right"], exclusive: true })
bro.wl.captureOutput(outputId) -> Promise<{ width, height, pixels }>
bro.wl.acquireSessionLock() -> LockHandle
```

### Group D: accessibility
`bro.a11y.announce(text, { priority })`, plus custom-role hooks for canvas-drawn UI. The bulk of broa11y's integration is engine-side, exporting the DOM's accessibility tree, and is tracked as its own roadmap item.

---

## 5. Size

Each binding consists of:
- CMake and the trampoline header (~100 LOC);
- copied helpers (~500 LOC, copied rather than written);
- the `native_*.cpp` files and the queue drain;
- a test suite.

These are estimates, to be corrected as bindings land.

| Library | Binding LOC | Test LOC | Total |
| :--- | ---: | ---: | ---: |
| brovfs | 3,200 | 800 | 4,000 |
| brosys | 3,800 | 900 | 4,700 |
| broapps | 2,200 | 600 | 2,800 |
| broconf | 1,400 | 400 | 1,800 |
| brokeys | 1,800 | 500 | 2,300 |
| brosearch | 1,000 | 300 | 1,300 |
| brothemes | 600 | 200 | 800 |
| brodisplays | 2,200 | 600 | 2,800 |
| brocred | 2,000 | 500 | 2,500 |
| brothumb | 1,400 | 400 | 1,800 |
| broseat | 1,500 | 400 | 1,900 |
| broportal | 2,400 | 600 | 3,000 |
| brocompositor | 3,200 | 800 | 4,000 |
| browl | 2,400 | 600 | 3,000 |
| broa11y (JS surface only) | 800 | 300 | 1,100 |
| bro host side (flags, mounts, stubs, docs) | 1,500 | — | 1,500 |
| **Total** | | | **~39,000** |

The trust model and broa11y's engine-side tree export are sized separately. brocas is deferred.

---

## 6. Order of work

Work proceeds in chunks of about 4k LOC per agent, at most three agents at once, with verification and commits between chunks. Each chunk ends with the sibling's own ctests passing on Windows and Linux, the binding mounted in bro behind its flag, and a headless bro test calling it from JS.

1. **The pattern, on one library.** broconf (small, cross-platform, no privileges) plus bro's host-side plumbing: flag, mount, stub, docs file, headless test. Every later binding copies this chunk.
2. **What the next apps need (ordinary-app surfaces):**
   - brovfs, brothumb and brosearch (file manager);
   - broapps and brokeys (launcher, keybindings);
   - brothemes.
3. **Trust model in bro.** Shell-app declaration in `bro.json`, a trusted install location, and gating of privileged namespaces. The trust model must land before step 4.
4. **Shell surfaces:** brosys, brodisplays and brocred (settings app, lock screen, notification centre); then broseat and broportal on Linux.
5. **After bro runs as the Linux compositor** (roadmap item 2): brocompositor and browl.
6. **broa11y:** the engine-side tree export first, then the small JS surface.

When a sibling's binding lands, bro starts depending on that sibling:
- declare it in `cmake/bro_pins.cmake`'s `bro_dependencies()` list along with its transitive siblings, and add it with `bro_dependency(<name> ...)` in `third_party/CMakeLists.txt`;
- mark it `dep` in `scripts/repos.txt`;
- add its namespace to [ecosystem.md](ecosystem.md) and its API file to the table in CLAUDE.md.

After that, bro builds the sibling's main (or `../<name>`); there is no pin to keep current.
