// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * =============================================================================
 * __bro — Internal System Panels & Runtime Telemetry Interface
 * =============================================================================
 */
__bro.splash.dunder_splash.dismiss = function() {};

/**
 * @readonly
 * @type {number}
 */
__bro.viewport.dunder_viewport.width;

/**
 * @readonly
 * @type {number}
 */
__bro.viewport.dunder_viewport.height;

/**
 * @readonly
 * @type {number}
 */
__bro.perf.dunder_perf.fps;

/**
 * @readonly
 * @type {number}
 */
__bro.perf.dunder_perf.frameTime;

/**
 * @readonly
 * @type {number}
 */
__bro.perf.dunder_perf.js;

/**
 * @readonly
 * @type {number}
 */
__bro.perf.dunder_perf.layout;

/**
 * @readonly
 * @type {number}
 */
__bro.perf.dunder_perf.raster;

/**
 * @readonly
 * @type {number}
 */
__bro.perf.dunder_perf.gpu;

/**
 * @readonly
 * @type {number}
 */
__bro.perf.dunder_perf.draw;

/**
 * @returns {number}
 */
__bro.perf.dunder_perf.windowCount = function() {};

/**
 * @param {number} index
 * @returns {number}
 */
__bro.perf.dunder_perf.windowId = function(index) {};

/**
 * @param {number} index
 * @returns {string}
 */
__bro.perf.dunder_perf.windowTitle = function(index) {};

/**
 * @param {number} index
 * @returns {number}
 */
__bro.perf.dunder_perf.windowWidth = function(index) {};

/**
 * @param {number} index
 * @returns {number}
 */
__bro.perf.dunder_perf.windowHeight = function(index) {};

/**
 * @param {number} index
 * @returns {boolean}
 */
__bro.perf.dunder_perf.windowFocused = function(index) {};

/**
 * @param {number} index
 * @returns {boolean}
 */
__bro.perf.dunder_perf.windowMinimized = function(index) {};

/**
 * @readonly
 * @type {number}
 */
__bro.perf.scene.dunder_scene.meshDrawn;

/**
 * @readonly
 * @type {number}
 */
__bro.perf.scene.dunder_scene.meshCulled;

/**
 * @readonly
 * @type {number}
 */
__bro.perf.scene.dunder_scene.instancedDrawn;

/**
 * @readonly
 * @type {number}
 */
__bro.perf.scene.dunder_scene.instancedCulled;

/**
 * @readonly
 * @type {number}
 */
__bro.perf.scene.dunder_scene.splatDrawn;

/**
 * @readonly
 * @type {number}
 */
__bro.perf.scene.dunder_scene.splatCulled;

/**
 * @readonly
 * @type {number}
 */
__bro.perf.scene.dunder_scene.particlesDrawn;

/**
 * @readonly
 * @type {number}
 */
__bro.perf.scene.dunder_scene.particlesCulled;

/**
 * @readonly
 * @type {number}
 */
__bro.perf.scene.dunder_scene.billboardsDrawn;

/**
 * @readonly
 * @type {number}
 */
__bro.perf.scene.dunder_scene.billboardsCulled;

/**
 * @readonly
 * @type {number}
 */
__bro.perf.scene.dunder_scene.decalsDrawn;

/**
 * @readonly
 * @type {number}
 */
__bro.perf.scene.dunder_scene.decalsCulled;

/**
 * @readonly
 * @type {number}
 */
__bro.perf.scene.dunder_scene.shadowDrawn;

/**
 * @readonly
 * @type {number}
 */
__bro.perf.scene.dunder_scene.shadowCulled;

/**
 * @readonly
 * @type {number}
 */
__bro.perf.scene.dunder_scene.shadowTilesTotal;

/**
 * @readonly
 * @type {number}
 */
__bro.perf.scene.dunder_scene.shadowTilesRendered;

/**
 * @readonly
 * @type {number}
 */
__bro.perf.scene.dunder_scene.shadowTilesCached;

/**
 * @readonly
 * @type {number}
 */
__bro.bronze.dunder_bronze.heapUsedBytes;

/**
 * @readonly
 * @type {number}
 */
__bro.bronze.dunder_bronze.heapCommittedBytes;

/**
 * @readonly
 * @type {number}
 */
__bro.bronze.dunder_bronze.heapReservedBytes;

/**
 * @readonly
 * @type {number}
 */
__bro.bronze.dunder_bronze.gcCollections;

/**
 * @readonly
 * @type {number}
 */
__bro.bronze.dunder_bronze.gcPauseNs;

/**
 * @readonly
 * @type {number}
 */
__bro.bronze.dunder_bronze.shapeTransitions;

/**
 * @returns {number}
 */
__bro.menu.dunder_menu.height = function() {};

/**
 * @returns {string}
 */
__bro.menu.dunder_menu.treeJson = function() {};

/**
 * @param {string} id
 */
__bro.menu.dunder_menu.click = function(id) {};

/**
 * @param {string} name
 */
__bro.settingsUI.dunder_settingsUI.show = function(name) {};

/**
 * @returns {string}
 */
__bro.settingsUI.dunder_settingsUI.panelsJson = function() {};

/**
 * @returns {string}
 */
__bro.settingsUI.dunder_settingsUI.activePanel = function() {};

__bro.settingsUI.dunder_settingsUI.toggle = function() {};

/**
 * @returns {boolean}
 */
__bro.settingsUI.dunder_settingsUI.isVisible = function() {};

/**
 * @returns {number}
 */
__bro.settingsUI.dunder_settingsUI.contentTop = function() {};

/**
 * @readonly
 * @type {boolean}
 */
__bro.inspector.dunder_inspector.visible;

/**
 * @readonly
 * @type {string}
 */
__bro.inspector.dunder_inspector.dock;

/**
 * @readonly
 * @type {number}
 */
__bro.inspector.dunder_inspector.width;

/**
 * @readonly
 * @type {number}
 */
__bro.inspector.dunder_inspector.height;

/**
 * @readonly
 * @type {boolean}
 */
__bro.inspector.dunder_inspector.pickerMode;

/**
 * @param {number} maxDepth
 * @returns {string}
 */
__bro.inspector.dunder_inspector.appTreeJson = function(maxDepth) {};

/**
 * @param {number} parentId
 * @returns {string}
 */
__bro.inspector.dunder_inspector.childrenJson = function(parentId) {};

/**
 * @returns {string}
 */
__bro.inspector.dunder_inspector.selectedJson = function() {};

/**
 * @param {number} id
 */
__bro.inspector.dunder_inspector.select = function(id) {};

/**
 * @param {string} dock
 */
__bro.inspector.dunder_inspector.setDock = function(dock) {};

/**
 * @param {number} px
 */
__bro.inspector.dunder_inspector.setSize = function(px) {};

/**
 * @param {boolean} on
 */
__bro.inspector.dunder_inspector.setPickerMode = function(on) {};

__bro.inspector.dunder_inspector.toggle = function() {};

