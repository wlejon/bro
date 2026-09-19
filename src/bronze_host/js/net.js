// net.js — the public shape of bro.net, assembled over the
// natives under __bro_native.net.

(function () {
    'use strict';

    const accessor = (obj, name, get, set) =>
        Object.defineProperty(obj, name, { get, set, enumerable: true, configurable: true });
    const fn = (obj, name, value) =>
        Object.defineProperty(obj, name, { value, writable: true, enumerable: true, configurable: true });
    const mount = (root, name) => root[name] !== undefined ? root[name] : (root[name] = {});
    // A raw payload is bytes: any ArrayBufferView, an ArrayBuffer, or a string
    // (UTF-8). `Uint8Array.from("text")` would coerce each character to NaN
    // and send zeros, which is why strings are encoded explicitly.
    const toU8 = (v) => {
        if (v instanceof Uint8Array) return v;
        if (ArrayBuffer.isView(v)) return new Uint8Array(v.buffer, v.byteOffset, v.byteLength);
        if (v instanceof ArrayBuffer) return new Uint8Array(v);
        if (typeof v === 'string') return new globalThis.TextEncoder().encode(v);
        if (v === null || v === undefined) return new Uint8Array(0);
        if (typeof v === 'object' && typeof v[Symbol.iterator] === 'function') return Uint8Array.from(v);
        return new globalThis.TextEncoder().encode(String(v));
    };

    // ---- bro.net -------------------------------------------------------------
    // The root off globalThis, never as a bare `bro`: a bare host-global read
    // is cached in this module's own data, which is one per compiled object
    // and not one per thread, and this module is entered on every Worker's
    // thread as well as the main one (host_worker.cpp). A cell the main realm
    // filled would hand the worker the main realm's root.
    const ns_net = mount(globalThis.bro, "net");

    fn(ns_net, "init", function init() {
        return __bro_native.net.init();
    });

    fn(ns_net, "host", function host(port, callback) {
        if (port === undefined) throw new TypeError("bro.net.host: port is required");
        __bro_native.net.host(port, callback);
        return true;
    });

    fn(ns_net, "unhost", function unhost() {
        __bro_native.net.unhost();
    });

    fn(ns_net, "close", function close() {
        __bro_native.net.unhost();
    });

    fn(ns_net, "closeHost", function closeHost() {
        __bro_native.net.unhost();
    });

    fn(ns_net, "isHosting", function isHosting() {
        return __bro_native.net.isHosting();
    });

    fn(ns_net, "connect", function connect(address, port, callback) {
        if (address === undefined) throw new TypeError("bro.net.connect: address is required");
        return __bro_native.net.connect(address, port === undefined ? 0 : port, callback);
    });

    fn(ns_net, "disconnect", function disconnect(peerId) {
        if (peerId === undefined) throw new TypeError("bro.net.disconnect: peerId is required");
        __bro_native.net.disconnect(peerId);
    });

    fn(ns_net, "disconnectAll", function disconnectAll() {
        __bro_native.net.disconnectAll();
    });

    fn(ns_net, "connections", function connections() {
        return Array.from(__bro_native.net.peers());
    });

    fn(ns_net, "peers", function peers() {
        return Array.from(__bro_native.net.peers());
    });

    // The third argument is `{reliable, channel, nodelay}`, a bare channel
    // number, or the legacy boolean `reliable`; the native reads all three
    // spellings (parseSendOptions, native_net.cpp).
    fn(ns_net, "send", function send(peerId, data, options) {
        if (peerId === undefined) throw new TypeError("bro.net.send: peerId is required");
        if (data === undefined) throw new TypeError("bro.net.send: data is required");
        __bro_native.net.sendRawOpts(peerId, toU8(data), options === undefined ? 0 : options);
    });

    fn(ns_net, "broadcast", function broadcast(data, options) {
        if (data === undefined) throw new TypeError("bro.net.broadcast: data is required");
        __bro_native.net.broadcastRawOpts(toU8(data), options === undefined ? 0 : options);
    });

    fn(ns_net, "sendClone", function sendClone(peerId, value, options) {
        if (peerId === undefined) throw new TypeError("bro.net.sendClone: peerId is required");
        return __bro_native.net.sendClone(peerId, value, options);
    });

    fn(ns_net, "broadcastClone", function broadcastClone(value, options) {
        __bro_native.net.broadcastClone(value, options);
    });

    fn(ns_net, "getPeerAddress", function getPeerAddress(peerId) {
        if (peerId === undefined) throw new TypeError("bro.net.getPeerAddress: peerId is required");
        return __bro_native.net.getPeerAddress(peerId);
    });

    fn(ns_net, "stats", function stats(peerId) {
        if (peerId !== undefined) {
            return JSON.parse(__bro_native.net.getPeerStats(peerId));
        }
        return JSON.parse(__bro_native.net.stats());
    });

    fn(ns_net, "getPeerStats", function getPeerStats(peerId) {
        if (peerId === undefined) throw new TypeError("bro.net.getPeerStats: peerId is required");
        return JSON.parse(__bro_native.net.getPeerStats(peerId));
    });

    fn(ns_net, "setPeerSimulatedLoss", function setPeerSimulatedLoss(peerId, chance, latencyMin, latencyMax) {
        if (peerId === undefined) throw new TypeError("bro.net.setPeerSimulatedLoss: peerId is required");
        if (chance === undefined) throw new TypeError("bro.net.setPeerSimulatedLoss: chance is required");
        if (latencyMin === undefined) throw new TypeError("bro.net.setPeerSimulatedLoss: latencyMin is required");
        if (latencyMax === undefined) throw new TypeError("bro.net.setPeerSimulatedLoss: latencyMax is required");
        __bro_native.net.setPeerSimulatedLoss(peerId, chance, latencyMin, latencyMax);
    });

    const listeners = {
        connect: new Set(),
        disconnect: new Set(),
        message: new Set(),
    };

    let onConnectHandler = null;
    let onDisconnectHandler = null;
    let onMessageHandler = null;

    accessor(ns_net, "onConnect", () => onConnectHandler, (fn) => { onConnectHandler = fn; });
    accessor(ns_net, "onconnect", () => onConnectHandler, (fn) => { onConnectHandler = fn; });
    accessor(ns_net, "onDisconnect", () => onDisconnectHandler, (fn) => { onDisconnectHandler = fn; });
    accessor(ns_net, "ondisconnect", () => onDisconnectHandler, (fn) => { onDisconnectHandler = fn; });
    accessor(ns_net, "onMessage", () => onMessageHandler, (fn) => { onMessageHandler = fn; });
    accessor(ns_net, "onmessage", () => onMessageHandler, (fn) => { onMessageHandler = fn; });

    fn(ns_net, "addEventListener", function addEventListener(type, listener) {
        if (listeners[type]) listeners[type].add(listener);
    });

    fn(ns_net, "removeEventListener", function removeEventListener(type, listener) {
        if (listeners[type]) listeners[type].delete(listener);
    });

    __bro_native.net._setDispatcher(function (type, a1, a2, a3) {
        if (type === "connect") {
            if (typeof onConnectHandler === 'function') onConnectHandler(a1);
            for (const l of listeners.connect) l(a1);
        } else if (type === "disconnect") {
            if (typeof onDisconnectHandler === 'function') onDisconnectHandler(a1, a2);
            for (const l of listeners.disconnect) l(a1, a2);
        } else if (type === "message") {
            if (typeof onMessageHandler === 'function') onMessageHandler(a1, a2, a3);
            for (const l of listeners.message) l(a1, a2, a3);
        }
    });

    // bro.net.sync: js/net_sync.js is a factory over these primitives, entered
    // before this module (dom_globals.cpp). Unconditional on purpose — an
    // install order that ran it after this one is a bug to hear about, not
    // a `sync` that quietly does not exist.
    ns_net.sync = globalThis.__bro_net_sync(ns_net);
})();
