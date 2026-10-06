// bro_core.js — the public shapes of `bro.time`, `bro.window`,
// `bro.settings`, `bro.appDir` / `bro.userDataDir` / `bro.resolvePath` and
// the system panels' `__bro.*`, assembled over the natives under
// `__bro_native` (src/bronze_host/host_natives.h states the convention;
// native_*.cpp register the pieces).
//
// Every native is spelled by its FULL dotted path, `__bro_native.x.y`, at
// the point of use: that is the spelling the compiler lowers to a direct
// call. An alias (`const N = __bro_native`) would compile to an ordinary
// property read that finds nothing, because the natives are not properties
// of the object.
//
// `bro`, `__bro` and `__bro_native` are host globals registered by
// host_bro_root.cpp before this module's entry runs; js/module.globals names
// them. The three roots and every namespace on them are plain objects, so
// what this file defines is ordinary JavaScript: accessors and functions a
// program can enumerate, alias, and pass around.
(function () {
    'use strict';

    const accessor = (obj, name, get, set) =>
        Object.defineProperty(obj, name, { get, set, enumerable: true, configurable: true });
    const fn = (obj, name, value) =>
        Object.defineProperty(obj, name, { value, writable: true, enumerable: true, configurable: true });

    // ---- bro.time ----------------------------------------------------------
    // Engine::setTimeScale clamps to [0, 100] and ignores a non-finite value;
    // the setter passes what it was given so that one rule lives in one place.
    accessor(bro.time, 'scale', () => __bro_native.time.scale, (v) => { __bro_native.time.scale = v; });
    accessor(bro.time, 'paused', () => __bro_native.time.paused, (v) => { __bro_native.time.paused = !!v; });
    accessor(bro.time, 'now', () => __bro_native.time.now, undefined);

    // ---- bro.appDir / bro.userDataDir / bro.resolvePath --------------------
    accessor(bro, 'appDir', () => __bro_native.paths.appDir, undefined);
    accessor(bro, 'userDataDir', () => __bro_native.paths.userDataDir, undefined);
    fn(bro, 'resolvePath', function resolvePath(path) {
        if (path === undefined) throw new TypeError('bro.resolvePath: path required');
        return __bro_native.paths.resolvePath(String(path));
    });
    fn(bro, 'resolveWritePath', function resolveWritePath(path) {
        if (path === undefined) throw new TypeError('bro.resolveWritePath: path required');
        return __bro_native.paths.resolveWritePath(String(path));
    });

    // ---- bro.window --------------------------------------------------------
    accessor(bro.window, 'state', () => __bro_native.window.state, undefined);
    accessor(bro.window, 'borderless', () => __bro_native.window.borderless,
             (v) => { __bro_native.window.borderless = !!v; });
    accessor(bro.window, 'alwaysOnTop', () => __bro_native.window.alwaysOnTop,
             (v) => { __bro_native.window.alwaysOnTop = !!v; });
    accessor(bro.window, 'title', () => __bro_native.window.title,
             (v) => { __bro_native.window.title = String(v ?? ''); });
    fn(bro.window, 'getTitle', function getTitle() { return __bro_native.window.title; });
    fn(bro.window, 'setTitle', function setTitle(v) { __bro_native.window.title = String(v ?? ''); });

    accessor(bro.window, 'opacity', () => __bro_native.window.opacity,
             (v) => {
                 const n = Number(v);
                 __bro_native.window.opacity = Number.isFinite(n) ? Math.max(0, Math.min(1, n)) : 1.0;
             });
    fn(bro.window, 'getOpacity', function getOpacity() { return __bro_native.window.opacity; });
    fn(bro.window, 'setOpacity', function setOpacity(v) {
        const n = Number(v);
        __bro_native.window.opacity = Number.isFinite(n) ? Math.max(0, Math.min(1, n)) : 1.0;
    });

    accessor(bro.window, 'fullscreen', () => __bro_native.window.fullscreen,
             (v) => { __bro_native.window.fullscreen = !!v; });
    fn(bro.window, 'getFullscreen', function getFullscreen() { return __bro_native.window.fullscreen; });
    fn(bro.window, 'setFullscreen', function setFullscreen(v) { __bro_native.window.fullscreen = !!v; });
    fn(bro.window, 'toggleFullscreen', function toggleFullscreen() {
        __bro_native.window.fullscreen = !__bro_native.window.fullscreen;
        return __bro_native.window.fullscreen;
    });

    accessor(bro.window, 'focused', () => __bro_native.window.focused, undefined);

    // Focus & blur events on bro.window
    const focusListeners = { focus: new Set(), blur: new Set() };
    let onFocusHandler = null;
    let onBlurHandler = null;

    accessor(bro.window, 'onfocus', () => onFocusHandler, (cb) => { onFocusHandler = typeof cb === 'function' ? cb : null; });
    accessor(bro.window, 'onblur', () => onBlurHandler, (cb) => { onBlurHandler = typeof cb === 'function' ? cb : null; });

    fn(bro.window, 'addEventListener', function addEventListener(type, listener) {
        if (typeof listener !== 'function') return;
        if (focusListeners[type]) focusListeners[type].add(listener);
    });
    fn(bro.window, 'removeEventListener', function removeEventListener(type, listener) {
        if (focusListeners[type]) focusListeners[type].delete(listener);
    });

    __bro_native.window._setFocusDispatcher(function (gained) {
        const type = gained ? 'focus' : 'blur';
        const evObj = { type, target: bro.window };
        if (gained && onFocusHandler) {
            try { onFocusHandler(evObj); } catch (e) { console.error(e); }
        } else if (!gained && onBlurHandler) {
            try { onBlurHandler(evObj); } catch (e) { console.error(e); }
        }
        for (const l of focusListeners[type]) {
            try { l(evObj); } catch (e) { console.error(e); }
        }
    });

    fn(bro.window, 'flash', function flash(on) {
        return __bro_native.window.flash(on === undefined ? true : !!on);
    });
    fn(bro.window, 'requestAttention', function requestAttention(on) {
        return __bro_native.window.flash(on === undefined ? true : !!on);
    });

    fn(bro.window, 'beep', function beep() {
        return __bro_native.window.beep();
    });

    const PROGRESS_STATES = { none: 0, normal: 1, error: 2, indeterminate: 3, paused: 4 };
    fn(bro.window, 'setProgress', function setProgress(state, value) {
        let s = 0;
        if (typeof state === 'string') s = PROGRESS_STATES[state.toLowerCase()] ?? 0;
        else if (typeof state === 'number') s = Math.max(0, Math.min(4, Math.floor(state)));
        const v = Math.max(0, Math.min(100, Math.floor(Number(value) || 0)));
        return __bro_native.window.setProgress(s, v);
    });

    fn(bro.window, 'notify', function notify(title, body, options) {
        if (title === undefined) throw new TypeError('bro.window.notify: title is required');
        const t = String(title);
        const b = body !== undefined ? String(body) : '';
        const opts = (options && typeof options === 'object') ? options : {};
        const icon = opts.icon ? String(opts.icon) : '';
        const timeout = typeof opts.timeout === 'number' ? Math.floor(opts.timeout) : -1;
        const silent = !!opts.silent;
        const replacesId = typeof opts.replacesId === 'number' ? (opts.replacesId >>> 0) : 0;
        return __bro_native.window.notify(t, b, icon, timeout, silent, replacesId);
    });

    const trayItemCallbacks = new Map();
    fn(bro.window, 'setTray', function setTray(options) {
        if (!options || typeof options !== 'object') throw new TypeError('bro.window.setTray: options object required');
        const icon = options.icon ? String(options.icon) : '';
        const tooltip = options.tooltip ? String(options.tooltip) : '';
        trayItemCallbacks.clear();
        const menuItems = [];
        if (Array.isArray(options.menu)) {
            for (let i = 0; i < options.menu.length; i++) {
                const item = options.menu[i];
                if (!item || typeof item !== 'object') continue;
                const id = item.id ? String(item.id) : ('item_' + i);
                if (typeof item.click === 'function') {
                    trayItemCallbacks.set(id, item.click);
                }
                menuItems.push({
                    id,
                    label: item.label ? String(item.label) : '',
                    type: item.type ? String(item.type) : 'normal',
                    checked: !!item.checked,
                    enabled: item.enabled !== false,
                });
            }
        }
        return __bro_native.window.setTray(icon, tooltip, JSON.stringify(menuItems));
    });

    fn(bro.window, 'removeTray', function removeTray() {
        trayItemCallbacks.clear();
        return __bro_native.window.removeTray();
    });

    fn(bro.window, 'hasTray', function hasTray() {
        return __bro_native.window.hasTray();
    });

    fn(bro.window, 'isTrayAvailable', function isTrayAvailable() {
        return __bro_native.window.isTrayAvailable();
    });

    __bro_native.window._setTrayDispatcher(function (id) {
        const cb = trayItemCallbacks.get(id);
        if (typeof cb === 'function') {
            try { cb({ id }); } catch (e) { console.error(e); }
        }
    });

    const hotkeyCallbacks = new Map();
    fn(bro.window, 'registerGlobalHotkey', function registerGlobalHotkey(accelerator, callback) {
        if (typeof accelerator !== 'string' || !accelerator) {
            throw new TypeError('bro.window.registerGlobalHotkey: accelerator string required');
        }
        if (typeof callback !== 'function') {
            throw new TypeError('bro.window.registerGlobalHotkey: callback function required');
        }
        const id = __bro_native.window.registerGlobalHotkey(accelerator);
        if (id > 0) {
            hotkeyCallbacks.set(id, callback);
        }
        return id;
    });

    fn(bro.window, 'unregisterGlobalHotkey', function unregisterGlobalHotkey(id) {
        const n = Number(id) >>> 0;
        hotkeyCallbacks.delete(n);
        return __bro_native.window.unregisterGlobalHotkey(n);
    });

    fn(bro.window, 'unregisterAllGlobalHotkeys', function unregisterAllGlobalHotkeys() {
        hotkeyCallbacks.clear();
        __bro_native.window.unregisterAllGlobalHotkeys();
    });

    __bro_native.window._setHotkeyDispatcher(function (accelOrId) {
        for (const [id, cb] of hotkeyCallbacks) {
            try { cb({ id, accelerator: accelOrId }); } catch (e) { console.error(e); }
        }
    });

    let singleInstanceCallback = null;
    fn(bro.window, 'requestSingleInstance', function requestSingleInstance(options) {
        if (!options || typeof options !== 'object') {
            throw new TypeError('bro.window.requestSingleInstance: options object required');
        }
        const name = options.name ? String(options.name) : 'default';
        if (typeof options.onInstance === 'function') {
            singleInstanceCallback = options.onInstance;
        } else {
            singleInstanceCallback = null;
        }
        const args = Array.isArray(options.args) ? options.args.map(String) : [];
        return __bro_native.window.requestSingleInstance(name, JSON.stringify(args));
    });

    __bro_native.window._setSingleInstanceDispatcher(function (argsJson) {
        if (typeof singleInstanceCallback === 'function') {
            let args = [];
            try { args = JSON.parse(argsJson); } catch (_) {}
            try { singleInstanceCallback(args); } catch (e) { console.error(e); }
        }
    });

    fn(bro.window, 'shutdownSingleInstance', function shutdownSingleInstance() {
        singleInstanceCallback = null;
        __bro_native.window.shutdownSingleInstance();
    });

    fn(bro.window, 'minimize', function minimize() { __bro_native.window.minimize(); });
    fn(bro.window, 'maximize', function maximize() { __bro_native.window.maximize(); });
    fn(bro.window, 'restore', function restore() { __bro_native.window.restore(); });
    fn(bro.window, 'getPosition', function getPosition() {
        __bro_native.window.getPosition();
        return { x: __bro_native.window.getPosition_x(), y: __bro_native.window.getPosition_y() };
    });
    fn(bro.window, 'setPosition', function setPosition(x, y) {
        __bro_native.window.setPosition(x, y);
    });
    fn(bro.window, 'getMinSize', function getMinSize() {
        __bro_native.window.getMinSize();
        return { width: __bro_native.window.getMinSize_width(), height: __bro_native.window.getMinSize_height() };
    });
    fn(bro.window, 'setMinSize', function setMinSize(width, height) {
        __bro_native.window.setMinSize(width, height);
    });
    fn(bro.window, 'getMaxSize', function getMaxSize() {
        __bro_native.window.getMaxSize();
        return { width: __bro_native.window.getMaxSize_width(), height: __bro_native.window.getMaxSize_height() };
    });
    fn(bro.window, 'setMaxSize', function setMaxSize(width, height) {
        __bro_native.window.setMaxSize(width, height);
    });
    // One snapshot of the display list, then the documented DisplayInfo
    // objects read out of it by index. The two rectangles are carried both
    // flat (x, y, width, height, workX, ...) as the dictionary documents and
    // nested (`bounds`, `workArea`), the shape bro.window has always
    // answered and code was written against.
    fn(bro.window, 'getDisplays', function getDisplays() {
        const n = __bro_native.window.getDisplays();
        const out = [];
        for (let i = 0; i < n; i++) {
            const x = __bro_native.window.getDisplays_x(i);
            const y = __bro_native.window.getDisplays_y(i);
            const width = __bro_native.window.getDisplays_width(i);
            const height = __bro_native.window.getDisplays_height(i);
            const workX = __bro_native.window.getDisplays_workX(i);
            const workY = __bro_native.window.getDisplays_workY(i);
            const workWidth = __bro_native.window.getDisplays_workWidth(i);
            const workHeight = __bro_native.window.getDisplays_workHeight(i);
            out.push({
                id: __bro_native.window.getDisplays_id(i),
                name: __bro_native.window.getDisplays_name(i),
                x, y, width, height,
                workX, workY, workWidth, workHeight,
                bounds: { x, y, width, height },
                workArea: { x: workX, y: workY, width: workWidth, height: workHeight },
                refreshRate: __bro_native.window.getDisplays_refreshRate(i),
                contentScale: __bro_native.window.getDisplays_contentScale(i),
                isPrimary: __bro_native.window.getDisplays_isPrimary(i),
                isCurrent: __bro_native.window.getDisplays_isCurrent(i),
            });
        }
        return out;
    });
    fn(bro.window, 'moveToDisplay', function moveToDisplay(id) {
        return __bro_native.window.moveToDisplay(id);
    });
    fn(bro.window, 'getSize', function getSize() {
        __bro_native.window.getSize();
        return { width: __bro_native.window.getSize_width(), height: __bro_native.window.getSize_height() };
    });
    function setSize(width, height) {
        if (width === undefined || height === undefined) {
            throw new TypeError('bro.window.setSize: width and height are required');
        }
        const w = Math.round(Number(width)), h = Math.round(Number(height));
        if (!(w >= 1 && h >= 1)) throw new RangeError('bro.window.setSize: width and height must be >= 1');
        __bro_native.window.setSize(w, h);
    }
    fn(bro.window, 'setSize', setSize);
    // window.resizeTo / resizeBy: the main window's size, as bro.window.setSize.
    fn(globalThis, 'resizeTo', function resizeTo(width, height) { setSize(width, height); });
    fn(globalThis, 'resizeBy', function resizeBy(dx, dy) {
        const cur = bro.window.getSize();
        setSize(cur.width + (Number(dx) || 0), cur.height + (Number(dy) || 0));
    });

    // ---- bro.quit ------------------------------------------------------------
    // Stops the app as closing the main window does. A no-op headless.
    fn(bro, 'quit', function quit() { __bro_native.window.quit(); });

    // ---- bro.settings ------------------------------------------------------
    // The store is text. A value is typed by its content on the way out —
    // the one rule that works for the engine's own keys and an app's alike —
    // and split by JS type on the way in, so the store's own stringification
    // runs. An object or array is stored as JSON and parsed back.
    const NUMBER = /^-?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?$/;
    function typed(text) {
        if (text === '') return undefined;
        if (text === 'true') return true;
        if (text === 'false') return false;
        if (NUMBER.test(text)) return Number(text);
        const c = text.charCodeAt(0);
        if (c === 123 /* { */ || c === 91 /* [ */) {
            try { return JSON.parse(text); } catch (e) { return text; }
        }
        return text;
    }
    function typedCategory(obj) {
        // A custom category arrives as {key: "text"}; the engine's own
        // categories arrive already typed. Type only the strings that look
        // like something else.
        for (const k in obj) {
            if (typeof obj[k] === 'string') obj[k] = typed(obj[k]);
        }
        return obj;
    }
    function write(setString, setNumber, setBool, key, value) {
        if (value === undefined) throw new TypeError('bro.settings: a value is required for ' + key);
        if (typeof value === 'boolean') setBool(key, value);
        else if (typeof value === 'number') setNumber(key, value);
        else if (typeof value === 'string') setString(key, value);
        else setString(key, JSON.stringify(value));
    }
    function keyList(keys, what) {
        if (typeof keys === 'string') keys = [keys];
        if (!Array.isArray(keys)) throw new TypeError(what + ': keys must be an array of binding strings');
        return keys.map(String).join('\n');
    }

    fn(bro.settings, 'get', function get(key) {
        return typed(__bro_native.settings.get(String(key)));
    });
    fn(bro.settings, 'getAll', function getAll(category) {
        const all = JSON.parse(__bro_native.settings.getAllJson(category === undefined ? '' : String(category)));
        if (all === null) return {};
        return category === undefined ? all : typedCategory(all);
    });
    fn(bro.settings, 'getDefaults', function getDefaults(category) {
        const d = JSON.parse(__bro_native.settings.getDefaultsJson(category === undefined ? '' : String(category)));
        return d === null ? {} : d;
    });
    fn(bro.settings, 'set', function set(key, value) {
        write((k, v) => __bro_native.settings.setString(k, v),
              (k, v) => __bro_native.settings.setNumber(k, v),
              (k, v) => __bro_native.settings.setBool(k, v), String(key), value);
    });
    fn(bro.settings, 'setDefault', function setDefault(key, value) {
        write((k, v) => __bro_native.settings.setDefaultString(k, v),
              (k, v) => __bro_native.settings.setDefaultNumber(k, v),
              (k, v) => __bro_native.settings.setDefaultBool(k, v), String(key), value);
    });
    fn(bro.settings, 'reset', function reset(category) {
        __bro_native.settings.reset(category === undefined ? '' : String(category));
    });
    fn(bro.settings, 'defineAction', function defineAction(name, keys, options) {
        const deadzone = options && typeof options.deadzone === 'number' ? options.deadzone : -1;
        __bro_native.settings.defineAction(String(name), keyList(keys, 'bro.settings.defineAction'), deadzone);
    });
    fn(bro.settings, 'rebindAction', function rebindAction(name, keys) {
        __bro_native.settings.rebindAction(String(name), keyList(keys, 'bro.settings.rebindAction'));
    });
    fn(bro.settings, 'resetAction', function resetAction(name) {
        __bro_native.settings.resetAction(String(name));
    });
    fn(bro.settings, 'resetAllActions', function resetAllActions() {
        __bro_native.settings.resetAllActions();
    });
    fn(bro.settings, 'getActionKeys', function getActionKeys(name) {
        return JSON.parse(__bro_native.settings.actionKeysJson(String(name)));
    });
    fn(bro.settings, 'getKeyAction', function getKeyAction(key) {
        const a = __bro_native.settings.keyAction(String(key));
        return a === '' ? null : a;
    });
    fn(bro.settings, 'getActionStrength', function getActionStrength(name) {
        return __bro_native.settings.actionStrength(String(name));
    });
    fn(bro.settings, 'isActionPressed', function isActionPressed(name) {
        return __bro_native.settings.isActionPressed(String(name));
    });
    fn(bro.settings, 'getActions', function getActions() {
        return JSON.parse(__bro_native.settings.actionsJson());
    });
    fn(bro.settings, 'getAppActions', function getAppActions() {
        return JSON.parse(__bro_native.settings.appActionsJson());
    });
    fn(bro.settings, 'getDisplayModes', function getDisplayModes() {
        return JSON.parse(__bro_native.settings.displayModesJson());
    });
    // Called with (category, key) after the engine has applied a change,
    // from the frame seam — never from inside the set() that made it.
    fn(bro.settings, 'onChange', function onChange(listener) {
        if (typeof listener !== 'function') {
            throw new TypeError('bro.settings.onChange: listener must be a function');
        }
        __bro_native.settings.onChange(listener);
    });

    // ---- __bro.splash / __bro.viewport ------------------------------------
    fn(__bro.splash, 'dismiss', function dismiss() { __bro_native.splash.dismiss(); });
    accessor(__bro.viewport, 'width', () => __bro_native.viewport.width, undefined);
    accessor(__bro.viewport, 'height', () => __bro_native.viewport.height, undefined);

    // ---- __bro.perf --------------------------------------------------------
    // Getters over the engine's own 500 ms frame statistics: perf.html reads
    // them each tick and writes its own DOM.
    accessor(__bro.perf, 'fps', () => __bro_native.perf.fps, undefined);
    accessor(__bro.perf, 'frameTime', () => __bro_native.perf.frameTime, undefined);
    accessor(__bro.perf, 'js', () => __bro_native.perf.js, undefined);
    accessor(__bro.perf, 'layout', () => __bro_native.perf.layout, undefined);
    accessor(__bro.perf, 'raster', () => __bro_native.perf.raster, undefined);
    accessor(__bro.perf, 'gpu', () => __bro_native.perf.gpu, undefined);
    accessor(__bro.perf, 'draw', () => __bro_native.perf.draw, undefined);
    accessor(__bro.perf, 'windows', () => {
        const n = __bro_native.perf.windowCount();
        const out = [];
        for (let i = 0; i < n; i++) {
            out.push({
                id: __bro_native.perf.windowId(i),
                title: __bro_native.perf.windowTitle(i),
                width: __bro_native.perf.windowWidth(i),
                height: __bro_native.perf.windowHeight(i),
                focused: __bro_native.perf.windowFocused(i),
                minimized: __bro_native.perf.windowMinimized(i),
            });
        }
        return out;
    }, undefined);
    // The 3D cull counters exist only in a build with the scene graph
    // (BRO_WITH_3D); the namespace object is there exactly when they are.
    if (__bro_native.perf.scene) {
        accessor(__bro.perf, 'scene', () => ({
            meshDrawn: __bro_native.perf.scene.meshDrawn,
            meshCulled: __bro_native.perf.scene.meshCulled,
            instancedDrawn: __bro_native.perf.scene.instancedDrawn,
            instancedCulled: __bro_native.perf.scene.instancedCulled,
            splatDrawn: __bro_native.perf.scene.splatDrawn,
            splatCulled: __bro_native.perf.scene.splatCulled,
            particlesDrawn: __bro_native.perf.scene.particlesDrawn,
            particlesCulled: __bro_native.perf.scene.particlesCulled,
            billboardsDrawn: __bro_native.perf.scene.billboardsDrawn,
            billboardsCulled: __bro_native.perf.scene.billboardsCulled,
            decalsDrawn: __bro_native.perf.scene.decalsDrawn,
            decalsCulled: __bro_native.perf.scene.decalsCulled,
            shadowDrawn: __bro_native.perf.scene.shadowDrawn,
            shadowCulled: __bro_native.perf.scene.shadowCulled,
            shadowTilesTotal: __bro_native.perf.scene.shadowTilesTotal,
            shadowTilesRendered: __bro_native.perf.scene.shadowTilesRendered,
            shadowTilesCached: __bro_native.perf.scene.shadowTilesCached,
        }), undefined);
    }

    // ---- __bro.bronze ------------------------------------------------------
    accessor(__bro.bronze, 'heapUsedBytes', () => __bro_native.bronze.heapUsedBytes, undefined);
    accessor(__bro.bronze, 'heapCommittedBytes', () => __bro_native.bronze.heapCommittedBytes, undefined);
    accessor(__bro.bronze, 'heapReservedBytes', () => __bro_native.bronze.heapReservedBytes, undefined);
    accessor(__bro.bronze, 'gcCollections', () => __bro_native.bronze.gcCollections, undefined);
    accessor(__bro.bronze, 'gcPauseNs', () => __bro_native.bronze.gcPauseNs, undefined);
    accessor(__bro.bronze, 'shapeTransitions', () => __bro_native.bronze.shapeTransitions, undefined);

    // ---- __bro.menu --------------------------------------------------------
    fn(__bro.menu, 'getHeight', function getHeight() { return __bro_native.menu.height(); });
    fn(__bro.menu, 'getTree', function getTree() { return JSON.parse(__bro_native.menu.treeJson()); });
    fn(__bro.menu, 'click', function click(id) { __bro_native.menu.click(String(id)); });

    // ---- __bro.settingsUI --------------------------------------------------
    const panels = () => JSON.parse(__bro_native.settingsUI.panelsJson());
    fn(__bro.settingsUI, 'show', function show(name) { __bro_native.settingsUI.show(String(name)); });
    fn(__bro.settingsUI, 'getAllPanels', function getAllPanels() {
        return panels().filter((p) => p.tabLabel !== '').map((p) => ({ name: p.name, tabLabel: p.tabLabel }));
    });
    fn(__bro.settingsUI, 'getSettingsPanels', function getSettingsPanels() {
        return panels().filter((p) => p.group === 'settings').map((p) => ({ name: p.name, label: p.tabLabel }));
    });
    fn(__bro.settingsUI, 'getActivePanel', function getActivePanel() {
        return __bro_native.settingsUI.activePanel();
    });
    fn(__bro.settingsUI, 'toggle', function toggle() { __bro_native.settingsUI.toggle(); });
    fn(__bro.settingsUI, 'isVisible', function isVisible() { return __bro_native.settingsUI.isVisible(); });
    fn(__bro.settingsUI, 'getViewport', function getViewport() {
        return {
            width: __bro_native.viewport.width,
            height: __bro_native.viewport.height,
            contentTop: __bro_native.settingsUI.contentTop(),
        };
    });

    // ---- __bro.inspector ---------------------------------------------------
    fn(__bro.inspector, 'getLayout', function getLayout() {
        return {
            visible: __bro_native.inspector.visible,
            dock: __bro_native.inspector.dock,
            width: __bro_native.inspector.width,
            height: __bro_native.inspector.height,
            pickerMode: __bro_native.inspector.pickerMode,
            viewportWidth: __bro_native.viewport.width,
            viewportHeight: __bro_native.viewport.height,
            menuTop: __bro_native.menu.height(),
        };
    });
    fn(__bro.inspector, 'getAppTree', function getAppTree(maxDepth) {
        return JSON.parse(__bro_native.inspector.appTreeJson(maxDepth === undefined ? 2 : maxDepth));
    });
    fn(__bro.inspector, 'getAppChildren', function getAppChildren(id) {
        return JSON.parse(__bro_native.inspector.childrenJson(id));
    });
    fn(__bro.inspector, 'getSelected', function getSelected() {
        return JSON.parse(__bro_native.inspector.selectedJson());
    });
    fn(__bro.inspector, 'select', function select(id) { __bro_native.inspector.select(id); });
    fn(__bro.inspector, 'setDock', function setDock(dock) { __bro_native.inspector.setDock(String(dock)); });
    fn(__bro.inspector, 'setSize', function setSize(px) { __bro_native.inspector.setSize(px); });
    fn(__bro.inspector, 'setPickerMode', function setPickerMode(on) { __bro_native.inspector.setPickerMode(!!on); });
    fn(__bro.inspector, 'toggle', function toggle() { __bro_native.inspector.toggle(); });
})();
