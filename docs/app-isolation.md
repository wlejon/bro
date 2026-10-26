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

`<iframe>` should become compartments as well, so there is one mechanism
(open question 1).

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
   (one mechanism)? Recommended: yes.
2. Per-compartment memory accounting in one shared bronze heap: allocation
   attribution, and what a budget means at collection.
3. Can the interpreter tier poll for time slices cheaply, and do brass's
   tier-1 and tier-2 code poll at every loop backedge today or only where
   OSR and GC need it?
4. Network granularity: on or off, or per host?
5. The exception policy: continue or disconnect.
6. The cost of freezing intrinsics at boot, and checking certificates in
   Release.

## Order of work

1. **The capability catalogue, and gating `fs`, `child_process`,
   `process.env`, the network and `<terminal>`**, in today's one-app-per-process
   bro. This contains mistakes there too. **Built** (2026-10-26):
   - The catalogue is one table, `src/engine/capabilities.cpp`: ask
     capabilities `files`, `net`, `spawn`, `mic`, `wl`, `displays`, `clip`,
     `pulse`, `remote` and `sys.<member>` (power, audio, network, bluetooth,
     notifications, tray, processes); shell capabilities `shell`,
     `compositor`, `seat`, `cred`, `portal`, `permissions`, granted only by
     the host executable (helm's main, `--grant`). The grants live in
     `AppPermissions` (`src/engine/app_permissions.cpp`), the store in
     `grant_store.cpp`.
   - Enforcement is at the natives, not in `require`: brokit's access policy
     (`brokit::api::setAccessPolicy`) asks at every fs entry point, sqlite
     open, process start and network connect or listen; bro installs it
     (`src/bronze_host/host_permissions.cpp`), with the terminal's spawn
     gate, broaudio's capture gate, and the path and spawn gates of brovfs,
     brosearch, brothumb and broapps. The gated `bro.<ns>` objects are
     still the stub or the live namespace, put in place by the catalogue; a
     declared, undecided one is a stub that asks on first touch.
   - Decided differently from the proposal: a grant is *allow* or *deny* (an
     undecided one asks), not allow/ask/deny; revocation takes effect at the
     next launch, not while the app runs (no `PermissionRevoked` yet);
     `sys.processes` is one capability, not `sys.processes.read`; `files`
     is one capability for everything beyond the app's own directories and
     what the user picked, and with it an app may read and write anywhere
     except bro's own state; identity is the install record's folder
     (`install:<id>`) with no hash yet (step 6), else the canonical folder;
     dev mode is the store's `devMode` flag (`bro --dev-mode on`), not a helm
     setting yet, and it also overrides the user's denials; the store is
     bro's own (`grants.json` in bro's config directory), which no app can
     write, rather than helm's.
   - Not gated yet: `process.env`, and the natives of other siblings that
     take a file path of their own (mesh, image, model and media loaders and
     encoders, screenshots).
2. **Frozen intrinsics;** per-realm host globals and HostClass prototypes.
3. **Compartment = Realm + document + ledger;** host objects owned by their
   compartment; `<appview>`.
4. **Fiber scheduling and time slices.**
5. **Disconnects, the error card, and the ledger's per-resource tests.**
6. **Install identity, helm's prompts and the Apps settings page.**
7. **Certificates checked in Release, and native preconditions.**
8. **Retire** realm-scope swapping. (`desktop_trust`'s location and
   environment rules, grants by id and the applet certificates went with
   step 1.)
