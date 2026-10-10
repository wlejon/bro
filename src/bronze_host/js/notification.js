// The web `Notification` API over bro.window.notify (docs/sys-api.js has
// the page): an app posts a desktop notification as a page does,
//
//   new Notification('Download finished', { body: 'report.pdf', icon: 'assets/done.png' })
//
// and the desktop shows it under the app's own name and id: a toast on
// Windows, the notification center on macOS, org.freedesktop.Notifications
// on Linux (platform/desktop_notifications.cpp). Headless records it
// (notifications()) and shows nothing.
//
// A desktop app does not ask its user for leave to notify, so `permission`
// is 'granted' and requestPermission() resolves 'granted' at once. `show`
// fires once the desktop took the notification, `error` when nothing could
// show it, `close` on close(). `click` is not delivered (the platforms'
// activation callbacks are not wired up yet).
//
// HOW IT IS SHIPPED. Compiled by bronze at build time (bro_compile_js in
// ../CMakeLists.txt, against js/module.globals) and entered from
// installNotificationModule() (host_js_modules.cpp); the installer lifts
// `Notification` into the host-global registry. bro.window.notify is read at
// the point of use, so the module may load before bro_core.js.
(function () {
    'use strict';

    const g = globalThis;
    const state = new WeakMap();   // Notification -> { listeners, closed }
    const tagIds = new Map();      // tag -> the native id it was last shown with

    function makeEvent(type, target) {
        let ev = null;
        if (typeof g.Event === 'function') {
            try { ev = new g.Event(type); } catch (e) { ev = null; }
        }
        if (!ev) ev = { type };
        try {
            Object.defineProperty(ev, 'target', { value: target, configurable: true });
            Object.defineProperty(ev, 'currentTarget', { value: target, configurable: true });
        } catch (e) { /* the Event's own accessors stay */ }
        return ev;
    }

    function fire(target, type) {
        const ev = makeEvent(type, target);
        const handler = target['on' + type];
        if (typeof handler === 'function') {
            try { handler.call(target, ev); } catch (e) { console.error(e); }
        }
        const s = state.get(target);
        const list = s && s.listeners.get(type);
        if (list) {
            for (const l of Array.from(list)) {
                try {
                    if (typeof l === 'function') l.call(target, ev);
                    else if (l && typeof l.handleEvent === 'function') l.handleEvent(ev);
                } catch (e) { console.error(e); }
            }
        }
    }

    // An icon as a file the desktop can read: an absolute path as it is, an
    // app-relative or mounted one (/app/..., assets/x.png) through
    // bro.resolvePath; none at all is the app's own icon.
    function iconPath(icon) {
        const bro = g.bro;
        if (icon === undefined || icon === null || icon === '') {
            return bro && bro.app && typeof bro.app.icon === 'string' ? bro.app.icon : '';
        }
        const s = String(icon);
        if (/^file:\/\//i.test(s)) {
            try { return decodeURIComponent(s.replace(/^file:\/\/(localhost)?/i, '').replace(/^\/([A-Za-z]:)/, '$1')); }
            catch (e) { return s; }
        }
        if (/^[A-Za-z]:[\\/]/.test(s) || s.startsWith('\\\\')) return s;
        if (s.charAt(0) === '/' && !/^\/(app|lib|system|std)\//.test(s)) return s;
        if (bro && typeof bro.resolvePath === 'function') {
            try { return bro.resolvePath(s) || s; } catch (e) { return s; }
        }
        return s;
    }

    class Notification {
        constructor(title, options) {
            if (new.target === undefined) {
                throw new TypeError("Failed to construct 'Notification': Please use the 'new' operator.");
            }
            if (arguments.length < 1) {
                throw new TypeError("Failed to construct 'Notification': 1 argument required, but only 0 present.");
            }
            const o = options === undefined || options === null ? {} : options;
            if (typeof o !== 'object') {
                throw new TypeError("Failed to construct 'Notification': The options are not an object.");
            }
            const def = (name, value) => Object.defineProperty(this, name, { value, enumerable: true, configurable: true });
            def('title', String(title));
            def('body', o.body === undefined ? '' : String(o.body));
            def('icon', o.icon === undefined ? '' : String(o.icon));
            def('tag', o.tag === undefined ? '' : String(o.tag));
            def('silent', o.silent === undefined || o.silent === null ? null : !!o.silent);
            def('requireInteraction', !!o.requireInteraction);
            def('renotify', !!o.renotify);
            def('data', o.data === undefined ? null : o.data);
            def('dir', o.dir === 'ltr' || o.dir === 'rtl' ? o.dir : 'auto');
            def('lang', o.lang === undefined ? '' : String(o.lang));
            def('badge', o.badge === undefined ? '' : String(o.badge));
            def('image', o.image === undefined ? '' : String(o.image));
            def('timestamp', typeof o.timestamp === 'number' ? o.timestamp : Date.now());
            def('actions', []);
            def('vibrate', []);
            this.onclick = null;
            this.onshow = null;
            this.onerror = null;
            this.onclose = null;
            state.set(this, { listeners: new Map(), closed: false });

            let id = 0;
            const bro = g.bro;
            if (bro && bro.window && typeof bro.window.notify === 'function') {
                try {
                    id = bro.window.notify(this.title, this.body, {
                        icon: iconPath(o.icon),
                        silent: !!o.silent,
                        timeout: this.requireInteraction ? 0 : -1,
                        replacesId: this.tag && tagIds.has(this.tag) ? tagIds.get(this.tag) : 0,
                    });
                } catch (e) {
                    id = 0;
                }
            }
            if (id > 0 && this.tag) tagIds.set(this.tag, id);
            // Events are tasks, after the constructor returns and the page
            // has had the chance to set onshow.
            const self = this;
            g.setTimeout(function () { fire(self, id > 0 ? 'show' : 'error'); }, 0);
        }

        close() {
            const s = state.get(this);
            if (!s || s.closed) return;
            s.closed = true;
            const self = this;
            g.setTimeout(function () { fire(self, 'close'); }, 0);
        }

        addEventListener(type, listener) {
            if (listener === null || listener === undefined) return;
            const s = state.get(this);
            if (!s) return;
            const key = String(type);
            if (!s.listeners.has(key)) s.listeners.set(key, new Set());
            s.listeners.get(key).add(listener);
        }

        removeEventListener(type, listener) {
            const s = state.get(this);
            const list = s && s.listeners.get(String(type));
            if (list) list.delete(listener);
        }

        dispatchEvent(event) {
            if (!event || typeof event.type !== 'string') {
                throw new TypeError("Failed to execute 'dispatchEvent' on 'EventTarget': parameter 1 is not of type 'Event'.");
            }
            fire(this, event.type);
            return true;
        }

        static get permission() { return 'granted'; }

        static requestPermission(callback) {
            if (typeof callback === 'function') {
                g.setTimeout(function () { callback('granted'); }, 0);
            }
            return Promise.resolve('granted');
        }

        static get maxActions() { return 0; }
    }
    Object.defineProperty(Notification.prototype, Symbol.toStringTag, {
        value: 'Notification', configurable: true,
    });

    g.Notification = Notification;
})();
