// The web `Notification` API over bro.window.notify (docs/sys-api.js has
// the page): an app posts a desktop notification as a page does,
//
//   new Notification('Download finished', { body: 'report.pdf', icon: 'assets/done.png',
//                                           data: { path }, actions: [{ action: 'open', title: 'Open' }] })
//
// and the desktop shows it under the app's own name and id: a toast on
// Windows, the notification center on macOS, org.freedesktop.Notifications
// on Linux (platform/desktop_notifications.cpp). Headless records it
// (notifications()) and shows nothing.
//
// A desktop app does not ask its user for leave to notify, so `permission`
// is 'granted' and requestPermission() resolves 'granted' at once. `show`
// fires once the desktop took the notification, `error` when nothing could
// show it, `close` on close() or when the user dismisses it.
//
// CLICKS. A click on the notification, or on one of its `actions` buttons,
// fires `click` on it, with `event.action` the button's action ('' for the
// notification itself) and `event.notification` the notification; bro
// brings the app's window forward first. A click the page holds no listening
// Notification for — one an earlier run posted, which started this run, or
// one from before a reload — fires `notificationclick` on window instead, as
// a service worker's would: `event.notification` is rebuilt from what was
// posted (title, body, tag, icon, data, actions) and `event.action` is the
// button. A dismissal likewise: `close`, else window's `notificationclose`.
// The native side calls __bro_notificationActivated (host_notification.cpp).
//
// HOW IT IS SHIPPED. Compiled by bronze at build time (bro_compile_js in
// ../CMakeLists.txt, against js/module.globals) and entered from
// installNotificationModule() (host_js_modules.cpp); the installer lifts
// `Notification` into the host-global registry. bro.window.notify is read at
// the point of use, so the module may load before bro_core.js.
(function () {
    'use strict';

    const g = globalThis;
    const state = new WeakMap();   // Notification -> { listeners, closed, id }
    const tagIds = new Map();      // tag -> the native id it was last shown with
    const live = new Map();        // native id -> the Notification the page holds
    const MAX_ACTIONS = 4;

    function makeEvent(type, target, extra) {
        let ev = null;
        if (typeof g.Event === 'function') {
            try { ev = new g.Event(type); } catch (e) { ev = null; }
        }
        if (!ev) ev = { type };
        try {
            if (target) {
                Object.defineProperty(ev, 'target', { value: target, configurable: true });
                Object.defineProperty(ev, 'currentTarget', { value: target, configurable: true });
            }
            if (extra) {
                for (const k of Object.keys(extra)) {
                    Object.defineProperty(ev, k, { value: extra[k], enumerable: true, configurable: true });
                }
            }
        } catch (e) { /* the Event's own accessors stay */ }
        return ev;
    }

    function listens(target, type) {
        if (typeof target['on' + type] === 'function') return true;
        const s = state.get(target);
        const list = s && s.listeners.get(type);
        return !!(list && list.size);
    }

    function fire(target, type, extra) {
        const ev = makeEvent(type, target, extra);
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

    function normalizeActions(list) {
        if (list === undefined || list === null) return [];
        if (typeof list !== 'object' || typeof list.length !== 'number') {
            throw new TypeError("Failed to construct 'Notification': The provided value cannot be converted to a sequence.");
        }
        const out = [];
        for (let i = 0; i < list.length && out.length < MAX_ACTIONS; i++) {
            const a = list[i];
            if (!a || typeof a !== 'object') continue;
            if (a.action === undefined || a.title === undefined) {
                throw new TypeError("Failed to construct 'Notification': an action needs `action` and `title`.");
            }
            out.push(Object.freeze({
                action: String(a.action),
                title: String(a.title),
                icon: a.icon === undefined ? '' : String(a.icon),
            }));
        }
        return out;
    }

    // What a click hands back, also to a later run: the notification as
    // posted. `data` goes as JSON (what JSON cannot hold is dropped).
    function payloadOf(n) {
        const base = {
            title: n.title, body: n.body, tag: n.tag, icon: n.icon, lang: n.lang, dir: n.dir,
            timestamp: n.timestamp, actions: n.actions.map((a) => ({ action: a.action, title: a.title, icon: a.icon })),
        };
        try {
            return JSON.stringify(Object.assign({}, base, { data: n.data }));
        } catch (e) {
            return JSON.stringify(Object.assign({}, base, { data: null }));
        }
    }

    // A Notification for what an earlier run (or an earlier page) posted:
    // its fields, shown nowhere again.
    function rebuild(payload) {
        let p = {};
        try { p = JSON.parse(payload) || {}; } catch (e) { p = {}; }
        const n = Object.create(Notification.prototype);
        const def = (name, value) => Object.defineProperty(n, name, { value, enumerable: true, configurable: true });
        def('title', typeof p.title === 'string' ? p.title : '');
        def('body', typeof p.body === 'string' ? p.body : '');
        def('icon', typeof p.icon === 'string' ? p.icon : '');
        def('tag', typeof p.tag === 'string' ? p.tag : '');
        def('silent', null);
        def('requireInteraction', false);
        def('renotify', false);
        def('data', p.data === undefined ? null : p.data);
        def('dir', p.dir === 'ltr' || p.dir === 'rtl' ? p.dir : 'auto');
        def('lang', typeof p.lang === 'string' ? p.lang : '');
        def('badge', '');
        def('image', '');
        def('timestamp', typeof p.timestamp === 'number' ? p.timestamp : 0);
        def('actions', Object.freeze(Array.isArray(p.actions) ? p.actions.map((a) => Object.freeze({
            action: String(a.action), title: String(a.title), icon: a.icon ? String(a.icon) : '',
        })) : []));
        def('vibrate', Object.freeze([]));
        n.onclick = null;
        n.onshow = null;
        n.onerror = null;
        n.onclose = null;
        state.set(n, { listeners: new Map(), closed: true, id: 0 });
        return n;
    }

    function dispatchOnWindow(type, extra) {
        const target = g.window || g;
        if (!target || typeof target.dispatchEvent !== 'function') return;
        const ev = makeEvent(type, null, extra);
        try { target.dispatchEvent(ev); } catch (e) { console.error(e); }
    }

    // host_notification.cpp: the user clicked (type 'click', with `action`)
    // or dismissed ('close') the notification posted as native `id` with
    // `payload`; `earlierRun` when another run of the app posted it.
    function activated(type, id, action, payload, earlierRun) {
        const n = !earlierRun && id > 0 ? live.get(id) : undefined;
        if (type === 'close') {
            if (n) {
                live.delete(id);
                const s = state.get(n);
                if (s && !s.closed) {
                    s.closed = true;
                    if (listens(n, 'close')) { fire(n, 'close'); return; }
                }
            }
            dispatchOnWindow('notificationclose', { notification: n || rebuild(payload) });
            return;
        }
        const act = action || '';
        if (n && listens(n, 'click')) {
            fire(n, 'click', { action: act, notification: n });
            return;
        }
        dispatchOnWindow('notificationclick', { notification: n || rebuild(payload), action: act });
    }
    g.__bro_notificationActivated = activated;

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
            def('actions', Object.freeze(normalizeActions(o.actions)));
            def('vibrate', Object.freeze([]));
            this.onclick = null;
            this.onshow = null;
            this.onerror = null;
            this.onclose = null;
            const s = { listeners: new Map(), closed: false, id: 0 };
            state.set(this, s);

            let id = 0;
            const bro = g.bro;
            if (bro && bro.window && typeof bro.window.notify === 'function') {
                try {
                    id = bro.window.notify(this.title, this.body, {
                        icon: iconPath(o.icon),
                        silent: !!o.silent,
                        timeout: this.requireInteraction ? 0 : -1,
                        replacesId: this.tag && tagIds.has(this.tag) ? tagIds.get(this.tag) : 0,
                        actions: this.actions,
                        payload: payloadOf(this),
                    });
                } catch (e) {
                    id = 0;
                }
            }
            if (id > 0) {
                s.id = id;
                live.set(id, this);
                if (this.tag) tagIds.set(this.tag, id);
            }
            // Events are tasks, after the constructor returns and the page
            // has had the chance to set onshow.
            const self = this;
            g.setTimeout(function () { fire(self, id > 0 ? 'show' : 'error'); }, 0);
        }

        close() {
            const s = state.get(this);
            if (!s || s.closed) return;
            s.closed = true;
            if (s.id && live.get(s.id) === this) live.delete(s.id);
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

        static get maxActions() { return MAX_ACTIONS; }
    }
    Object.defineProperty(Notification.prototype, Symbol.toStringTag, {
        value: 'Notification', configurable: true,
    });

    g.Notification = Notification;
})();
