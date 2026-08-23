// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * =============================================================================
 * bro.steam — Steamworks Integration & Services
 * =============================================================================
 *
 * Steamworks integration providing user authentication, achievements, stats,
 * overlay activation, friends list, rich presence, matchmaking lobbies, and voice chat.
 * @example
 * if (bro.steam.available) {
 *     console.log('Logged into Steam as:', bro.steam.personaName);
 *     bro.steam.setAchievement('ACH_FIRST_WIN');
 *   }
 */
/**
 * @readonly
 * @type {boolean}
 */
bro.steam.available;

/**
 * @readonly
 * @type {string}
 */
bro.steam.reason;

/**
 * @readonly
 * @type {number}
 */
bro.steam.appId;

/**
 * @readonly
 * @type {string}
 */
bro.steam.steamId;

/**
 * @readonly
 * @type {string}
 */
bro.steam.personaName;

/**
 * @readonly
 * @type {boolean}
 */
bro.steam.isLoggedOn;

/**
 * @readonly
 * @type {boolean}
 */
bro.steam.isVoiceRecording;

/**
 * @readonly
 * @type {number}
 */
bro.steam.voiceSampleRate;

/**
 * @type {EventHandler}
 */
bro.steam.onpulse;

/**
 * @type {EventHandler}
 */
bro.steam.onfriends;

/**
 * @type {EventHandler}
 */
bro.steam.onoverlay;

/**
 * @type {EventHandler}
 */
bro.steam.onjoinrequest;

/**
 * @type {EventHandler}
 */
bro.steam.onlobbyentered;

/**
 * @type {EventHandler}
 */
bro.steam.onlobbyupdated;

/**
 * @type {EventHandler}
 */
bro.steam.onlobbyleft;

/**
 * @type {EventHandler}
 */
bro.steam.onlobbyinvite;

/**
 * @type {EventHandler}
 */
bro.steam.onlobbyjoinrequest;

/**
 * @type {EventHandler}
 */
bro.steam.onvoicecaptured;

/**
 * @param {string} name
 * @returns {boolean}
 */
bro.steam.getAchievement = function(name) {};

/**
 * @param {string} name
 * @returns {boolean}
 */
bro.steam.setAchievement = function(name) {};

/**
 * @param {string} name
 * @returns {boolean}
 */
bro.steam.clearAchievement = function(name) {};

/**
 * @param {string} name
 * @returns {number}
 */
bro.steam.getStat = function(name) {};

/**
 * @param {string} name
 * @param {number} value
 * @returns {boolean}
 */
bro.steam.setStat = function(name, value) {};

/**
 * @returns {boolean}
 */
bro.steam.storeStats = function() {};

/**
 * @param {string} [dialog]
 */
bro.steam.activateOverlay = function(dialog) {};

/**
 * @param {string} url
 */
bro.steam.activateOverlayToWebPage = function(url) {};

/**
 * @returns {Array<Object>}
 */
bro.steam.getFriends = function() {};

/**
 * @param {string} steamId
 * @param {(string|number)} [size]
 * @returns {Promise<Object>}
 */
bro.steam.getAvatar = function(steamId, size) {};

/**
 * @param {string} key
 * @param {string} value
 * @returns {boolean}
 */
bro.steam.setRichPresence = function(key, value) {};

bro.steam.clearRichPresence = function() {};

/**
 * @param {string} type
 * @param {number} maxMembers
 * @returns {Promise<Object>}
 */
bro.steam.createLobby = function(type, maxMembers) {};

/**
 * @param {string} lobbyId
 * @returns {Promise<Object>}
 */
bro.steam.joinLobby = function(lobbyId) {};

/**
 * @param {string} lobbyId
 */
bro.steam.leaveLobby = function(lobbyId) {};

/**
 * @param {string} lobbyId
 * @param {string} key
 * @param {string} value
 * @returns {boolean}
 */
bro.steam.setLobbyData = function(lobbyId, key, value) {};

/**
 * @param {string} lobbyId
 * @returns {Array<Object>}
 */
bro.steam.getLobbyMembers = function(lobbyId) {};

/**
 * @param {string} lobbyId
 * @returns {string}
 */
bro.steam.getLobbyOwner = function(lobbyId) {};

/**
 * @param {string} lobbyId
 * @param {string} key
 * @returns {string}
 */
bro.steam.getLobbyData = function(lobbyId, key) {};

/**
 * @param {Object} [filter]
 */
bro.steam.requestLobbyList = function(filter) {};

/**
 * @param {string} lobbyId
 * @param {string} steamId
 * @returns {boolean}
 */
bro.steam.inviteUserToLobby = function(lobbyId, steamId) {};

bro.steam.startVoiceRecording = function() {};

bro.steam.stopVoiceRecording = function() {};

/**
 * @param {Uint8Array} data
 * @param {number} [sampleRate]
 * @returns {Promise<Float32Array>}
 */
bro.steam.decodeVoice = function(data, sampleRate) {};

