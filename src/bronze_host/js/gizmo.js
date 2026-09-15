// gizmo.js — the public shape of bro.gizmo, assembled over the
// natives under __bro_native.gizmo.
(function () {
    'use strict';

    const accessor = (obj, name, get, set) =>
        Object.defineProperty(obj, name, { get, set, enumerable: true, configurable: true });
    const fn = (obj, name, value) =>
        Object.defineProperty(obj, name, { value, writable: true, enumerable: true, configurable: true });
    const mount = (root, name) => root[name] !== undefined ? root[name] : (root[name] = {});

    // ---- bro.gizmo -----------------------------------------------------------
    const ns_gizmo = mount(bro, "gizmo");
    accessor(ns_gizmo, "visible",
        function () {
            return __bro_native.gizmo.visible;
        },
        undefined);
    accessor(ns_gizmo, "dragging",
        function () {
            return __bro_native.gizmo.dragging;
        },
        undefined);
    accessor(ns_gizmo, "hovered", () => null, undefined);
    fn(ns_gizmo, "show", function show() {
        __bro_native.gizmo.show();
    });
    fn(ns_gizmo, "hide", function hide() {
        __bro_native.gizmo.hide();
    });
    fn(ns_gizmo, "setMode", function setMode(mode) {
        if (mode === undefined) throw new TypeError("bro.gizmo.setMode: mode is required");
        __bro_native.gizmo.setMode(mode);
    });
    fn(ns_gizmo, "setSpace", function setSpace(space) {
        if (space === undefined) throw new TypeError("bro.gizmo.setSpace: space is required");
        __bro_native.gizmo.setSpace(space);
    });
    fn(ns_gizmo, "setPosition", function setPosition(x, y, z) {
        if (x === undefined) throw new TypeError("bro.gizmo.setPosition: x is required");
        if (y === undefined) throw new TypeError("bro.gizmo.setPosition: y is required");
        if (z === undefined) throw new TypeError("bro.gizmo.setPosition: z is required");
        __bro_native.gizmo.setPosition(x, y, z);
    });
    fn(ns_gizmo, "setOrientation", function setOrientation(x, y, z, w) {
        if (x === undefined) throw new TypeError("bro.gizmo.setOrientation: x is required");
        if (y === undefined) throw new TypeError("bro.gizmo.setOrientation: y is required");
        if (z === undefined) throw new TypeError("bro.gizmo.setOrientation: z is required");
        if (w === undefined) throw new TypeError("bro.gizmo.setOrientation: w is required");
        __bro_native.gizmo.setOrientation(x, y, z, w);
    });
    fn(ns_gizmo, "configure", function configure(config) {
        if (config === undefined) throw new TypeError("bro.gizmo.configure: config is required");
        const d_config = config;
        const d_config_colors = d_config.colors === undefined ? {} : d_config.colors;
        __bro_native.gizmo.configure(d_config.size !== undefined, d_config.size === undefined ? 0 : d_config.size, d_config_colors.x !== undefined, d_config_colors.x === undefined ? '' : d_config_colors.x, d_config_colors.y !== undefined, d_config_colors.y === undefined ? '' : d_config_colors.y, d_config_colors.z !== undefined, d_config_colors.z === undefined ? '' : d_config_colors.z, d_config_colors.hover !== undefined, d_config_colors.hover === undefined ? '' : d_config_colors.hover, d_config_colors.active !== undefined, d_config_colors.active === undefined ? '' : d_config_colors.active, d_config.emissive !== undefined, d_config.emissive === undefined ? 0 : d_config.emissive, d_config.emissiveHover !== undefined, d_config.emissiveHover === undefined ? 0 : d_config.emissiveHover, d_config.alwaysOnTop !== undefined, d_config.alwaysOnTop === undefined ? false : d_config.alwaysOnTop);
    });
    fn(ns_gizmo, "attach", function attach(handlers) {
        if (handlers === undefined) throw new TypeError("bro.gizmo.attach: handlers is required");
        const d_handlers = handlers;
        __bro_native.gizmo.attach(d_handlers.position, d_handlers.orientation, d_handlers.beginDrag, d_handlers.translate, d_handlers.rotate, d_handlers.scale, d_handlers.endDrag, d_handlers.hoverChange);
    });
    fn(ns_gizmo, "detach", function detach() {
        __bro_native.gizmo.detach();
    });
})();
