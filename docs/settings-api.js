// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * =============================================================================
 * bro.settings — Engine & Display Settings Management
 * =============================================================================
 */
/**
 * @param {string} key
 * @returns {string}
 */
bro.settings.get = function(key) {};

/**
 * @param {string} category
 * @returns {string}
 */
bro.settings.getAllJson = function(category) {};

/**
 * @param {string} category
 * @returns {string}
 */
bro.settings.getDefaultsJson = function(category) {};

/**
 * @param {string} key
 * @param {string} value
 */
bro.settings.setString = function(key, value) {};

/**
 * @param {string} key
 * @param {number} value
 */
bro.settings.setNumber = function(key, value) {};

/**
 * @param {string} key
 * @param {boolean} value
 */
bro.settings.setBool = function(key, value) {};

/**
 * @param {string} key
 * @param {string} value
 */
bro.settings.setDefaultString = function(key, value) {};

/**
 * @param {string} key
 * @param {number} value
 */
bro.settings.setDefaultNumber = function(key, value) {};

/**
 * @param {string} key
 * @param {boolean} value
 */
bro.settings.setDefaultBool = function(key, value) {};

/**
 * @param {string} category
 */
bro.settings.reset = function(category) {};

/**
 * @param {string} action
 * @param {string} keysJoined
 * @param {number} deadzone
 */
bro.settings.defineAction = function(action, keysJoined, deadzone) {};

/**
 * @param {string} action
 * @param {string} keysJoined
 */
bro.settings.rebindAction = function(action, keysJoined) {};

/**
 * @param {string} action
 */
bro.settings.resetAction = function(action) {};

bro.settings.resetAllActions = function() {};

/**
 * @param {string} action
 * @returns {string}
 */
bro.settings.actionKeysJson = function(action) {};

/**
 * @param {string} key
 * @returns {string}
 */
bro.settings.keyAction = function(key) {};

/**
 * @param {string} action
 * @returns {number}
 */
bro.settings.actionStrength = function(action) {};

/**
 * @param {string} action
 * @returns {boolean}
 */
bro.settings.isActionPressed = function(action) {};

/**
 * @returns {string}
 */
bro.settings.actionsJson = function() {};

/**
 * @returns {string}
 */
bro.settings.appActionsJson = function() {};

/**
 * @returns {string}
 */
bro.settings.displayModesJson = function() {};

/**
 * @param {Function} listener
 */
bro.settings.onChange = function(listener) {};

