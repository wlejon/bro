# App isolation (design proposal)

**Status: proposed 2026-10-26; step 1 of the order of work (below) is built.**
Step 1 replaced the location and environment rules of `desktop_trust.cpp`,
grants by app id in `permissions.json`, and the applet certificates
(`app.cert.json`, `bronze verify-applet`, broapps' certificate inspection,
helm's badge), which are deleted. What it built, and where it decided
differently from this proposal, is under "Order of work"; the user-facing
rules are in [apps.md](apps.md), Permissions. The rest is still a design to
discuss.

## Goals

1. **Contain mistakes (primary).** An app's bug never takes helm or another app
   down and never corrupts another app's state. Uncaught exceptions, runaway
   loops, running out of memory, misusing an API: each ends in an error that
   names the app, what it was doing and where, and the app is *disconnected*
   rather than the process crashing.
2. **Resisting hostile apps must stay possible.** Nothing in the design may
   assume an app is friendly. Where it falls short of that today, the gap is
   listed (below) rather than designed in.
3. **Per-app permission control.** The user sees and changes what each
   installed app may do, in helm, and is asked at the moment an app first needs
   something.
4. **Share what is in memory.** helm runs helmapps inline, in its process: one
   heap, one GPU device, one copy of the engine's caches, of shared code (the
   kit in `/lib`) and of model weights in VRAM, and no process start on launch.

Not goals for now: defending against code that runs as the user outside bro,
kernel-level sandboxing, and timing side channels.

## What is wrong with today's trust

- Only ten namespaces are gated (`compositor`, `wl`, `displays`, `seat`, `sys`,
  `cred`, `portal`, `clip`, `pulse`, `remote`). These are open to every app:
  - `fs`, `child_process` and `process.env`;
  - `fetch` and the network;
  - `<terminal>`, which spawns shells.

  Any app can therefore rewrite the permissions file, edit helm's UI, or
  spawn `bro` with `BRO_TRUST_ALL=1`.
- Grants in `permissions.json` are keyed by the id in the app's own `bro.json`,
  which any app can claim.
- Trust by install location, where the dev setup makes the location
  user-writable (`apps/` and `system/` beside a build tree's executable), plus
  environment switches that child processes inherit (`BRO_TRUSTED_APP_DIR`,
  `BRO_TRUST_ALL`).
- Grants are a whole namespace at a time: listing processes needs `bro.sys`,
  which also brings power-off.
- Nothing asks the user.
- Applet certificates are JSON the app's author writes; they attest nothing
  bro checks against anything it trusts.
- `<iframe>` sub-documents share one global object and every intrinsic; only
  expandos are swapped per realm scope (`host_realm_scope.cpp`). That keeps
  documents apart, but it is no boundary between them.

## The model

An app instance runs in a **compartment**:

- its own global object and module registry (a bronze `Realm`);
- its own `bro` root holding only the capabilities it was granted;
- its own document, composited into the host's page by an element (say
  `<appview>`);
- its own brass fiber;
- its own **resource ledger** of everything native it created.

Compartments share the engine, the GPU device, the caches, and the
**frozen** intrinsics.

There is no ambient authority. Anything that acts outside the compartment is
a **capability**: an object the compartment holds because it was granted.
A compartment can do only what it can reach.

The **host is the authority**. In helm, that is the shell: it holds the
grants per app identity, draws the prompts, and revokes. A plain `bro <app>`
running one app per process applies the same model, with the bro process as
the authority, using native dialogs.

## Compartments

**Intrinsics are shared and frozen.** Before any app code runs, the host
freezes every built-in constructor, prototype and their statics (what SES
calls `lockdown`). A compartment cannot change what another one, or helm,
calls. Freezing costs the helmapps nothing; none of them patches a built-in
prototype. It should help the JIT, too: a frozen prototype never bumps the
proto epoch, so the caches keyed on it never miss for that reason.

**What else bronze shares per thread has to be audited and split per
compartment, or frozen:**
- the host-global registry (`rtHostGlobalEntries`);
- HostClass prototypes;
- bro's per-process tables in `bronze_host` (realm scopes, the content-window
  proxies, parsed documents, the privilege tables).

`Realm` already carries its own global object and module registry.

