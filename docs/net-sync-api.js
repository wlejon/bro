// =============================================================================
// bro.net.sync API Reference — high-level multiplayer
// =============================================================================
//
// `bro.net.sync` is a Godot-style high-level multiplayer layer (think
// MultiplayerSpawner + MultiplayerSynchronizer + rpc) built in pure JS over
// the bro.net primitives (see docs/net-api.js). It provides:
//
//   - Spawn/despawn replication: the host spawns typed objects; every client
//     runs the matching registered factory and gets a live replica.
//   - Authority: host-authoritative by default; per-object authority can be
//     handed to a client. Only the authority's writes replicate.
//   - State sync: the authority snapshots declared props at a fixed tick and
//     sends deltas (changed props only) unreliable+nodelay, with periodic
//     reliable keyframes repairing packet loss. Optional interpolation
//     smooths numeric props on replicas.
//   - RPC: named handlers invoked across the wire, args structured-cloned.
//
// Objects are id-keyed PLAIN JS OBJECTS produced by your factories — sync is
// deliberately not coupled to the DOM or the 3D scene graph. Your factory is
// the place to create scene nodes/elements; your destroy hook is where they
// are torn down.
//
// Present whenever bro.net is (main document and workers). In builds compiled
// without BRO_WITH_NET, `bro.net.available === false` and `bro.net.sync` does
// not exist.
//
// -----------------------------------------------------------------------------
// Topology and authority model
// -----------------------------------------------------------------------------
//
// Sessions are a star: ONE host, N clients. All traffic flows through the
// host (clients never talk to each other directly):
//
//   - The host owns object identity: only the host may spawn(), despawn(),
//     and setAuthority().
//   - Every object has one authority — the host (default) or one client.
//     The authority's writes to the declared sync props replicate to
//     everyone; the host relays client-authority state to the other clients.
//   - Writes by non-authorities are NOT sent anywhere and are overwritten
//     when the authority next changes the prop or a keyframe arrives.
//   - If a client disconnects, its objects REVERT TO HOST AUTHORITY (they are
//     not despawned). If a client loses the host, its replicas are kept,
//     frozen at their last state — the app decides what that means.
//   - A client that connects late receives the full current world: the host
//     replays every live object as a spawn carrying its current state.
//
// -----------------------------------------------------------------------------
// Wire discipline (what rides where)
// -----------------------------------------------------------------------------
//
//   channel 0, reliable:            spawns, despawns, authority changes,
//                                   full-state keyframes, and RPCs with the
//                                   default mode:'reliable'. Ordered, so
//                                   control operations are seen consistently.
//   channel 1, unreliable+nodelay:  delta state updates (freshest-data-only;
//                                   losing one is fine — the next change or
//                                   keyframe repairs it) and RPCs registered
//                                   mode:'unreliable'.
//
// Every sync message is a structured-clone value carrying a reserved `__sync`
// key. Do NOT use `__sync` as a top-level key in your own sendClone values —
// such messages are treated as sync traffic. Everything else — raw
// send()/broadcast() bytes and untagged clone values — passes through to your
// own bro.net.onmessage untouched.
//
// Deltas carry per-object sequence numbers; stale or reordered deltas are
// dropped. Delta detection compares snapshots with Object.is: assign NEW
// values to replicate. Mutating a nested object in place is invisible to the
// differ (it will still go out with the next keyframe). Keep sync props to
// primitives (numbers, strings, booleans, null) for predictable behavior.
//
// bro.net.onmessage/onconnect/ondisconnect remain single-slot properties, but
// sync chains politely: when host()/join() is called it takes the underlying
// slot and re-exposes the property so `bro.net.onmessage = fn` keeps working,
// set before or after sync starts, and receives every non-sync message.
//
// =============================================================================

const sync = bro.net.sync;


// --- Type registry -------------------------------------------------------------

/**
 * Register a replicated object type. Both ends must register the same type
 * names BEFORE connecting (a spawn of an unregistered type is tracked on a
 * bare object with a console warning).
 *
 * @param {string} type - Type name (e.g. "player", "crate")
 * @param {Object} def
 * @param {function(Object): Object} def.create - Factory. Receives the initial
 *   state and returns the object to replicate onto (a plain object; create
 *   your scene node / DOM element here and return a state holder). After
 *   create(), sync assigns all declared props present in the state onto the
 *   returned object.
 * @param {function(Object)} [def.destroy] - Teardown hook, called with the
 *   object on despawn (both on the host and on every client).
 * @param {string[]|{props: string[], interpolate?: string[]}} def.sync -
 *   Declared sync props. The authority snapshots exactly these each tick.
 *   `interpolate` lists numeric props that replicas smooth: instead of
 *   snapping, the value lerps to each incoming target over one tick interval
 *   (adding ~1/tickHz of display latency). Interpolation is a REPLICA-side
 *   registration choice — each context smooths according to its own def.
 */
