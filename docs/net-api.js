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

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * @type {EventHandler}
 */
bro.net.onconnect;

/**
 * @type {EventHandler}
 */
bro.net.ondisconnect;

/**
 * @type {EventHandler}
 */
bro.net.onmessage;

/**
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
 * @param {(ArrayBuffer|ArrayBufferView)} data
 * @param {number} [channel=0]
 */
bro.net.send = function(peerId, data, channel) {};

/**
 * @param {(ArrayBuffer|ArrayBufferView)} data
 * @param {number} [channel=0]
 */
bro.net.broadcast = function(data, channel) {};

/**
 * @param {number} peerId
 * @param {*} value
 * @param {number} [channel=0]
 */
bro.net.sendClone = function(peerId, value, channel) {};

/**
 * @param {*} value
 * @param {number} [channel=0]
 */
bro.net.broadcastClone = function(value, channel) {};

/**
 * The live connection ids, the same numbers onconnect / onmessage hand
 * out. A connection id is a full unsigned 32-bit GameNetworkingSockets
 * handle: as `sequence<long>` the top-bit ids came back negative and no
 * longer compared equal to the id an event delivered.
 * @returns {Array<number>}
 */
bro.net.peers = function() {};

/**
 * @param {number} peerId
 * @returns {string}
 */
bro.net.getPeerAddress = function(peerId) {};

/**
 * @returns {NetStats}
 */
bro.net.stats = function() {};

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

