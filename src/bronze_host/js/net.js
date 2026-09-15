// net.js — the public shape of bro.net, assembled over the
// natives under __bro_native.net.

(function () {
    'use strict';

    const accessor = (obj, name, get, set) =>
        Object.defineProperty(obj, name, { get, set, enumerable: true, configurable: true });
    const fn = (obj, name, value) =>
        Object.defineProperty(obj, name, { value, writable: true, enumerable: true, configurable: true });
    const mount = (root, name) => root[name] !== undefined ? root[name] : (root[name] = {});
    const toU8 = (v) => v instanceof Uint8Array ? v : Uint8Array.from(v);

    // ---- bro.net -------------------------------------------------------------
    const ns_net = mount(bro, "net");

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

    fn(ns_net, "send", function send(peerId, data, channel) {
        if (peerId === undefined) throw new TypeError("bro.net.send: peerId is required");
        if (data === undefined) throw new TypeError("bro.net.send: data is required");
        __bro_native.net.send(peerId, toU8(data), channel === undefined ? 0 : channel);
    });

    fn(ns_net, "broadcast", function broadcast(data, channel) {
        if (data === undefined) throw new TypeError("bro.net.broadcast: data is required");
        __bro_native.net.broadcast(toU8(data), channel === undefined ? 0 : channel);
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

    // Mount net.sync if available
    if (typeof globalThis.__bro_net_sync === 'function') {
        ns_net.sync = globalThis.__bro_net_sync(ns_net);
    }
})();
