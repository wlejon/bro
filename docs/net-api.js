// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * =============================================================================
 * bro.net & bro.net.sync — Game Networking and Replication System
 * =============================================================================
 *
 * Low-level UDP game networking backed by GameNetworkingSockets, with raw
 * binary packets, structured clone messaging, and multi-channel delivery.
 * Includes bro.net.sync high-level multiplayer state replication and RPCs.
 * @example
 * bro.net.onconnect = (peerId) => console.log('Peer connected:', peerId);
 *   bro.net.onmessage = (peerId, data) => console.log('Received message:', data);
 *   bro.net.host(7777);
 */
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

/**
 * @param {string} address
 * @param {number} port
 * @param {Function} [callback]
 * @returns {number}
 */
bro.net.connect = function(address, port, callback) {};

/**
 * @param {number} peerId
 */
bro.net.disconnect = function(peerId) {};

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
 * @returns {Array<number>}
 */
bro.net.peers = function() {};

/**
 * @param {number} peerId
 * @returns {string|null}
 */
bro.net.getPeerAddress = function(peerId) {};

/**
 * @returns {Object}
 */
bro.net.stats = function() {};

/**
 * @param {number} peerId
 * @returns {Object|null}
 */
bro.net.getPeerStats = function(peerId) {};

/**
 * @param {number} peerId
 * @param {number} chance
 * @param {number} latencyMin
 * @param {number} latencyMax
 */
bro.net.setPeerSimulatedLoss = function(peerId, chance, latencyMin, latencyMax) {};

