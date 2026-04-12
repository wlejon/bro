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
// Quick start (host):
//   bro.net.init();
//   bro.net.host(27015);
//   bro.net.onconnect = (connId) => console.log("client connected:", connId);
//   bro.net.onmessage = (connId, data) => {
//     const text = new TextDecoder().decode(data);
//     console.log("received:", text);
//   };
//
// Quick start (client):
//   bro.net.init();
//   bro.net.connect("127.0.0.1:27015");
//
// =============================================================================


// -----------------------------------------------------------------------------
// bro.net — Namespace (not a constructor)
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
 * Send data to a specific connection.
 *
 * @param {number} connId - Connection handle (received via onconnect callback)
 * @param {string|ArrayBuffer|TypedArray} data - Payload to send
 * @param {boolean} [reliable=true] - Use reliable ordered delivery (true) or
 *                                     unreliable unordered (false)
 * @returns {boolean} true if the message was queued successfully
 */
net.send(connId, "hello world");
net.send(connId, new Uint8Array([1, 2, 3]).buffer, false);  // unreliable

/**
 * Broadcast data to all connected peers (host mode).
 *
 * @param {string|ArrayBuffer|TypedArray} data - Payload to send
 * @param {boolean} [reliable=true] - Use reliable ordered delivery
 */
net.broadcast("game state update");
net.broadcast(new Float32Array([x, y, z]).buffer, false);  // unreliable


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
 * @param {ArrayBuffer} data - The received message payload
 */
net.onmessage = function(connId, data) {
  // Decode text messages:
  const text = new TextDecoder().decode(data);

  // Or read binary data:
  const view = new DataView(data);
  const x = view.getFloat32(0, true);
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
