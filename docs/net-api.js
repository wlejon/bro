// =============================================================================
// bro Networking API Reference
// =============================================================================
//
// The networking API is available to all bro apps via the global `bro.net`
// object. It wraps Valve's GameNetworkingSockets (GNS) library through QuickJS
// bindings, providing peer-to-peer and client/server networking with reliable
// and unreliable message delivery.
//
// Two modes of operation:
//   - Host: listen on a port, accept client connections
//   - Client: connect to a remote host by address
//
// For game-object replication (spawn/despawn, authority, delta state sync,
// RPC) see the high-level layer built on these primitives: `bro.net.sync`,
// documented in docs/net-sync-api.js.
//
// Quick start (host):
//   bro.net.init();
//   bro.net.host(27015);
//   bro.net.onconnect = (connId) => console.log("client connected:", connId);
//   bro.net.onmessage = (connId, data, channel) => {
//     const text = new TextDecoder().decode(data);
//     console.log("received:", text, "on channel", channel);
//   };
//
// Quick start (client):
//   bro.net.init();
//   bro.net.connect("127.0.0.1:27015");
//
// Available in every JS context: the main document AND workers. Each context
// gets its own independent subscriber (its own connections, host socket, and
// callbacks); a worker can host, connect, send, sendClone, and receive
// exactly like the main thread. Clone values are always deserialized in the
// receiving context.
//
// -----------------------------------------------------------------------------
// Channels (GNS lanes)
// -----------------------------------------------------------------------------
//
// Every connection has 8 channels (0..7), mapped 1:1 onto GameNetworkingSockets
// lanes with equal priority and equal bandwidth weight. Channels eliminate
// head-of-line blocking between unrelated streams: a large reliable transfer
// on channel 1 does not stall chat messages on channel 2.
//
// Ordering guarantees:
//   - RELIABLE messages on the SAME channel arrive in send order (this is the
//     only strong ordering guarantee).
//   - Messages on DIFFERENT channels may arrive in any relative order.
//   - UNRELIABLE messages may be lost or arrive out of order even within a
//     channel.
//
// Reliability x nodelay matrix ({reliable, nodelay} send options):
//   reliable:true,  nodelay:false  (default), ordered, retransmitted, Nagle
//                                   batching (small sends coalesce ~a few ms).
//   reliable:true,  nodelay:true, ordered + retransmitted, flushed
//                                   immediately (no Nagle batching).
//   reliable:false, nodelay:false, fire-and-forget, Nagle batching.
//   reliable:false, nodelay:true, fire-and-forget, flushed immediately, and
//                                   DROPPED rather than buffered if the link
//                                   can't take it right now (freshest-data-only
//                                   semantics for high-rate state updates).
//
// The lane set is fixed at connection setup; `channel` outside 0..7 is clamped.
//
// -----------------------------------------------------------------------------
// Wire format (bro <-> bro only, pre-1.0)
// -----------------------------------------------------------------------------
//
// Every message carries a 2-byte frame header on the wire:
//   byte 0: 0xB7, magic/format version. Changes if the framing ever changes,
//           so mismatched peers drop messages loudly instead of misparsing.
//   byte 1: frame type, 0x00 raw bytes (send/broadcast), 0x01 structured
//           clone (sendClone/broadcastClone). Room for future frame types.
//
// Consequence: raw payloads are NOT byte-identical on the wire to what send()
// was given, so bro.net peers must all speak this format (any bro build with
// this API does; non-bro peers are not supported). Messages with an
// unrecognized header, unknown frame type, or malformed clone payload are
// dropped with a log diagnostic. They never reach onmessage and never crash
// the receiver, even from a hostile peer.
//
// =============================================================================


// -----------------------------------------------------------------------------
// bro.net, Namespace (not a constructor)
// -----------------------------------------------------------------------------

const net = bro.net;


// --- Lifecycle ---------------------------------------------------------------

/**
 * Initialize the GNS networking library.
 * Must be called before any other bro.net method.
 * @returns {boolean} true if initialization succeeded
 */
net.init();

/**
 * Close the listen socket and disconnect all clients (host mode).
 * Does nothing if not currently hosting.
 */
net.close();


// --- Host Mode ---------------------------------------------------------------

/**
 * Start listening for incoming connections on the given port.
 * Throws if init() has not been called.
 *
 * @param {number} port - Port number (1..65535)
 * @returns {boolean} true if the listen socket was created successfully
 */
net.host(27015);

/**
 * Whether this peer is currently hosting (has an open listen socket).
 * @returns {boolean}
 */
net.isHosting();


// --- Client Mode -------------------------------------------------------------

/**
 * Connect to a remote host.
 * Throws if init() has not been called.
 *
 * @param {string} address - Remote address as "ip:port" (e.g. "192.168.1.5:27015")
 * @returns {boolean} true if the connection attempt was initiated
 */
net.connect("127.0.0.1:27015");


// --- Sending Data ------------------------------------------------------------

/**
 * Send raw bytes to a specific connection. Delivered to the peer's onmessage
 * as an ArrayBuffer.
 *
 * @param {number} connId - Connection handle (received via onconnect callback)
 * @param {string|ArrayBuffer|TypedArray} data - Payload to send
 * @param {boolean|Object} [options=true] - Legacy boolean = reliable flag, or:
 * @param {boolean} [options.reliable=true] - Reliable ordered delivery
 * @param {number}  [options.channel=0]    - Channel 0..7 (clamped); reliable
 *                                           ordering holds per channel
 * @param {boolean} [options.nodelay=false] - Flush immediately (see the
 *                                           reliability x nodelay matrix above)
 * @returns {boolean} true if the message was queued successfully
 */