**Host objects carry their owning compartment.** A DOM node, a canvas or a
scene handle passed into another compartment is refused at `unwrap`, the
same check the `Brand` table already makes for type.

**Nothing mutable crosses the boundary.** helm and an app talk over a message
channel (structured clone) and through capability objects helm grants. A
capability call runs in helm's compartment behind a membrane: arguments are
cloned in, results cloned out, and a callback becomes a capability in turn.
This replaces `wrapRealmFunction` and the shared `contentWindow` for apps.

`<iframe>`s are compartments as well, so there is one mechanism (open
question 1). A page's own frames are of its app, and they reach one another
directly.

## Capabilities and permissions

Every ambient power becomes a capability, not just the ten namespaces:
- `require('fs')` returns the compartment's scoped `fs`;
- `child_process`, `process.env`, `fetch` and the network, and `<terminal>`
  are all granted or absent.

Capabilities are finer than namespaces. For example, `sys.processes.read`,
`sys.power` and `sys.network` are separate.

**Granted without asking:**
- the app's own data, config and cache directories;
- its DOM, canvas, WebGL and scene;
- audio output, timers, `bro.conf` under its own keys.

**Asked for, or denied:**
- files outside its own directories;
- microphone and camera, screen capture, network;
- spawning processes and terminals;
- each system capability.

Shell capabilities (`compositor`, `seat`, `cred`) go only to the shell.

**The manifest says what an app may ask for** (`permissions`). Declaring grants
nothing. A grant is *allow*, *ask* or *deny* per capability per app, revocable
while the app runs. A revoked capability goes dead: calls throw
`PermissionRevoked`, or the app is disconnected if it holds the capability
open (a stream, say).

**File access is the user's choice (the powerbox).** A file or folder the user
picks in a dialog the host draws is handed to the app as a capability for
exactly that file or folder. An app that wants "a file" never needs to be
granted "all files".

**Identity is the install record, not the id.** `bro --install` records the
id, the install path and a hash of what was installed. Grants attach to that
record, and a changed hash asks again. An app run from a source folder is
identified by its path and prompts like any other.

**Development mode is explicit.** It is a helm setting that grants every app
what it asks for, shown in the bar while on. It replaces `BRO_TRUST_ALL` and
`BRO_TRUSTED_APP_DIR`; nothing is decided by inherited environment variables.

**The grant store is the host's.** helm keeps it, and in-process apps cannot
write it, since they have no `fs` outside their own directories.

**helm's Settings gets an Apps page:** every installed app, each capability
and its state, when it was last used, and a revoke switch.

## Disconnects

A disconnect is how a mistake ends: the app stops, everything else goes on,
and the user sees why.

**Where one happens:**
- an uncaught exception (policy to decide: log and continue as a browser
  does, or disconnect after repeated ones);
- a time slice overrun that keeps recurring, at which point helm marks the
  app not responding and offers to stop it;
- the app's memory budget exceeded;
- a stack overflow;
- a native precondition failed;
- a capability revoked while held open.

