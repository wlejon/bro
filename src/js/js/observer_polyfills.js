(function() {
    // ---------------------------------------------------------------
    // ResizeObserver — polls element sizes after layout
    // ---------------------------------------------------------------
    globalThis.ResizeObserver = class ResizeObserver {
        constructor(callback) {
            this._callback = callback;
            this._targets = [];
            this._sizes = new Map();
            if (!globalThis.__bro_resize_observers)
                globalThis.__bro_resize_observers = [];
            globalThis.__bro_resize_observers.push(this);
        }
        observe(target, options) {
            if (!this._targets.includes(target))
                this._targets.push(target);
        }
        unobserve(target) {
            var idx = this._targets.indexOf(target);
            if (idx >= 0) this._targets.splice(idx, 1);
            this._sizes.delete(target);
        }
        disconnect() {
            this._targets = [];
            this._sizes.clear();
            if (globalThis.__bro_resize_observers) {
                var idx = globalThis.__bro_resize_observers.indexOf(this);
                if (idx >= 0) globalThis.__bro_resize_observers.splice(idx, 1);
            }
        }
        // Called by engine after layout to check for size changes
        _check() {
            var entries = [];
            for (var i = 0; i < this._targets.length; i++) {
                var target = this._targets[i];
                var rect = target.getBoundingClientRect();
                var prev = this._sizes.get(target);
                var w = rect.width, h = rect.height;
                if (!prev || prev.w !== w || prev.h !== h) {
                    this._sizes.set(target, { w: w, h: h });
                    entries.push({
                        target: target,
                        contentRect: rect,
                        borderBoxSize: [{ inlineSize: w, blockSize: h }],
                        contentBoxSize: [{ inlineSize: w, blockSize: h }],
                        devicePixelContentBoxSize: [{ inlineSize: w, blockSize: h }]
                    });
                }
            }
            if (entries.length > 0)
                this._callback(entries, this);
        }
    };

    // ---------------------------------------------------------------
    // IntersectionObserver — checks element visibility vs viewport
    // ---------------------------------------------------------------
    globalThis.IntersectionObserver = class IntersectionObserver {
        constructor(callback, options) {
            this._callback = callback;
            this._root = (options && options.root) || null;
            this._rootMargin = (options && options.rootMargin) || '0px';
            this._thresholds = (options && options.threshold) || [0];
            if (typeof this._thresholds === 'number')
                this._thresholds = [this._thresholds];
            this._targets = [];
            this._prevRatios = new Map();
            if (!globalThis.__bro_intersection_observers)
                globalThis.__bro_intersection_observers = [];
            globalThis.__bro_intersection_observers.push(this);
        }
        get root() { return this._root; }
        get rootMargin() { return this._rootMargin; }
        get thresholds() { return this._thresholds; }
        observe(target) {
            if (!this._targets.includes(target))
                this._targets.push(target);
        }
        unobserve(target) {
            var idx = this._targets.indexOf(target);
            if (idx >= 0) this._targets.splice(idx, 1);
            this._prevRatios.delete(target);
        }
        disconnect() {
            this._targets = [];
            this._prevRatios.clear();
            if (globalThis.__bro_intersection_observers) {
                var idx = globalThis.__bro_intersection_observers.indexOf(this);
                if (idx >= 0) globalThis.__bro_intersection_observers.splice(idx, 1);
            }
        }
        takeRecords() { return []; }
        // Called by engine after layout
        _check() {
            var vpW = globalThis.innerWidth || 800;
            var vpH = globalThis.innerHeight || 600;
            var rootRect = this._root
                ? this._root.getBoundingClientRect()
                : { x: 0, y: 0, width: vpW, height: vpH, top: 0, left: 0, right: vpW, bottom: vpH };
            var entries = [];
            for (var i = 0; i < this._targets.length; i++) {
                var target = this._targets[i];
                var rect = target.getBoundingClientRect();
                // Calculate intersection
                var intLeft = Math.max(rect.x, rootRect.x);
                var intTop = Math.max(rect.y, rootRect.y);
                var intRight = Math.min(rect.x + rect.width, rootRect.x + rootRect.width);
                var intBottom = Math.min(rect.y + rect.height, rootRect.y + rootRect.height);
                var intW = Math.max(0, intRight - intLeft);
                var intH = Math.max(0, intBottom - intTop);
                var intArea = intW * intH;
                var targetArea = rect.width * rect.height;
                var ratio = targetArea > 0 ? intArea / targetArea : 0;
                var isIntersecting = ratio > 0;
                // Check if we crossed a threshold
                var prevRatio = this._prevRatios.get(target);
                if (prevRatio === undefined) prevRatio = -1;
                var crossed = false;
                for (var t = 0; t < this._thresholds.length; t++) {
                    var th = this._thresholds[t];
                    if ((prevRatio < th && ratio >= th) || (prevRatio >= th && ratio < th)) {
                        crossed = true;
                        break;
                    }
                }
                if (prevRatio === -1) crossed = true; // initial observation
                if (crossed) {
                    this._prevRatios.set(target, ratio);
                    entries.push({
                        target: target,
                        boundingClientRect: rect,
                        intersectionRatio: ratio,
                        intersectionRect: { x: intLeft, y: intTop, width: intW, height: intH,
                            top: intTop, left: intLeft, right: intRight, bottom: intBottom },
                        isIntersecting: isIntersecting,
                        rootBounds: rootRect,
                        time: performance.now()
                    });
                }
            }
            if (entries.length > 0)
                this._callback(entries, this);
        }
    };

})();