sync.register('player', {
  create(state) {
    const node = scene.createBox({ size: [1, 1, 1] });   // your visuals
    return { node, x: 0, y: 0, hp: 100, name: '' };      // the synced object
  },
  destroy(obj) { obj.node.remove(); },
  sync: { props: ['x', 'y', 'hp', 'name'], interpolate: ['x', 'y'] },
});


// --- Session lifecycle ---------------------------------------------------------

/**
 * Become the session host.
 *
 * With `port`, calls bro.net.init() + bro.net.host(port) for you. Without it,
 * layers over a listen socket the app already opened (call sync.host() right
 * after bro.net.host()). Either way, call BEFORE clients connect: connections
 * that already exist are greeted with the current world immediately, but
 * anything a client was sent earlier predates sync.
 *
 * @param {Object} [opts]
 * @param {number} [opts.port]             - Listen port (omit to layer over an
 *                                           app-managed socket)
 * @param {number} [opts.tickHz=20]        - State sync rate (authority side)
 * @param {number} [opts.keyframeEvery=30] - Full reliable keyframe every N
 *                                           ticks (default: every ~1.5 s at
 *                                           20 Hz). Bounds staleness after
 *                                           packet loss.
 */
sync.host({ port: 27015 });

/**
 * Join a session as a client.
 *
 * With `address`, calls bro.net.init() + bro.net.connect(address) for you.
 * Without it, layers over a connection the app is establishing itself — call
 * sync.join() BEFORE the connection lands so the host's spawn replay is
 * received. One host connection per context.
 *
 * @param {Object} [opts]
 * @param {string} [opts.address]          - "ip:port" of the host
 * @param {number} [opts.tickHz=20]        - Tick rate for objects this client
 *                                           has authority over
 * @param {number} [opts.keyframeEvery=30]
 */
sync.join({ address: '127.0.0.1:27015' });

/** @type {boolean} True once host() or join() has been called. */
sync.active;

/** @type {boolean} True when this context is the session host. */
sync.isHost;

/** @type {?number} Client side: the connection id of the host (null before
 *  the connection lands or after losing it). */
sync.hostConn;


// --- Spawning (host-only) --------------------------------------------------------

/**
 * Create a replicated object. Host-only. Runs the local factory, assigns the
 * initial state, and replicates the spawn (with full state, reliable) to all
 * clients — including clients that connect later (late-join replay).
 *
 * @param {string} type - A registered type name
 * @param {Object} [state] - Initial values for declared sync props
 * @returns {Object} The object returned by the factory
 */
const p1 = sync.spawn('player', { x: 10, y: 0, hp: 100, name: 'alice' });

/**
 * Destroy a replicated object everywhere. Host-only. Runs the destroy hook
 * locally and on every client.
 *
 * @param {Object} obj - An object returned by spawn() (or a client factory)
 */
sync.despawn(p1);


// --- Authority (host-only) -------------------------------------------------------

/**
 * Transfer authority over an object. Host-only.
 *
 * @param {Object} obj
 * @param {?number} conn - A client connection id (from the host's onconnect),
 *   or 0/null to take authority back to the host.
 *
 * The new authority starts replicating on its next tick (its first send is a
 * full reliable keyframe, giving replicas a clean baseline). In-flight deltas
 * from the previous authority may straggle in for up to one tick; they are
 * rejected by the host's per-object authority check.
 */
sync.setAuthority(p1, clientConn);   // client now drives p1
sync.setAuthority(p1, null);         // host takes it back

/**
 * Whether THIS context is the authority for obj (i.e. local writes to its
 * declared props replicate).
 * @returns {boolean}
 */
sync.isAuthority(p1);


// --- State sync ------------------------------------------------------------------
//
// There is no "send state" call: the authority just writes to the object.
//
//   p1.x = 42;      // replicated (declared prop, we are the authority)
//   p1.vx = 3;      // NOT replicated (undeclared) — purely local
//
// Each tick, changed declared props go out as a delta; every keyframeEvery
// ticks the full declared state goes out reliably. Replicas apply state onto
// the same objects, so game code reads replicated objects exactly like local
// ones.


// --- RPC -------------------------------------------------------------------------

