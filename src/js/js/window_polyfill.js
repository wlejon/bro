(function() {
    var listeners = {};
    globalThis.__bro_win_listeners = listeners;
    globalThis.addEventListener = function(type, fn, opts) {
        if (!listeners[type]) listeners[type] = [];
        var entry = { fn: fn, capture: false, once: false, passive: false };
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
    globalThis.dispatchEvent = function(event) {
        var type = event.type || (typeof event === 'string' ? event : '');
        var arr = listeners[type];
        if (!arr) return true;
        var toRemove = [];
        for (var i = 0; i < arr.length; i++) {
            try {
                arr[i].fn(event);
                if (arr[i].once) toRemove.push(i);
            } catch(e) { console.error('Event handler error:', e); }
        }
        for (var j = toRemove.length - 1; j >= 0; j--) arr.splice(toRemove[j], 1);
        return !(event && event.defaultPrevented);
    };
    globalThis.__bro_dispatch_window_event = function(type, event) {
        var arr = listeners[type];
        if (!arr) return;
        var toRemove = [];
        for (var i = 0; i < arr.length; i++) {
            try {
                arr[i].fn(event);
                if (arr[i].once) toRemove.push(i);
            } catch(e) { console.error('Event handler error:', e); }
        }
        for (var j = toRemove.length - 1; j >= 0; j--) arr.splice(toRemove[j], 1);
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

    // location methods — no-ops (no real navigation)
    location.replace = function() {};
    location.reload = function() {};
    location.assign = function() {};
    location.toString = function() { return location.href; };
})();
