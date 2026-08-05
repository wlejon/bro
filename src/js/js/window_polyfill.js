(function() {
    var listeners = {};
    globalThis.__bro_win_listeners = listeners;
    globalThis.addEventListener = function(type, fn, opts) {
        if (!listeners[type]) listeners[type] = [];
        // `seq` is the shared C++/JS registration counter (see
        // dom/event_target.h): it is what lets __bro_dispatch_window_event
        // run this listener in registration order against the realm's C++
        // window listeners. The fallback keeps the polyfill standalone —
        // unstamped records simply sort ahead of every C++ one.
        var entry = { fn: fn, capture: false, once: false, passive: false,
                      seq: (typeof globalThis.__bro_listener_seq === 'function')
                             ? globalThis.__bro_listener_seq() : 0 };
        if (typeof opts === 'boolean') {
            entry.capture = opts;
        } else if (opts && typeof opts === 'object') {
            entry.capture = !!opts.capture;
            entry.once = !!opts.once;
            entry.passive = !!opts.passive;
        }
        listeners[type].push(entry);
    };
    globalThis.removeEventListener = function(type, fn, opts) {
        var arr = listeners[type];
        if (!arr) return;
        var capture = false;
        if (typeof opts === 'boolean') capture = opts;
        else if (opts && typeof opts === 'object') capture = !!opts.capture;
        for (var i = 0; i < arr.length; i++) {
            if (arr[i].fn === fn && arr[i].capture === capture) {
                arr.splice(i, 1);
                return;
            }
        }
    };
    // __bro_dispatch_window_event is installed from C++
    // (js::installWindowEventDispatch, called right after this file is
    // evaluated). It reads the `listeners` map above for the JS listeners and
    // the realm Document's windowListeners() for the C++ ones, and runs both
    // in registration order. Keeping one dispatcher — rather than a JS loop
    // here and a C++ loop elsewhere — is what makes a window listener behave
    // the same whoever registered it and whoever fired the event.
    //
    // The fallback below only runs if the C++ half was not installed (the
    // polyfill evaluated standalone); it is the loop this file used to have.
    if (typeof globalThis.__bro_dispatch_window_event !== 'function') {
        globalThis.__bro_dispatch_window_event = function(type, event, capture) {
            var arr = listeners[type];
            if (!arr) return;
            var filterByCapture = (capture !== undefined);
            var isCap = !!capture;
            var snapshot = arr.slice();
            for (var i = 0; i < snapshot.length; i++) {
                var entry = snapshot[i];
                if (filterByCapture && entry.capture !== isCap) continue;
                if (arr.indexOf(entry) === -1) continue;
                try {
                    entry.fn(event);
                    if (entry.once) {
                        var idx = arr.indexOf(entry);
                        if (idx !== -1) arr.splice(idx, 1);
                    }
                    if (event && event._immediateStopped) break;
                } catch(e) { console.error('Event handler error:', e); }
            }
        };
    }
    globalThis.dispatchEvent = function(event) {
        var type = event.type || (typeof event === 'string' ? event : '');
        // One dispatcher, so a JS-dispatched window event reaches C++
        // listeners exactly as a host-dispatched one does. `capture` is left
        // undefined: a window event fired directly at the window has no
        // propagation path, so both capture and bubble listeners run.
        globalThis.__bro_dispatch_window_event(type, event);
        return !(event && event.defaultPrevented);
    };

    // --- SPA history + location compat ---
    var _stack = [{ state: null, url: location.href }];
    var _index = 0;

    function _parseUrl(url) {
        var resolved = url;
        if (url === undefined || url === null) {
            resolved = location.href;
        } else if (url.indexOf('://') === -1) {
            if (url.charAt(0) === '/') {
                resolved = location.origin + url;
            } else if (url.charAt(0) === '?' || url.charAt(0) === '#') {
                resolved = location.origin + location.pathname + url;
            } else {
                var path = location.pathname;
                var lastSlash = path.lastIndexOf('/');
                resolved = location.origin + path.substring(0, lastSlash + 1) + url;
            }
        }
        var obj = { href: resolved, origin: location.origin, protocol: location.protocol,
                    host: location.host, hostname: location.hostname, port: location.port,
                    pathname: '/', search: '', hash: '' };
        var rest = resolved;
        var originIdx = resolved.indexOf(obj.origin);
        if (originIdx >= 0) rest = resolved.substring(originIdx + obj.origin.length);
        var hashIdx = rest.indexOf('#');
        if (hashIdx >= 0) { obj.hash = rest.substring(hashIdx); rest = rest.substring(0, hashIdx); }
        var searchIdx = rest.indexOf('?');
        if (searchIdx >= 0) { obj.search = rest.substring(searchIdx); rest = rest.substring(0, searchIdx); }
        obj.pathname = rest || '/';
        obj.href = obj.origin + obj.pathname + obj.search + obj.hash;
        return obj;
    }

    function _applyUrl(parts) {
        location.href = parts.href;
        location.pathname = parts.pathname;
        location.search = parts.search;
        location.hash = parts.hash;
    }

    function _firePopstate(state) {
        var evt = (typeof PopStateEvent !== 'undefined')
            ? new PopStateEvent('popstate', { state: state })
            : { type: 'popstate', state: state };
        evt.isTrusted = true;
        globalThis.__bro_dispatch_window_event('popstate', evt);
    }

    history.pushState = function(state, title, url) {
        var parts = _parseUrl(url);
        var oldHash = location.hash;
        _stack.splice(_index + 1);
        _stack.push({ state: state, url: parts.href });
        _index = _stack.length - 1;
        history.state = state;
        history.length = _stack.length;
        _applyUrl(parts);
        if (parts.hash !== oldHash) {
            var hEvt = (typeof HashChangeEvent !== 'undefined')
                ? new HashChangeEvent('hashchange', {
                    oldURL: location.origin + location.pathname + oldHash,
                    newURL: parts.href
                  })
                : { type: 'hashchange', oldURL: location.origin + location.pathname + oldHash, newURL: parts.href };
            hEvt.isTrusted = true;
            globalThis.__bro_dispatch_window_event('hashchange', hEvt);
        }
    };

    history.replaceState = function(state, title, url) {
        var parts = _parseUrl(url);
        _stack[_index] = { state: state, url: parts.href };
        history.state = state;
        _applyUrl(parts);
    };

    history.go = function(delta) {
        delta = delta || 0;
        var target = _index + delta;
        if (target < 0 || target >= _stack.length) return;
        _index = target;
        var entry = _stack[_index];
        history.state = entry.state;
        _applyUrl(_parseUrl(entry.url));
        _firePopstate(entry.state);
    };

    history.back = function() { history.go(-1); };
    history.forward = function() { history.go(1); };

    // location methods. There is no real navigation, so replace/assign are
    // no-ops — but reload() is real: the engine installs a native hook in
    // realms that can reload (the app document reloads the whole app; an
    // iframe sub-document reloads that iframe). The reload is DEFERRED to a
    // safe point in the engine loop — this call returns and the current
    // script keeps running; the realm is torn down and the app re-run fresh
    // afterwards. Realms without the hook (system panels) keep the no-op.
    location.replace = function() {};
    location.reload = function() {
        if (typeof globalThis.__bro_location_reload === 'function')
            globalThis.__bro_location_reload();
    };
    location.assign = function() {};
    location.toString = function() { return location.href; };

    // ------------------------------------------------------------
    // window scrolling
    // ------------------------------------------------------------
    // The bro engine maintains a single scrollable root element; window
    // scroll maps to setting scrollTop/scrollLeft on document.documentElement
    // (or body if the document element isn't scrollable). The `behavior`
    // option is accepted but always treated as 'auto' — smooth scrolling
    // animation would need engine-side interpolation.
    function _scrollArgs(a, b) {
        if (a !== null && typeof a === 'object' && !Array.isArray(a)) {
            return { x: a.left, y: a.top };
        }
        return { x: a, y: b };
    }
    function _scrollTarget() {
        return document.documentElement || document.body;
    }
    globalThis.scrollTo = function(a, b) {
        var p = _scrollArgs(a, b);
        var t = _scrollTarget();
        if (!t) return;
        if (typeof p.x === 'number') t.scrollLeft = p.x;
        if (typeof p.y === 'number') t.scrollTop = p.y;
        globalThis.scrollX = t.scrollLeft;
        globalThis.scrollY = t.scrollTop;
        globalThis.pageXOffset = t.scrollLeft;
        globalThis.pageYOffset = t.scrollTop;
    };
    globalThis.scrollBy = function(a, b) {
        var p = _scrollArgs(a, b);
        var t = _scrollTarget();
        if (!t) return;
        if (typeof p.x === 'number') t.scrollLeft = t.scrollLeft + p.x;
        if (typeof p.y === 'number') t.scrollTop  = t.scrollTop  + p.y;
        globalThis.scrollX = t.scrollLeft;
        globalThis.scrollY = t.scrollTop;
        globalThis.pageXOffset = t.scrollLeft;
        globalThis.pageYOffset = t.scrollTop;
    };
    globalThis.scroll = globalThis.scrollTo;

    // Element.prototype.scrollTo / scrollBy and requestFullscreen are added
    // in dom_polyfills.js (which runs after the Element class is registered).
    // Expose the _scrollArgs helper so it can share arg parsing.
    globalThis.__bro_scroll_args = _scrollArgs;

    // Page visibility + fullscreen setup moved to dom_polyfills.js, since
    // `document` isn't bound to a real Document yet at this point in init.

    // document.exitFullscreen / Element.prototype.requestFullscreen — bro
    // controls fullscreen through bro.settings; the Element side is added
    // in dom_polyfills.js after the class is registered. Expose the forwarder.
    globalThis.__bro_set_fullscreen_setting = function(value) {
        if (typeof bro !== 'undefined' && bro.settings && typeof bro.settings.set === 'function') {
            try { bro.settings.set('graphics.fullscreen', value); return Promise.resolve(); }
            catch(e) { return Promise.reject(e); }
        }
        return Promise.reject(new Error('fullscreen not supported'));
    };
    if (typeof document.exitFullscreen === 'undefined') {
        document.exitFullscreen = function() { return globalThis.__bro_set_fullscreen_setting(false); };
    }
})();
