// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * =============================================================================
 * bro.net & bro.net.sync — Game Networking and Replication System
 * =============================================================================
 *
 * Low-level UDP game networking backed by GameNetworkingSockets, with raw
 * binary packets, structured clone messaging, and multi-channel delivery.
 * Includes bro.net.sync high-level multiplayer state replication and RPCs.
 * @typedef {Object} NetStats
 * @property {number} [ping]
 * @property {number} [packetLoss]
 * @property {number} [bytesSent]
 * @property {number} [bytesRecv]
 */

/**
 * @typedef {Object} NetSendOptions
 * @property {boolean} [reliable=true]
 * @property {number} [channel=0]
 * @property {boolean} [nodelay=false]
 */

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * @type {EventHandler}
 */
bro.net.onconnect;

/**
 * @type {EventHandler}
 */
bro.net.onConnect;

/**
 * @type {EventHandler}
 */
bro.net.ondisconnect;

/**
 * @type {EventHandler}
 */
bro.net.onDisconnect;

/**
 * @type {EventHandler}
 */
bro.net.onmessage;

/**
 * @type {EventHandler}
 */
bro.net.onMessage;

/**
 * Initialize the GameNetworkingSockets subsystem explicitly (also initialized lazily on host/connect).
 * @returns {boolean}
 */
bro.net.init = function() {};

/**
 * Shut down all connections, stop hosting, and close the networking subsystem.
 */
bro.net.close = function() {};

/**
 * @param {'connect'|'disconnect'|'message'} type
 * @param {Function} listener
 */
bro.net.addEventListener = function(type, listener) {};

/**
 * @param {'connect'|'disconnect'|'message'} type
 * @param {Function} listener
 */
bro.net.removeEventListener = function(type, listener) {};

/**
 * Listen on UDP `port`. Needs the `net` capability (docs/apps.md,
 * Permissions), asked as `udp-bind://0.0.0.0:<port>`; a refusal throws an
 * Error with `code: 'EACCES'` before anything is bound.
 * @param {number} port
 * @param {Function} [callback]
 */
bro.net.host = function(port, callback) {};

bro.net.unhost = function() {};

/** Alias of unhost(): stop listening. */
bro.net.closeHost = function() {};

/**
 * Whether host() is listening.
 * @returns {boolean}
 */
bro.net.isHosting = function() {};

/**
 * Connect to a host. Needs the `net` capability, asked as
 * `udp://<address>:<port>`; a refusal throws an Error with `code: 'EACCES'`
 * before anything is sent.
 * @param {string} address
 * @param {number} port
 * @param {Function} [callback]
 * @returns {number}
 */
bro.net.connect = function(address, port, callback) {};

/**
 * Closes the connection to one peer. `reason` is the application-defined
 * disconnect code the peer's ondisconnect receives (0 when omitted), the
 * same number GameNetworkingSockets carries on the wire: a wrapper that
 * dropped it left every kick indistinguishable from a network drop.
 *
 * @param {number} peerId
 * @param {number} [reason=0]
 */
bro.net.disconnect = function(peerId, reason) {};

bro.net.disconnectAll = function() {};

/**
 * @param {number} peerId
 * @param {(string|ArrayBuffer|ArrayBufferView)} data
 * @param {(NetSendOptions|number|boolean)} [options]
 */
bro.net.send = function(peerId, data, options) {};

/**
 * @param {(string|ArrayBuffer|ArrayBufferView)} data
 * @param {(NetSendOptions|number|boolean)} [options]
 */
bro.net.broadcast = function(data, options) {};

/**
 * @param {number} peerId
 * @param {*} value
 * @param {(NetSendOptions|number|boolean)} [options]
 */
bro.net.sendClone = function(peerId, value, options) {};

/**
 * @param {*} value
 * @param {(NetSendOptions|number|boolean)} [options]
 */
bro.net.broadcastClone = function(value, options) {};

/**
 * The live connection ids, the same numbers onconnect / onmessage hand
 * out. A connection id is a full unsigned 32-bit GameNetworkingSockets
 * handle: as `sequence<long>` the top-bit ids came back negative and no
 * longer compared equal to the id an event delivered.
 * @returns {Array<number>}
 */
bro.net.peers = function() {};

/** Alias of `peers()`. @returns {Array<number>} */
bro.net.connections = function() {};

/**
 * @param {number} peerId
 * @returns {string}
 */
bro.net.getPeerAddress = function(peerId) {};

/**
 * @param {number} [peerId]
 * @returns {NetStats}
 */
bro.net.stats = function(peerId) {};

/**
 * @param {number} peerId
 * @returns {NetStats}
 */
bro.net.getPeerStats = function(peerId) {};

/**
 * @param {number} peerId
 * @param {number} chance
 * @param {number} latencyMin
 * @param {number} latencyMax
 */
bro.net.setPeerSimulatedLoss = function(peerId, chance, latencyMin, latencyMax) {};