/**
 * Register a named RPC handler and/or its per-RPC configuration (Godot's
 * rpc_config analog). Handlers receive the sender's connection id followed by
 * the call args (structured-clone round-tripped). fromConn is 0 when the
 * handler runs via callLocal (a local self-invocation).
 *
 * Where each option is read from (register the SAME config on every peer,
 * exactly like Godot's rpc_config):
 *   - mode, callLocal: the CALLER's registration — they shape the send.
 *   - authority:       the HOST's registration — enforced when a client's
 *     call arrives at the host. A context that only sends an RPC may declare
 *     config with fn = null.
 *
 * @param {string} name
 * @param {?function(number, ...*)} fn - (fromConn, ...args). Pass null to
 *   declare configuration for an RPC this context only sends.
 * @param {Object} [opts]
 * @param {string} [opts.mode='reliable'] - 'reliable' rides the ordered
 *   control channel (0). 'unreliable' rides the state channel (1,
 *   unreliable+nodelay): lost frames are simply gone (no keyframe repairs an
 *   RPC), delivery order is not guaranteed — relative to other unreliable
 *   RPCs OR to reliable traffic sent around the same time. Use it for
 *   high-rate fire-and-forget calls (muzzle flashes, pings) where the next
 *   call supersedes a lost one.
 * @param {boolean} [opts.callLocal=false] - call() also invokes the caller's
 *   own handler, synchronously after the send, with fromConn 0 (Godot's
 *   call_local). Applies to call() only, on host and clients alike; callTo()
 *   never invokes locally.
 * @param {string} [opts.authority='any'] - 'host': only the host may invoke
 *   this RPC. A client call is rejected host-side with a console warning and
 *   counted in _stats().rpcsRejected — never an exception across the wire.
 * @param {boolean} [opts.relay=true] - false pins the RPC host-only for
 *   client->client callTo(): the host refuses to relay it (see callTo).
 */
sync.rpc('chat', (from, text) => {
  console.log('chat from', from, ':', text);
});
sync.rpc('flash', (from, x, y) => { /* ... */ }, { mode: 'unreliable' });
sync.rpc('kick', (from, who) => { /* ... */ }, { authority: 'host' });
sync.rpc('emote', null, { mode: 'unreliable' });   // config-only: send-side

/**
 * Invoke a named RPC remotely:
 *   - called on a CLIENT: runs on the host.
 *   - called on the HOST:  runs on every client.
 * With callLocal registered, the caller's own handler ALSO runs, synchronously
 * after the send, with fromConn 0 — exactly once (there is no wire echo).
 * Without it, call() never runs locally (Godot's rpc() without call_local).
 *
 * Args may be anything structured-clonable (nested objects, typed arrays,
 * BigInt, ...); functions/Mesh/ImageBitmap reject with a TypeError.
 *
 * Unknown RPC names (nothing registered on the receiving side) log a console
 * warning and drop — a bad name never throws across the wire.
 *
 * @param {string} name
 * @param {...*} args
 */
sync.call('chat', 'hello everyone');

/**
 * Invoke a named RPC on one specific client. Host-only (Godot's rpc_id()).
 *
 * @param {number} conn - Target client connection id
 * @param {string} name
 * @param {...*} args
 */
sync.callTo(clientConn, 'chat', 'psst — just you');


// --- Introspection ---------------------------------------------------------------

sync.get(id);          // -> object for a replicated id, or null
sync.idOf(obj);        // -> stable numeric id, or null
sync.typeOf(obj);      // -> registered type name, or null
sync.objects();        // -> array of all live replicated objects
sync._stats();         // -> debug counters: {ticks, deltaMsgs, deltaEntities,
                       //    deltaProps, keyframes, rpcsSent, rpcsRecv,
                       //    rpcsRejected, applied, stale, unknown, objects}


// =============================================================================
// Complete Example: host + client
// =============================================================================

/*
// --- Shared (both sides) ---
function registerTypes() {
  bro.net.sync.register('ball', {
    create(state) { return { x: 0, y: 0, hp: 100 }; },
    destroy(obj)  { console.log('ball gone'); },
    sync: { props: ['x', 'y', 'hp'], interpolate: ['x', 'y'] },
  });
}

// --- Host ---
registerTypes();
bro.net.sync.host({ port: 27015 });
const ball = bro.net.sync.spawn('ball', { x: 5, y: 5, hp: 100 });
setInterval(() => { ball.x += 1; }, 100);            // replicates automatically

bro.net.sync.rpc('hit', (from, amount) => {          // client -> host
  ball.hp -= amount;                                 // host is authority: replicates
  if (ball.hp <= 0) bro.net.sync.despawn(ball);
});

// --- Client ---
registerTypes();
bro.net.sync.join({ address: '127.0.0.1:27015' });
// factories run automatically as spawns (and the late-join replay) arrive
bro.net.sync.call('hit', 25);                        // runs on the host

// App messaging still works alongside sync:
bro.net.onmessage = (conn, data, channel) => {       // non-sync traffic only
  console.log('app message', data);
};
*/