**How it works:**
- Termination is uncatchable by the app. It unwinds the app's fiber
  (brass's zero-cost exceptions) and marks the compartment dead.
- All of the app's capabilities go dead, and its ledger is released.
- Its document is replaced by an error card naming the app, the reason and
  the site (function, file:line, the native being called).

**The resource ledger is the hard part.** Every native resource a compartment
creates is registered with it, and disconnecting releases all of them: DOM
nodes, GPU objects, canvases and scene graphs, terminal sessions, audio
streams, timers, watchers, Workers, model handles. Pieces exist today (realm
shutdown hooks, the siblings' `shutdown*Async` hooks); this makes them one
ledger. Each resource kind needs a test that disconnects an app in the
middle of using it, because teardown is where bro's bugs have historically
lived.

## Scheduling: no app can stall helm

**Each compartment runs on its own `brass::gc::Fiber` on helm's thread.** An
entry into the app resumes its fiber: an event, a timer, an rAF, a promise
job.

**A time-slice check at the safepoint polls** (loop backedges, function entry,
and the interpreter's equivalent) yields back to helm's scheduler once the
app's slice is spent. helm's frame goes on, and the app resumes on a later
turn. That is preemption on one thread with one shared heap.

**Native calls cannot be preempted,** so natives an app can reach must not
block:
- synchronous file I/O, model loads and synchronous inference become async or
  move to worker threads;
- a native call that runs past a threshold is reported, naming the native.

**An app's Workers stay threads with their own heaps,** as now. They belong to
the app's ledger and receive capabilities only by grant.

## The native boundary and where proofs fit

Isolation inside one process is only as good as what it trusts: bronze and
brass's generated code, bro's natives, and the sibling libraries. JS cannot
crash on its own. A null dereference throws and an out-of-bounds read returns
`undefined`. What can crash is the compiler wrongly removing a check, or
native code given bad input or having a bug of its own.

**Compiler certificates checked at load.** Every check the compiler removes
carries a certificate:
- brass's barrier and guard-elimination certificates;
- bronze's IL proofs for raw unboxes, pins and bounds.

Every check it cannot prove stays in as a guard. Today these are checked only
in Debug and test builds (`BRONZE_VERIFY_MIR_DEFAULT`; full MIR verification
was a quarter of load time). For compartment code the certificates are
checked in Release, alone, without the full verifier; that cost must be
measured. Then the generated code has exactly the property wanted: a proven
site cannot fail, and every unproven site has a guard.

**Native preconditions at every native entry.** Each native states its
preconditions:
- handle brand and owning compartment;
- ranges and lengths;
- lifetime (not torn down);
- thread.

The entry checks them, and a failure disconnects the app with a message naming
the native and the bad argument. The preconditions are declared once beside
the native's registration (`natives/<sub>/`, the siblings' `src/api/`) so
check and declaration cannot drift. Where the compiler proves a precondition
holds at a call site, the entry check is dropped with a certificate: the same
proof-or-guard pattern, carried over to natives. The call graph also says
exactly which natives each app can reach.

**What proofs cannot contain in-process:**
- a bug inside a native given valid input (a Skia null, a driver fault, a
  CUDA error, a decoder overrun);
- loss of the Vulkan device, which is lost for every compartment sharing it.

For the primary goal these are accepted. If they need containing, the riskiest
native work (ML inference, media decoding) moves to one shared service process
that all compartments reach through a capability, which still shares model
weights. That changes nothing in the capability model.

**Not part of the boundary:** the static hazard checker stays a lint, and
applet certificates are retired.

## Toward hostile apps

What the design must keep possible, and what it still lacks for it:

- **No ambient authority anywhere.** The capability catalogue above is
  required for that, not just for mistakes.
- **Grants bound to a verified install identity** (the install hash above).
- **A memory-safe trusted base:** certificates checked in Release, native
  preconditions fuzzed, and the shared service process for native code too
  risky to trust in-process.
- **Budgets for shared resources** (memory, GPU time) so one app cannot starve
  the rest.
- **Out-of-process compartments:** the same capabilities over a channel, for
  apps not trusted with in-process natives at all.

## Open questions

1. Do `<iframe>`s become compartments, removing realm-scope expando swapping
   (one mechanism)? *Answered (step 3):* yes. Every sub-document is a
   compartment with its own realm, and the swapping is gone. A page's own
   frames share its ownership group, so they reach one another as
   same-origin frames do; another app inline is an `<appview>`, behind the
   membrane.
2. Per-compartment memory accounting in one shared bronze heap: allocation
   attribution, and what a budget means at collection.
3. Can the interpreter tier poll for time slices cheaply, and do brass's
   tier-1 and tier-2 code poll at every loop backedge today or only where
   OSR and GC need it? *Answered (step 4):* every tier polls at every loop
   backedge now (brass's module interrupt poll), and function entries poll
   through the prologue's existing stack-limit check. The cost is below noise:
   one load and one compare per backedge (measurements under step 4).
4. Network granularity: on or off, or per host?
5. The exception policy: continue or disconnect.
6. The cost of freezing intrinsics at boot, and checking certificates in
   Release. *Freezing answered (step 2):* about 3 ms per thread (the walk
   freezes some 980 objects), paid once by the main thread and once by each
   Worker, and invisible in a headless app's start; compiled code is no slower
   (three.js in brobench shows no difference above noise), because a frozen
   object's caches keep hitting (measurements under step 2).
   Certificates are still open.

## Order of work

1. **The capability catalogue, and gating `fs`, `child_process`,
   `process.env`, the network and `<terminal>`**, in today's one-app-per-process
   bro. This contains mistakes there too. **Built** (2026-10-26):
   - The catalogue is one table, `src/engine/capabilities.cpp`: ask
     capabilities `files`, `net`, `spawn`, `mic`, `env`, `mpris`, `wl`,
     `displays`, `clip`, `pulse`, `remote` and `sys.<member>` (power, audio,
     network, bluetooth,
     notifications, tray, processes); shell capabilities `shell`,
     `compositor`, `seat`, `cred`, `portal`, `permissions`, granted only by
     the host executable (helm's main, `--grant`). The grants live in
     `AppPermissions` (`src/engine/app_permissions.cpp`), the store in
     `grant_store.cpp`.
   - Enforcement is at the natives, not in `require`. Every native that opens
     a path, connects, listens, starts a process or captures asks one gate,
     bronze's `embed/embed_access.h` (`AccessKind` Read, Write, Spawn, Net,
     Capture, EnvRead, EnvWrite, DesktopRead, DesktopWrite), which bro
     installs over `AppPermissions` (`src/bronze_host/host_permissions.cpp`).
     The askers: brokit's fs, sqlite, child_process, net, fetch, WebSocket
     and `process.env`; the terminal's spawn; broaudio's capture; brovfs,
     brosearch, brothumb, broapps and broconf; the media, model and game-AI
     siblings' file natives (bro.image, AudioContext and bro.ear, bro.mesh
     and bro.rigging, bro.themes, bro.tensor, bro.lm, bro.diffusion,
     brosoundml, bro.vision, bro.ai.game); bro's own natives through
     `host_access.h` (screenshots, recordings, encoders, bro.media, scene
     textures, bro.net, notification and tray icons, Workers; the headless
     driver's own writers are the developer's harness, ungated under
     bro-headless, gated in a window: `requireHarnessPathAccess`); and the
     page's subresources (`util::subresourceRefusal`: a path asks Read, an
     http(s) URL asks `net`, answered from what is decided, never a prompt
     from markup). The gated `bro.<ns>` objects are proxies over the live
     namespace (or its stub) that ask on first touch and refuse once revoked.
   - `process.env` (`env`): without it an app sees only who and where it is
     (`envAlwaysVisible` in `app_permissions.cpp`: the user and home, the
     temp and system folders, PATH and the shell, LANG/LANGUAGE/TZ and
     `LC_*`, the terminal and desktop, `XDG_*`, the machine's OS, CPU and
     name, bro's own `BRO_*`, and `CI`); tokens, keys and the rest are
     absent rather than refused, so enumeration and `in` agree. A write it may
     not make lands in an app-local overlay: the app reads it back and its
     children inherit it, but the process's environment is untouched.
   - `mpris` is its own capability: the media players listed say what the
     user is playing, and a published player can stand in for another.
     bro-headless keeps bro.mpris off the session bus altogether.
   - An app's own `bro.conf` section (`<appId>.*`) is always writable, as its
     own directories are; reading the desktop's keys and writing them need
     `files`.
   - The path check is cheap: `checkFs` makes the path absolute and lexically
     normal, resolves the parent once per directory (a cache cleared when the
     grants change) and checks only the leaf for a link, all outside the
     permissions lock. Windows, Release, a file in the app's data dir:
     `statSync` 87 us through the gate against about 70 us with no gate,
     `existsSync` of a missing file 25.5 against 12.8 (it was 56 us over
     before the cache).
   - Decided differently from the proposal: a grant is *allow* or *deny* (an
     undecided one asks), not allow/ask/deny;
     `sys.processes` is one capability, not `sys.processes.read`; `files`
     is one capability for everything beyond the app's own directories and
     what the user picked, and with it an app may read and write anywhere
     except bro's own state; identity is the install record (step 6), else
     the canonical folder; dev mode is the store's `devMode` flag
     (`bro --dev-mode on`, or helm's Settings), shown in helm's bar while it
     is on, and it also overrides the user's denials; the store is
     bro's own (`grants.json` in bro's config directory), which no app can
     write, rather than helm's.
   - Revocation while the app runs (pulled forward from step 5). Each running
     bro polls `grants.json`'s modification time and size every 250 ms of
     wall time (stamped before the read, a failed read skipped, so a write in
     progress is never taken for an empty store), on the frame pump; `bro
     --revoke-app`, `--reset-app` and helm's `bro.permissions.set` reach
     every running app that way. A change reloads the grants, and then:
     a call under a capability taken back throws an Error named
     `PermissionRevoked` (code EACCES); and what the app holds open under it
     is closed with that Error. The ledger is `embed::holdAccess` /
     `accessChanged` / `revalidateHeldAccess`: whatever opened a resource
     holds it there, and its revoke runs on the thread that holds it
     (the main thread or the Worker's own loop). Held: brokit's sockets,
     listeners, UDP sockets, WebSockets (closed 1008), fetches and their
     bodies, children (killed; 'error' before 'exit') and fs watchers;
     `<terminal>` sessions (a `permissionrevoked` event, the reason printed,
     then the kill); the microphone, mic taps and loopback captures
     (bro.mic's `onError`, bro.listen's `revoked`); bro.vfs watches,
     directory models and usage scans; bro.search walks.
   - Each capability's last use per app is recorded (`usage.json` beside the
     store, flushed with the frame pump, not per call), shown by
     `bro.permissions.list` and helm's page.
   - Not closed on revocation yet: a handle the app took from a gated
     namespace before (the proxy refuses only new reads off the namespace);
     a file already open; `getUserMedia` tracks (the capture stops, but no
     `ended` fires); one-shot jobs (a bro.vfs copy runs to its end); bro.net
     sockets, `VideoEncoder`/`GifEncoder` outputs and bro-ctl recordings,
     which do not hold yet; apps started through broapps' `launch` (their
     own processes, with grants of their own). Two processes changing
     `grants.json` at once: the last write wins.
2. **Frozen intrinsics;** per-realm host globals and HostClass prototypes.
   **Built** (2026-10-26):
   - Lockdown is bronze's `embed::lockdown()` (`runtime/lockdown.h`, and
     "Realms and lockdown" in bronze's `docs/internals.md`). It walks from the
     builtin globals and the intrinsics no global names (the iterator,
     generator and async prototypes) through properties, accessors and
     prototypes, and freezes all of it. bro calls it first thing in
     `installWebHostGlobals` and at the start of every Worker thread, before
     any install and before any script, bro's own `bro_core.js` included, so
     nothing in bro can lean on a patched intrinsic either. A plain bronze
     program never locks down.
   - The override mistake is fixed in the engine rather than by SES's
     accessor conversion: an inherited read-only data property on an object
     lockdown froze is shadowed by an own property on the receiver
     (`MyError.prototype.name = 'X'`, `obj.toString = f`), and the frozen
     object itself still refuses. Reads pay nothing for it.
   - Frozen objects keep their inline caches: a frozen object's layout can
     never change, so bronze's read, prototype-chain, enumeration and statics
     caches name it as they name any shape, and after the walk nothing bumps
     a prototype epoch on its account.
   - What the host installs is the realm's own and is not frozen: the host
     globals (bronze's registry is per `Realm` now; a new realm starts with
     none), and every HostClass prototype. bronze's `embed::HostClass` keeps a
     constructor and prototype per (class, realm); bro's own
     `bronze_host/host_class.cpp` does the same (it kept one pair per process,
     so a Worker's install overwrote the main thread's); and the 24 siblings'
     copies of `src/api/host_class.*` are gone, each now an alias of bronze's
     (bromesh and brogameagent keep their own `ObjectBuilder`, whose methods
     wrap every native, through a thin subclass). A destroyed realm drops its
     classes (`embed::addRealmDestroyHook`).
   - The one built-in patcher in the ecosystem was bro's own Intl
     (`host_intl.cpp`), which assigned `Number.prototype.toLocaleString`,
     the `Date` `toLocaleDateString`/`toLocaleTimeString` and
     `String.prototype.localeCompare`. It now gives them to bronze
     (`embed::setLocaleMethod`), and the frozen builtin calls them.
   - bro's realm-scope tables, audited: the scope expandos and baseline
     overrides, the content-window proxies and the parsed documents were
     already keyed by scope; the custom-element registry was one per process,
     so a frame's `customElements.define('x-row')` defined it for its parent
     too. It is one per scope now, dropped with the scope. Left for step 3:
     frames are still scopes of one bronze realm, and a removed `<iframe>`
     does not clear its scope's tables (a reload, a new `src` and a closed
     window do).
   - Tests: bronze `eval_lockdown_test` (everything reached frozen, refusals,
     the override fix at each tier, two realms sharing the frozen set, hot
     reads still hitting caches), `embed_global_object_test` and
     `embed_realm_test` (host globals, lazy globals and HostClass prototypes
     per realm; the locale hook); bro `bro_host_class_realm_test` (bro's
     HostClass per realm) and `tests/dom/test_iframe_realm_isolation.js`
     (a frame's patch of `Array.prototype` refused, the override fix, host
     prototypes still extensible, one custom-element registry per frame).
   - Measured (RTX 4090 box, Release, headless): the lockdown walk is 3.0 to
     3.2 ms; a headless start of `demos/dom-lab`, interleaved six times each
     way, was 1220 to 1262 ms wall locked and 1229 to 1259 ms not (no
     difference above noise). three.js in `../brobench` (the AOT-compiled
     app, 300 frames a scene), six runs each way interleaved, js ms per
     frame, median (best): many_meshes 129.4 (115.3) locked against 120.9
     (112.6) not, instanced 28.0 (23.4) against 28.8 (26.7), hierarchy 19.3
     (13.7) against 18.8 (15.0). The box was shared with other builds, and
     runs of one mode spread 15 to 40 %, so no difference shows in either
     direction: lockdown does not cost the compiled code, and a win from
     epochs that never bump is too small to see over three.js's own misses
     (its prototypes are its own, not the builtins').
3. **Compartment = Realm + document + ledger;** host objects owned by their
   compartment; `<appview>`. **Built** (2026-10-26):
   - **A compartment is a bronze realm** bound to one document and one
     `AppPermissions` (`src/bronze_host/host_compartment.h`). The app's
     document is the app compartment, on the default realm; every
     sub-document the engine hosts is a compartment of its own: an
     `<iframe>`, a secondary window, a system panel, an `<appview>`. Each
     realm installs its own host globals, `bro` root, host classes and
     module instances. The realm-scope swapping that made frames share one
     realm (`host_realm_scope.*`) is gone, which is step 8.
   - **Ownership groups answer open question 1.** An app's frames, windows
     and panels join the app's realm group (bronze `embed::joinRealmGroup`).
     Within a group they reach one another as the web's same-origin frames
     do: `contentWindow` and `contentDocument` are the frame's own,
     `parent`, `top` and `frameElement` the page's, and a DOM node passes
     between them. Each still has its own globals, and every call runs in
     the callee's realm, so a frame's function sees the frame's globals
     whoever calls it. An appview is a group of its own, and another app's
     handle reaching a native is a TypeError.
   - **The membrane** (`host_membrane.h`). Between apps only a window proxy
     crosses: `postMessage` and `closed`. A post goes as bytes and is read
     back in the receiver's realm, with its `source` the receiver's proxy
     for the sender. The page grants functions with `appview.expose(name,
     fn)`; inside the app they are `bro.app.host[name]`. A call clones its
     arguments into the page, runs there and clones the result back.
     Promises settle across, a function argument becomes a capability the
     other way, and errors cross as clones.
   - **`<appview>`** (`docs/appview-api.js`, `host_appview.cpp`). The app is
     who its `bro.json` says: its own `bro.app` (id, name, folders) and its
     own grants from its manifest and the grant store. The page's grants and
     folders never pass to it; bro's shared asset mounts do. An undecided
     capability is a `permissionrequest` event at the element, answered with
     `allow()` / `deny()`, later through `decide()` after `preventDefault()`,
     and by bro's own dialog when no listener answers. The page controls its
     lifecycle (`start`, `stop`, `running`), and its exposed functions
     survive restarts.
   - **Per-compartment host state.** Window proxies, the custom-element
     registries, observer hooks (each compartment's records and frame
     callbacks), the deferred module groups (each realm's `bro.scene` and
     the rest run in that realm), the parsed documents and the `PerCompartment`
     slots (one value per compartment where the host kept one per process)
     are each the compartment's and go with it. A compartment torn down while
     its own code runs is destroyed at the next frame seam.
   - **Promise jobs run in the compartment that subscribed them**
     (`installJobCompartmentHooks`).
   - **Tests:** `tests/appview/test_appview.js` (identity, no
     `contentDocument`, the proxy window, `expose` across the membrane,
     the page's denial held and recorded, echo, stop/start, removal), the
     iframe suites (realm call, callback and isolation, `contentWindow`,
     module scripts, `location.reload`), and bronze's `eval_test` and
     `embed_realm_test` for callee realms and groups.
   - **Still open:** the ledger holds what the gates hold (step 1), but not
     yet every resource a compartment's natives create (step 5).
4. **Fiber scheduling and time slices.** **Built** (2026-10-26): the engine
   half in bronze and brass, and bro's half for `<appview>`. bronze's
   `docs/internals.md` ("Realm ownership", "Compartments") describes the
   mechanism.
   - **bro's half** (`host_compartment.h`, `runEntry`). An appview has a
     fiber of its own: 8 ms slices and an 8 MB stack, because its scripts
     compile on it. Every entry that starts its code goes through
     `runEntry`:
     - Async entries are timers, `requestAnimationFrame`, messages, the
       document's scripts, and the observer tick. One that outlasts its
       slice yields, and the frame seam (`serviceCompartmentFibers`, step
       0b of `hostFrame`) gives it one slice per frame until it finishes.
       Async entries that arrive while a job is suspended queue behind it,
       in order.
     - Sync entries are events dispatched into it, a capability call
       into it, and panel hooks. The host needs these finished, so a job
       suspended before one finishes first. An app that does not return
       within `kSyncEntryBudgetMs` (1 s) is terminated, with nothing it
       can catch. It is logged, its view is stopped, and its page gets a
       `terminated` event at the element, with the reason and where the
       app was. `start()` runs it again.
     - Host code that enters another compartment from an app's fiber (a
       capability call into the page, a clone) holds yields off for its
       `CompartmentScope`, so the host's scope stack never holds a
       suspended entry.
     - A compartment whose entry is on its fiber is never destroyed
       mid-run (`fiberDepth`). A suspended job is unwound before the
       realm goes.
     - A terminated app's document resolves to a dead slot until the
       engine releases it, never to the page's compartment.
     - Frames, windows and panels are the page's own app and run on the
       page's stack.
   - **Tests (bro):** `tests/appview/test_appview_slices.js`. A 2e8-step
     loop on a message leaves the frame that started it with the job
     unfinished, and it finishes over later frames while the app runs on.
     A click listener that never returns stops the app with "did not
     return", the page hears `terminated`, and `start()` runs the app
     again.
   - **Still open in bro:** promise jobs the host queues for an appview
     from the page's stack (a membrane promise settling) run at the page's
     checkpoint, not on the fiber. Input to a suspended app waits for the
     suspended job, within the sync budget, rather than queueing as a task.
   - **The API.** `embed::createCompartment(realm, {sliceMs, stackBytes,
     name})` gives a realm a fiber. `runInCompartment(c, job)` runs one entry
     there, then that fiber's own microtasks, and returns `Idle`, `Yielded` or
     `Terminated`. `resumeCompartment` continues a yielded entry with a fresh
     slice, and `terminateCompartment(c, reason)` ends the compartment.
   - **Where it yields.** Loop backedges in every tier (brass
     `Module::set_interrupt_poll`, on the TLS block's stack-limit word), and
     function entries (the prologue's existing stack-limit check).
   - **Who arms it.** One watchdog thread arms the running compartment's
     thread once its slice deadline passes.
   - **Termination** is brass's `TerminationException`, which no guest
     `catch` or `finally` takes. It unwinds the fiber to its entry, and every
     later job is refused.
   - **Per-stack state.** The runtime state that follows one stack's frames
     is saved and restored per fiber (brass stack locals): the shadow stack,
     the realm stack, the handle scopes, the microtask queue, and native call
     scratch. A suspended compartment's frames stay roots.
   - **Realm ownership** (pulled forward from step 3). A function enters
     its own realm when the host calls it. A native handle unwraps only in
     the realm that made it, so another app's handle reaches a native as a
     TypeError. Each realm has its own instance of a compiled module's
     data.
   - **What is not preempted.** A native runs to its end, and how far it
     overran the slice is recorded, with the stack it was called from.
     Outlined parallel loop kernels are not polled; they are counted ranges
     on worker threads. `embed::NoYieldScope` holds yields off for host code
     that must not be re-entered.
   - **Tests.** bronze `eval_compartment_test`: in each tier, a runaway loop
     yields and is terminated, a long job finishes slice by slice, and
     termination skips `catch` and `finally`. It also checks that each
     compartment drains its own microtasks, that an uncaught error leaves
     the compartment alive, and that sixteen suspended compartments survive
     full collections. bronze `embed_realm_test` covers handle ownership.
     brass `test_interrupt_poll` covers the poll in each tier.
   - **Measured** (Windows, Release; bronze's bench kernels on the JIT path,
     median compute ms of 7 runs, interleaved with a build that has no
     poll):
     - Tier 2: within 1.5% everywhere (mat4_kernel 1438 against 1417,
       numeric_loop 28.5 against 28.4, dynamic_bitwise 2618 against 2632).
     - Auto: within noise (three_math 264.6 against 264.1,
       typed_array_crunch 115.5 against 110.6).
     - Tier 1 and the interpreter: as often faster as slower.
     - Writing the interpreter's check as one load also made it faster than
       before the poll existed (brass's ratchet: interp_fib_iter_40 5.4x
       against a golden of 7.4x).
   - **Still open:** polls inside long builtins (a 100 M-element
     `Array.prototype.fill` runs to its end), and the async split of
     synchronous natives an app can reach (file I/O, model loads), which
     bro does when it adopts compartments.
5. **Disconnects, the error card, and the ledger's per-resource tests.**
   Begun (2026-10-26):
   - **Teardown test** (`tests/appview/test_appview_teardown.js`, fixture
     `tests/test_app/appview_ledger`). An app holding a 2D canvas, a
     sounding oscillator, an interval, an `fs.watch` on its own folder, a
     Worker writing a heartbeat file and a `<terminal>` child is stopped
     three ways (`stop()`, terminated for not returning, removed); each
     time, checked from outside, the child process is gone and the
     heartbeat stops. Run on halo.
   - **Workers are the compartment's.** A sub-document's `new Worker(url)`
     resolves against its own document (it read the page's folder before),
     and its compartment terminates the workers it made at teardown
     (`host_worker.cpp`).
   - **Still open:** WebGL and the 3D scene in a sub-document (both answer
     null, so an inline app has neither); the error card; the exception
     policy (open question 5); recurring overruns marked not responding;
     per-kind tests for model handles and GPU objects once a sub-document
     can hold them; brokit's `fs` / `fetch` relative paths resolve against
     the process's app folder, not an appview's.
6. **Install identity, helm's prompts and the Apps settings page.** Identity
   and the page are **built** (2026-10-26); helm's prompts are not:
   - `bro --install` writes `<root>/<id>.install.json` beside the installed
     folder (`src/engine/install_record.*`): each file's path, size,
     modification time and SHA-256, and the tree's hash (SHA-256 over
     "path\0size\0file hash\n" in path order). The grant store's record for
     the app carries the hash (`installHash`) its decisions were made for.
   - Launch hashes nothing: it compares each recorded file's size and time
     with the folder (0.5 ms for a three-file app on Windows); only a
     difference costs a full hash then. A background thread hashes the
     content after launch and, on a mismatch, takes the grants back while
     the app runs.
   - A changed hash marks the app's decisions `changed` (`bro --permissions`,
     `bro.permissions.list`) and asks again; a reinstall of the same content
     keeps them. A `--link` install records no content (it is a checkout
     being worked on): its hash names the link's target. An installed folder
     with no record is `unrecorded`, and grants made then hold only while it
     stays so.
   - helm's Settings, App Permissions (`helm/ui/js/settings/pages/permissions.js`):
     every installed or known app, each declared capability with its state
     and last use, revoke and reset, the picked paths with removal, and dev
     mode, all through `bro.permissions` (`docs/permissions-api.js`); dev
     mode on shows a pill in helm's bar.
7. **Certificates checked in Release, and native preconditions.**
8. **Retire** realm-scope swapping. **Done with step 3** (2026-10-26):
   `host_realm_scope.*` is deleted, and every sub-document is a realm.
   (`desktop_trust`'s location and environment rules, grants by id and the
   applet certificates went with step 1.)
