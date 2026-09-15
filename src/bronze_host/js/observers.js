// MutationObserver, ResizeObserver and IntersectionObserver, as JavaScript.
//
// The classes ARE this file. C++ supplies only what JavaScript cannot see for
// itself, through one host object, `__bro_observers`
// (src/bronze_host/host_observer_hooks.cpp):
//
//   observeMutations(node, subtree)     the DOM records notices for this node
//   unobserveMutations(node, subtree)   ...and stops when the last interest goes
//   takeMutations()                     the raw notices since the last take,
//                                       each naming the watched nodes it hit
//   onMutation(fn)                      fn() runs when the first notice lands
//                                       after a take — this file queues the
//                                       delivery microtask from it
//   onFrame(fn)                         fn() runs once per frame from the
//                                       host's frame seam, after rAF and its
//                                       microtask checkpoint — where the
//                                       resize and intersection passes belong
//
// Everything else — which observer wants which record, the old-value rule,
// the attribute filter, the record batching, the microtask timing, the sizes,
// the intersection geometry, the threshold crossings — is here, in the
// language the web specifies it in.
//
// DELIVERY TIMING is the part worth stating. Mutation records are delivered
// from a MICROTASK queued at the first notice after each take, so a run of
// mutations in one script turn is one callback with every record, after the
// turn — never inside it — and a callback that mutates what it observes is
// heard about in a following microtask of the same checkpoint rather than
// re-entering itself. Resize and intersection are delivered from the frame
// seam after layout: reading getBoundingClientRect lays the document out
// first (Engine::flushLayoutForRead), so a size changed from inside a
// requestAnimationFrame callback is measured by the pass of that same frame.
//
// WHAT THIS FILE MAY NAME. It is compiled against js/module.globals, which
// lists three bare identifiers; every other host global is read off
// `globalThis` at the point of use, so the module compiles before the host
// that will run it exists.
(function () {
    'use strict';

    const g = globalThis;
    const hooks = __bro_observers;

    // A throw out of an observer callback is reported through the host's
    // error funnel rather than silencing the observers still to run: a
    // microtask that throws is caught by the host's queueMicrotask and logged
    // with the error's own name and message.
    function reportUncaught(err) {
        g.queueMicrotask(function () { throw err; });
    }

    function isNode(v) {
        return v !== null && typeof v === 'object' && typeof v.nodeType === 'number';
    }

    function isElement(v) {
        return isNode(v) && v.nodeType === 1;
    }

    // A DOMRectReadOnly-shaped plain object: the eight fields every rect the
    // web hands an observer carries.
    function makeRect(x, y, width, height) {
        return {
            x: x, y: y, width: width, height: height,
            top: y, left: x, right: x + width, bottom: y + height,
        };
    }

    // -----------------------------------------------------------------------
    // MutationObserver
    // -----------------------------------------------------------------------

    class MutationRecord {
        constructor(type, target, addedNodes, removedNodes, previousSibling,
                    nextSibling, attributeName, oldValue) {
            this.type = type;
            this.target = target;
            this.addedNodes = addedNodes;
            this.removedNodes = removedNodes;
            this.previousSibling = previousSibling;
            this.nextSibling = nextSibling;
            this.attributeName = attributeName;
            this.attributeNamespace = null;
            this.oldValue = oldValue;
        }
    }

    // Per-observer state, keyed by the observer so the public object carries
    // nothing but its prototype: callback, the registrations (node -> the
    // options observe() was given for it) and the record queue.
    const moState = new WeakMap();

    // Every observer with at least one registration, in creation order —
    // the spec's "notify list". An observer that disconnects leaves it and
    // becomes collectable.
    const activeMutationObservers = new Set();
    let mutationMicrotaskQueued = false;

    // The observe() options algorithm (DOM §4.3.1), including the two
    // shorthands: asking for an old value or an attribute filter turns the
    // corresponding kind on without naming it.
    function normalizeMutationOptions(options) {
        const o = options === undefined || options === null ? {} : options;
        if (typeof o !== 'object') {
            throw new TypeError('MutationObserver.observe: options must be an object');
        }
        const hasAttributeOldValue = 'attributeOldValue' in o;
        const hasAttributeFilter = 'attributeFilter' in o;
        const hasCharacterDataOldValue = 'characterDataOldValue' in o;
        let attributes = 'attributes' in o ? !!o.attributes : undefined;
        let characterData = 'characterData' in o ? !!o.characterData : undefined;
        if (attributes === undefined && (hasAttributeOldValue || hasAttributeFilter)) {
            attributes = true;
        }
        if (characterData === undefined && hasCharacterDataOldValue) characterData = true;
        const childList = !!o.childList;
        attributes = !!attributes;
        characterData = !!characterData;
        if (!childList && !attributes && !characterData) {
            throw new TypeError(
                'MutationObserver.observe: one of childList, attributes or ' +
                'characterData must be true');
        }
        if (hasAttributeOldValue && !!o.attributeOldValue && !attributes) {
            throw new TypeError(
                'MutationObserver.observe: attributeOldValue requires attributes');
        }
        if (hasAttributeFilter && !attributes) {
            throw new TypeError(
                'MutationObserver.observe: attributeFilter requires attributes');
        }
        if (hasCharacterDataOldValue && !!o.characterDataOldValue && !characterData) {
            throw new TypeError(
                'MutationObserver.observe: characterDataOldValue requires characterData');
        }
        let attributeFilter = null;
        if (hasAttributeFilter) {
            attributeFilter = new Set();
            for (const name of o.attributeFilter) attributeFilter.add(String(name));
        }
        return {
            childList: childList,
            attributes: attributes,
            characterData: characterData,
            subtree: !!o.subtree,
            attributeOldValue: !!o.attributeOldValue,
            characterDataOldValue: !!o.characterDataOldValue,
            attributeFilter: attributeFilter,
        };
    }

    // One raw notice against one observer's registrations: the "queue a
    // mutation record" steps, run for the observer as a whole. `raw.nodes`
    // are the watched nodes the DOM layer matched WHEN THE MUTATION HAPPENED
    // — the target itself and any subtree-watched ancestor it had at that
    // instant, which is the only time the question has a right answer (a
    // removed node has no ancestors afterwards).
    function considerRawRecord(st, raw) {
        let interested = false;
        let wantOldValue = false;
        for (let i = 0; i < raw.nodes.length; i++) {
            const node = raw.nodes[i];
            const o = st.targets.get(node);
            if (o === undefined) continue;
            // An ancestor registration only counts with subtree; the target's
            // own registration always does.
            if (node !== raw.target && !o.subtree) continue;
            if (raw.type === 'childList') {
                if (!o.childList) continue;
            } else if (raw.type === 'attributes') {
                if (!o.attributes) continue;
                if (o.attributeFilter !== null && !o.attributeFilter.has(raw.attributeName)) {
                    continue;
                }
                if (o.attributeOldValue) wantOldValue = true;
            } else {
                if (!o.characterData) continue;
                if (o.characterDataOldValue) wantOldValue = true;
            }
            interested = true;
        }
        if (!interested) return;
        st.queue.push(new MutationRecord(
            raw.type, raw.target,
            raw.added !== null ? [raw.added] : [],
            raw.removed !== null ? [raw.removed] : [],
            raw.previousSibling, raw.nextSibling,
            raw.attributeName,
            wantOldValue ? raw.oldValue : null));
    }

    // Move everything the DOM has recorded since the last take into the
    // observers' own queues. Called before every operation that reads or
    // changes registrations, so a record is always assigned against the
    // registrations that stood when its mutation happened — disconnect()
    // drops exactly what the web's disconnect drops, and takeRecords()
    // answers synchronously.
    function pumpMutations() {
        const batch = hooks.takeMutations();
        for (let i = 0; i < batch.length; i++) {
            const raw = batch[i];
            for (const observer of activeMutationObservers) {
                considerRawRecord(moState.get(observer), raw);
            }
        }
    }

    function notifyMutationObservers() {
        mutationMicrotaskQueued = false;
        pumpMutations();
        // Snapshot first: a callback may construct or disconnect observers,
        // and the set must not be walked while it changes under the walk.
        const notifyList = Array.from(activeMutationObservers);
        for (let i = 0; i < notifyList.length; i++) {
            const observer = notifyList[i];
            const st = moState.get(observer);
            const records = st.queue;
            if (records.length === 0) continue;
            st.queue = [];
            try {
                st.callback.call(observer, records, observer);
            } catch (err) {
                reportUncaught(err);
            }
        }
    }

    // The host calls this at the first notice after each take; the microtask
    // is the web's "queue a mutation observer microtask", deduplicated by the
    // flag exactly as the spec's "mutation observer microtask queued" is.
    function scheduleMutationDelivery() {
        if (mutationMicrotaskQueued) return;
        mutationMicrotaskQueued = true;
        g.queueMicrotask(notifyMutationObservers);
    }

    class MutationObserver {
        constructor(callback) {
            if (typeof callback !== 'function') {
                throw new TypeError('MutationObserver: the argument must be a function');
            }
            moState.set(this, { callback: callback, targets: new Map(), queue: [] });
        }

        observe(target, options) {
            const st = moState.get(this);
            if (st === undefined) {
                throw new TypeError('MutationObserver.observe: receiver is not an observer');
            }
            if (!isNode(target)) {
                throw new TypeError('MutationObserver.observe: target must be a node');
            }
            const opts = normalizeMutationOptions(options);
            pumpMutations();
            const previous = st.targets.get(target);
            if (previous === undefined) {
                hooks.observeMutations(target, opts.subtree);
            } else if (previous.subtree !== opts.subtree) {
                hooks.unobserveMutations(target, previous.subtree);
                hooks.observeMutations(target, opts.subtree);
            }
            // Re-observing a node REPLACES its options rather than stacking a
            // second registration that would double every record.
            st.targets.set(target, opts);
            activeMutationObservers.add(this);
        }

        // bro's extension: drop ONE target and keep the rest. Not on the
        // web (there it is disconnect or nothing), but it was on bro's
        // MutationObserver before this module and code was written to it.
        // Dropping the last target is a disconnect, queue included.
        unobserve(target) {
            const st = moState.get(this);
            if (st === undefined) {
                throw new TypeError('MutationObserver.unobserve: receiver is not an observer');
            }
            const opts = st.targets.get(target);
            if (opts === undefined) return;
            pumpMutations();
            hooks.unobserveMutations(target, opts.subtree);
            st.targets.delete(target);
            if (st.targets.size === 0) this.disconnect();
        }

        disconnect() {
            const st = moState.get(this);
            if (st === undefined) return;
            pumpMutations();
            for (const [node, opts] of st.targets) {
                hooks.unobserveMutations(node, opts.subtree);
            }
            st.targets.clear();
            // The web drops the record queue too: a disconnected observer's
            // pending records are gone, not merely undelivered.
            st.queue = [];
            activeMutationObservers.delete(this);
        }

        takeRecords() {
            const st = moState.get(this);
            if (st === undefined) return [];
            pumpMutations();
            const records = st.queue;
            st.queue = [];
            return records;
        }
    }

    hooks.onMutation(scheduleMutationDelivery);

    // -----------------------------------------------------------------------
    // ResizeObserver
    // -----------------------------------------------------------------------
    //
    // A poll, not a notification: a box changes size for reasons that never
    // touch the DOM — a window resize, a font arriving, a sibling growing —
    // so there is no mutation to hang a notice on. The pass runs from the
    // frame seam and follows the spec's loop: gather the observations whose
    // size changed, deliver them, then gather again for targets DEEPER than
    // the shallowest one just delivered, so a callback that resizes its own
    // children is heard about in the same frame while one that resizes an
    // ancestor is not (that would loop forever) and is reported instead.

    class ResizeObserverSize {
        constructor(inlineSize, blockSize) {
            this.inlineSize = inlineSize;
            this.blockSize = blockSize;
        }
    }

    class ResizeObserverEntry {
        constructor(target, contentRect, borderBoxSize, contentBoxSize,
                    devicePixelContentBoxSize) {
            this.target = target;
            this.contentRect = contentRect;
            this.borderBoxSize = borderBoxSize;
            this.contentBoxSize = contentBoxSize;
            this.devicePixelContentBoxSize = devicePixelContentBoxSize;
        }
    }

    const roState = new WeakMap();
    const activeResizeObservers = new Set();

    const BOX_OPTIONS = ['content-box', 'border-box', 'device-pixel-content-box'];

    function px(v) {
        const n = parseFloat(v);
        return n === n ? n : 0;   // NaN -> 0: an unset or non-length value
    }

    // Every size an entry reports, measured once per target per pass. The
    // border box is what getBoundingClientRect answers; the content box is
    // what clientWidth/clientHeight answer in this host (the layout box's
    // content rect, which is also what contentRect reports); the content
    // rect's origin is the padding offset inside the border box, as the
    // spec has it.
    function measureBoxes(target) {
        const border = target.getBoundingClientRect();
        const contentW = target.clientWidth;
        const contentH = target.clientHeight;
        const cs = g.getComputedStyle(target);
        const dpr = g.devicePixelRatio || 1;
        return {
            borderW: border.width, borderH: border.height,
            contentW: contentW, contentH: contentH,
            padLeft: px(cs.paddingLeft), padTop: px(cs.paddingTop),
            deviceW: Math.round(contentW * dpr), deviceH: Math.round(contentH * dpr),
        };
    }

    function sizeForBox(m, box) {
        if (box === 'border-box') return [m.borderW, m.borderH];
        if (box === 'device-pixel-content-box') return [m.deviceW, m.deviceH];
        return [m.contentW, m.contentH];
    }

    // The spec's "calculate depth": parentNode hops to the root of the tree.
    function nodeDepth(node) {
        let depth = 0;
        for (let p = node.parentNode; p !== null && p !== undefined; p = p.parentNode) depth++;
        return depth;
    }

    function makeResizeEntry(target, m) {
        return new ResizeObserverEntry(
            target,
            makeRect(m.padLeft, m.padTop, m.contentW, m.contentH),
            [new ResizeObserverSize(m.borderW, m.borderH)],
            [new ResizeObserverSize(m.contentW, m.contentH)],
            [new ResizeObserverSize(m.deviceW, m.deviceH)]);
    }

    // "Gather active observations at depth": for each observer, the targets
    // whose observed box differs from the last size reported for it and that
    // sit deeper than `depth`. The rest of the changed ones are SKIPPED, and
    // a skipped observation at the end of the loop is the error the web
    // reports.
    function gatherActiveResize(depth) {
        const batches = [];
        let skipped = false;
        for (const observer of activeResizeObservers) {
            const st = roState.get(observer);
            let entries = null;
            let shallowest = Infinity;
            for (const [target, obs] of st.targets) {
                const m = measureBoxes(target);
                const size = sizeForBox(m, obs.box);
                if (size[0] === obs.lastW && size[1] === obs.lastH) continue;
                const targetDepth = nodeDepth(target);
                if (targetDepth <= depth) { skipped = true; continue; }
                obs.lastW = size[0];
                obs.lastH = size[1];
                if (entries === null) entries = [];
                entries.push(makeResizeEntry(target, m));
                if (targetDepth < shallowest) shallowest = targetDepth;
            }
            if (entries !== null) {
                batches.push({ observer: observer, entries: entries, depth: shallowest });
            }
        }
        return { batches: batches, skipped: skipped };
    }

    function deliverResizeObservations() {
        if (activeResizeObservers.size === 0) return;
        let depth = 0;
        let gathered = gatherActiveResize(depth);
        while (gathered.batches.length > 0) {
            let shallowest = Infinity;
            for (let i = 0; i < gathered.batches.length; i++) {
                const b = gathered.batches[i];
                if (b.depth < shallowest) shallowest = b.depth;
                try {
                    roState.get(b.observer).callback.call(b.observer, b.entries, b.observer);
                } catch (err) {
                    reportUncaught(err);
                }
            }
            depth = shallowest;
            gathered = gatherActiveResize(depth);
        }
        if (gathered.skipped) {
            reportUncaught(new Error(
                'ResizeObserver loop completed with undelivered notifications.'));
        }
    }

    class ResizeObserver {
        constructor(callback) {
            if (typeof callback !== 'function') {
                throw new TypeError('ResizeObserver: the argument must be a function');
            }
            roState.set(this, { callback: callback, targets: new Map() });
        }

        observe(target, options) {
            const st = roState.get(this);
            if (st === undefined) {
                throw new TypeError('ResizeObserver.observe: receiver is not an observer');
            }
            if (!isElement(target)) {
                throw new TypeError('ResizeObserver.observe: target must be an element');
            }
            const o = options === undefined || options === null ? {} : options;
            const box = o.box === undefined ? 'content-box' : String(o.box);
            if (BOX_OPTIONS.indexOf(box) < 0) {
                throw new TypeError('ResizeObserver.observe: box must be one of ' +
                                    BOX_OPTIONS.join(', '));
            }
            // Observing again with the same box is a no-op; with another box
            // it is an unobserve and a fresh observation, as the spec says.
            const previous = st.targets.get(target);
            if (previous !== undefined && previous.box === box) return;
            // The last reported size starts at 0x0 (ResizeObservation's
            // initial lastReportedSizes), which is what makes the FIRST pass
            // report a rendered element's current size unprompted — and NOT
            // report an element that is not rendered at all.
            st.targets.set(target, { box: box, lastW: 0, lastH: 0 });
            activeResizeObservers.add(this);
        }

        unobserve(target) {
            const st = roState.get(this);
            if (st === undefined) return;
            st.targets.delete(target);
            if (st.targets.size === 0) activeResizeObservers.delete(this);
        }

        disconnect() {
            const st = roState.get(this);
            if (st === undefined) return;
            st.targets.clear();
            activeResizeObservers.delete(this);
        }
    }

    // -----------------------------------------------------------------------
    // IntersectionObserver
    // -----------------------------------------------------------------------
    //
    // Geometry, once per frame: the target's border box against the root's
    // (the viewport when root is null), the root box grown by rootMargin, the
    // clipped intersection, its ratio of the target's area, and the index of
    // the threshold that ratio has reached. An entry is queued when that
    // index or the intersecting bit differs from the last one queued for the
    // target — with the previous index starting at -1, so the first pass
    // always reports, as the web does.

    class IntersectionObserverEntry {
        constructor(time, rootBounds, boundingClientRect, intersectionRect,
                    isIntersecting, intersectionRatio, target) {
            this.time = time;
            this.rootBounds = rootBounds;
            this.boundingClientRect = boundingClientRect;
            this.intersectionRect = intersectionRect;
            this.isIntersecting = isIntersecting;
            this.intersectionRatio = intersectionRatio;
            this.target = target;
        }
    }

    const ioState = new WeakMap();
    const activeIntersectionObservers = new Set();

    // "0px", "10px 5%", up to four values in top/right/bottom/left order,
    // each a px length or a percentage of the root's corresponding dimension.
    function parseRootMargin(text) {
        const parts = String(text).trim().split(/\s+/).filter(function (p) { return p !== ''; });
        if (parts.length === 0) parts.push('0px');
        if (parts.length > 4) {
            throw new SyntaxError('IntersectionObserver: rootMargin must have at most four values');
        }
        const margins = [];
        for (let i = 0; i < parts.length; i++) {
            const m = /^(-?(?:\d+\.?\d*|\.\d+))(px|%)$/.exec(parts[i]);
            if (m === null) {
                throw new SyntaxError('IntersectionObserver: rootMargin must be px or % lengths');
            }
            margins.push({ value: parseFloat(m[1]), unit: m[2] });
        }
        // CSS shorthand expansion: 1 -> all, 2 -> v h, 3 -> t h b.
        while (margins.length < 4) {
            margins.push(margins[margins.length === 1 ? 0 : margins.length === 2 ? 0 : 1]);
        }
        return margins;
    }

    function marginText(margins) {
        return margins.map(function (m) { return m.value + m.unit; }).join(' ');
    }

    function normalizeThresholds(threshold) {
        const list = threshold === undefined ? [0]
                   : Array.isArray(threshold) ? threshold.slice() : [threshold];
        if (list.length === 0) list.push(0);
        for (let i = 0; i < list.length; i++) {
            const t = Number(list[i]);
            if (!(t >= 0 && t <= 1)) {
                throw new RangeError('IntersectionObserver: threshold values must be in [0, 1]');
            }
            list[i] = t;
        }
        list.sort(function (a, b) { return a - b; });
        return list;
    }

    function inRootTree(root, target) {
        if (root !== null) return root !== target && root.contains(target);
        const doc = target.ownerDocument;
        const html = doc ? doc.documentElement : null;
        return !!html && (html === target || html.contains(target));
    }

    function computeIntersection(st, target) {
        const rootRect = st.root === null
            ? makeRect(0, 0, g.innerWidth, g.innerHeight)
            : st.root.getBoundingClientRect();
        const mg = st.margins;
        const mTop = mg[0].unit === '%' ? mg[0].value * rootRect.height / 100 : mg[0].value;
        const mRight = mg[1].unit === '%' ? mg[1].value * rootRect.width / 100 : mg[1].value;
        const mBottom = mg[2].unit === '%' ? mg[2].value * rootRect.height / 100 : mg[2].value;
        const mLeft = mg[3].unit === '%' ? mg[3].value * rootRect.width / 100 : mg[3].value;
        const rootBounds = makeRect(
            rootRect.left - mLeft, rootRect.top - mTop,
            rootRect.width + mLeft + mRight, rootRect.height + mTop + mBottom);

        const targetRect = target.getBoundingClientRect();
        const bounding = makeRect(targetRect.left, targetRect.top,
                                  targetRect.width, targetRect.height);

        const left = Math.max(bounding.left, rootBounds.left);
        const top = Math.max(bounding.top, rootBounds.top);
        const right = Math.min(bounding.right, rootBounds.right);
        const bottom = Math.min(bounding.bottom, rootBounds.bottom);
        // Intersecting, or edge-adjacent: a zero-area overlap still counts,
        // which is what lets a zero-height target be observed at all.
        const isIntersecting = inRootTree(st.root, target) &&
                               left <= right && top <= bottom;
        const intersectionRect = isIntersecting
            ? makeRect(left, top, right - left, bottom - top)
            : makeRect(0, 0, 0, 0);

        const targetArea = bounding.width * bounding.height;
        const intersectionArea = intersectionRect.width * intersectionRect.height;
        let ratio;
        if (targetArea > 0) ratio = intersectionArea / targetArea;
        else ratio = isIntersecting ? 1 : 0;
        if (ratio > 1) ratio = 1;

        let thresholdIndex = st.thresholds.length;
        for (let i = 0; i < st.thresholds.length; i++) {
            if (st.thresholds[i] > ratio) { thresholdIndex = i; break; }
        }
        return {
            rootBounds: rootBounds,
            bounding: bounding,
            intersectionRect: intersectionRect,
            isIntersecting: isIntersecting,
            ratio: ratio,
            thresholdIndex: thresholdIndex,
        };
    }

    function deliverIntersectionObservations() {
        if (activeIntersectionObservers.size === 0) return;
        const now = g.performance.now();
        const notifyList = [];
        for (const observer of activeIntersectionObservers) {
            const st = ioState.get(observer);
            for (const [target, obs] of st.targets) {
                const r = computeIntersection(st, target);
                if (r.thresholdIndex === obs.previousThresholdIndex &&
                    r.isIntersecting === obs.previousIsIntersecting) {
                    continue;
                }
                obs.previousThresholdIndex = r.thresholdIndex;
                obs.previousIsIntersecting = r.isIntersecting;
                st.queue.push(new IntersectionObserverEntry(
                    now, r.rootBounds, r.bounding, r.intersectionRect,
                    r.isIntersecting, r.ratio, target));
            }
            if (st.queue.length > 0) notifyList.push(observer);
        }
        for (let i = 0; i < notifyList.length; i++) {
            const observer = notifyList[i];
            const st = ioState.get(observer);
            const entries = st.queue;
            st.queue = [];
            try {
                st.callback.call(observer, entries, observer);
            } catch (err) {
                reportUncaught(err);
            }
        }
    }

    class IntersectionObserver {
        constructor(callback, options) {
            if (typeof callback !== 'function') {
                throw new TypeError('IntersectionObserver: the argument must be a function');
            }
            const o = options === undefined || options === null ? {} : options;
            let root = null;
            if (o.root !== undefined && o.root !== null) {
                if (!isElement(o.root)) {
                    throw new TypeError('IntersectionObserver: root must be an element or null');
                }
                root = o.root;
            }
            const margins = parseRootMargin(o.rootMargin === undefined ? '0px' : o.rootMargin);
            ioState.set(this, {
                callback: callback,
                root: root,
                margins: margins,
                rootMargin: marginText(margins),
                thresholds: normalizeThresholds(o.threshold),
                targets: new Map(),
                queue: [],
            });
        }

        get root() { return ioState.get(this).root; }
        get rootMargin() { return ioState.get(this).rootMargin; }
        get thresholds() { return ioState.get(this).thresholds.slice(); }

        observe(target) {
            const st = ioState.get(this);
            if (st === undefined) {
                throw new TypeError('IntersectionObserver.observe: receiver is not an observer');
            }
            if (!isElement(target)) {
                throw new TypeError('IntersectionObserver.observe: target must be an element');
            }
            if (st.targets.has(target)) return;
            st.targets.set(target, { previousThresholdIndex: -1, previousIsIntersecting: false });
            activeIntersectionObservers.add(this);
        }

        unobserve(target) {
            const st = ioState.get(this);
            if (st === undefined) return;
            st.targets.delete(target);
            if (st.targets.size === 0 && st.queue.length === 0) {
                activeIntersectionObservers.delete(this);
            }
        }

        disconnect() {
            const st = ioState.get(this);
            if (st === undefined) return;
            st.targets.clear();
            st.queue = [];
            activeIntersectionObservers.delete(this);
        }

        takeRecords() {
            const st = ioState.get(this);
            if (st === undefined) return [];
            const entries = st.queue;
            st.queue = [];
            return entries;
        }
    }

    // -----------------------------------------------------------------------
    // The frame seam
    // -----------------------------------------------------------------------

    // Sizes first, intersections second, the order the web's "update the
    // rendering" runs them in: a resize callback that moves things is seen
    // by the intersection pass of the same frame.
    hooks.onFrame(function () {
        deliverResizeObservations();
        deliverIntersectionObservations();
    });

    g.MutationObserver = MutationObserver;
    g.MutationRecord = MutationRecord;
    g.ResizeObserver = ResizeObserver;
    g.ResizeObserverEntry = ResizeObserverEntry;
    g.ResizeObserverSize = ResizeObserverSize;
    g.IntersectionObserver = IntersectionObserver;
    g.IntersectionObserverEntry = IntersectionObserverEntry;
})();