net.send(connId, "hello world");
net.send(connId, new Uint8Array([1, 2, 3]).buffer, false);            // legacy boolean
net.send(connId, posBuf, { reliable: false, channel: 1, nodelay: true });

/**
 * Broadcast raw bytes to all connected peers (host mode).
 *
 * @param {string|ArrayBuffer|TypedArray} data - Payload to send
 * @param {boolean|Object} [options=true] - Same options as send()
 */
net.broadcast("game state update");
net.broadcast(new Float32Array([x, y, z]).buffer, { reliable: false });

/**
 * Send a structured VALUE to a specific connection (structured clone over the
 * wire). Delivered to the peer's onmessage as the decoded value, not an
 * ArrayBuffer.
 *
 * Survives the trip: plain objects, arrays, strings, numbers, booleans,
 * null/undefined, BigInt, ArrayBuffer, and TypedArrays (nested arbitrarily,
 * up to 64 levels deep).
 *
 * Rejected with a TypeError: functions, symbols, Mesh, ImageBitmap (these
 * transfer by in-process pointer and cannot cross a network, export bytes
 * instead), and transfer lists (pass an options object, not an array).
 *
 * @param {number} connId - Connection handle
 * @param {*} value - Any clonable value
 * @param {Object} [options] - Same {reliable, channel, nodelay} as send()
 * @returns {boolean} true if the message was queued successfully
 */
net.sendClone(connId, { type: "spawn", id: 7, pos: new Float32Array([x, y, z]) },
              { channel: 2 });

/**
 * Broadcast a structured value to all connected peers (host mode).
 *
 * @param {*} value - Any clonable value (see sendClone)
 * @param {Object} [options] - Same {reliable, channel, nodelay} as send()
 */
net.broadcastClone({ tick: 1042, entities: [{ id: 1, hp: 80 }] }, { channel: 3 });


// --- Disconnecting -----------------------------------------------------------

/**
 * Disconnect a specific connection.
 *
 * @param {number} connId - Connection handle
 * @param {number} [reason=0] - Application-defined reason code
 */
net.disconnect(connId);
net.disconnect(connId, 1001);


// --- Querying ----------------------------------------------------------------

/**
 * Get all active connection handles.
 * @returns {number[]} Array of connection IDs
 */
const conns = net.connections();

/**
 * Get real-time statistics for a connection.
 *
 * @param {number} connId - Connection handle
 * @returns {{ ping: number, packetLoss: number, bytesSent: number, bytesRecv: number } | null}
 *   - ping: round-trip time in milliseconds
 *   - packetLoss: packet loss ratio (0.0 .. 1.0)
 *   - bytesSent: bytes per second sent
 *   - bytesRecv: bytes per second received
 *   Returns null if the connection is invalid.
 */
const stats = net.stats(connId);
if (stats) {
  console.log(`Ping: ${stats.ping.toFixed(1)}ms, Loss: ${(stats.packetLoss * 100).toFixed(1)}%`);
}


// --- Callbacks (Event Handlers) ----------------------------------------------

/**
 * Called when a new peer connects (host mode: client joined;
 * client mode: connection to host established).
 *
 * @param {number} connId - The new connection's handle
 */
net.onconnect = function(connId) {
  console.log("Connected:", connId);
};

/**
 * Called when a peer disconnects or the connection is lost.
 *
 * @param {number} connId - The disconnected connection's handle
 * @param {number} reason - Disconnect reason code (0 = normal)
 */
net.ondisconnect = function(connId, reason) {
  console.log("Disconnected:", connId, "reason:", reason);
};

/**
 * Called when a message is received from a peer.
 *
 * @param {number} connId - Connection handle of the sender
 * @param {ArrayBuffer|*} data - ArrayBuffer for send()/broadcast() payloads;
 *   the decoded value for sendClone()/broadcastClone() payloads
 * @param {number} channel - Channel (0..7) the message arrived on
 */
net.onmessage = function(connId, data, channel) {
  if (data instanceof ArrayBuffer) {
    // Raw send: decode text or read binary
    const text = new TextDecoder().decode(data);
    const view = new DataView(data);
  } else {
    // sendClone: `data` is already the structured value
    console.log("clone on channel", channel, data);
  }
};


// =============================================================================
// Complete Example: Simple Chat
// =============================================================================

/*
// --- Host ---
bro.net.init();
bro.net.host(27015);

const clients = new Set();

bro.net.onconnect = (id) => {
  clients.add(id);
  bro.net.broadcast(`Player ${id} joined`);
};

bro.net.ondisconnect = (id) => {
  clients.delete(id);
  bro.net.broadcast(`Player ${id} left`);
};

bro.net.onmessage = (id, data) => {
  const msg = new TextDecoder().decode(data);
  bro.net.broadcast(`[${id}] ${msg}`);
};

// --- Client ---
bro.net.init();
bro.net.connect("127.0.0.1:27015");

bro.net.onmessage = (id, data) => {
  console.log(new TextDecoder().decode(data));
};

// Send a chat message (connId from onconnect)
bro.net.send(connId, "Hello everyone!");
*/
