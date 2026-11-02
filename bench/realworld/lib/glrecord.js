// WebGL call recorder: a Proxy over the context (and over each extension
// object it hands out) that logs every method call as one line,
//   <frame>\t<name>(<args>) = <return>
// with handles named by type and first-seen order (WebGLBuffer#3), small
// typed arrays printed, large ones and long strings hashed (the strings,
// shader sources mostly, are listed in full at the end). Property reads
// (constants, drawingBufferWidth) pass through unlogged.

function hashBytes(view) {
    const u8 = new Uint8Array(view.buffer, view.byteOffset, view.byteLength);
    let h = 0x811c9dc5 | 0;
    for (let i = 0; i < u8.length; i++) h = Math.imul(h ^ u8[i], 16777619);
    return (h >>> 0).toString(16).padStart(8, '0');
}
function hashString(s) {
    let h = 0x811c9dc5 | 0;
    for (let i = 0; i < s.length; i++) {
        const c = s.charCodeAt(i);
        h = Math.imul(h ^ (c & 0xff), 16777619);
        h = Math.imul(h ^ (c >>> 8), 16777619);
    }
    return (h >>> 0).toString(16).padStart(8, '0');
}

function ctorName(v) {
    try { return (v && v.constructor && v.constructor.name) || 'Object'; } catch (e) { return 'Object'; }
}

export function makeRecorder() {
    const rec = { frame: -1, enabled: true, out: [], strings: new Map(), ids: new Map(), counts: Object.create(null), exts: new Map() };

    rec.handle = (o) => {
        let id = rec.ids.get(o);
        if (id === undefined) {
            const t = ctorName(o);
            rec.counts[t] = (rec.counts[t] || 0) + 1;
            id = t + '#' + rec.counts[t];
            rec.ids.set(o, id);
        }
        return id;
    };

    rec.nameHandle = (o, t) => {
        rec.counts[t] = (rec.counts[t] || 0) + 1;
        rec.ids.set(o, t + '#' + rec.counts[t]);
    };

    const fmt = (v, depth) => {
        if (v === null) return 'null';
        const t = typeof v;
        if (t === 'undefined') return 'undefined';
        if (t === 'number') return Object.is(v, -0) ? '-0' : String(v);
        if (t === 'boolean' || t === 'bigint') return String(v);
        if (t === 'string') {
            if (v.length <= 48) return JSON.stringify(v);
            const h = hashString(v);
            rec.strings.set(h, v);
            return `str[${v.length}]#${h}`;
        }
        if (t === 'function') return 'fn';
        if (ArrayBuffer.isView(v)) {
            const n = ctorName(v);
            if (v.length !== undefined && v.length <= 16) return n + '[' + Array.prototype.join.call(v, ',') + ']';
            return `${n}[${v.length !== undefined ? v.length : v.byteLength}]#${hashBytes(v)}`;
        }
        const known = rec.ids.get(v);
        if (known !== undefined) return known;
        if (v instanceof ArrayBuffer) return `ArrayBuffer[${v.byteLength}]#${hashBytes(new Uint8Array(v))}`;
        if (Array.isArray(v)) return depth > 2 ? '[...]' : '[' + v.map((x) => fmt(x, depth + 1)).join(',') + ']';
        const n = ctorName(v);
        if (rec.exts.has(v)) return 'ext:' + rec.exts.get(v);
        if (n.startsWith('WebGL') && n !== 'WebGLActiveInfo' && n !== 'WebGLShaderPrecisionFormat' && n !== 'WebGL2RenderingContext') return rec.handle(v);
        if ('rangeMin' in v && 'precision' in v) return `{rangeMin:${v.rangeMin},rangeMax:${v.rangeMax},precision:${v.precision}}`;
        if ('name' in v && 'size' in v && 'type' in v && Object.keys(v).length <= 3) return `{name:${JSON.stringify(v.name)},size:${v.size},type:${v.type}}`;
        if ('width' in v && 'height' in v && (n.indexOf('Canvas') >= 0 || n.indexOf('Image') >= 0 || n.indexOf('Video') >= 0 || n.indexOf('Bitmap') >= 0)) return `${n}(${v.width}x${v.height})`;
        if (depth > 2) return n;
        const keys = Object.keys(v).sort();
        if (keys.length === 0) return rec.handle(v);
        return '{' + keys.map((k) => k + ':' + fmt(v[k], depth + 1)).join(',') + '}';
    };
    rec.fmt = fmt;

    rec.log = (name, args, ret) => {
        if (!rec.enabled) return;
        let s = rec.frame + '\t' + name + '(' + Array.prototype.map.call(args, (a) => fmt(a, 0)).join(', ') + ')';
        if (ret !== undefined) s += ' = ' + fmt(ret, 0);
        rec.out.push(s);
    };

    rec.wrap = (target, prefix) => {
        prefix = prefix || 'gl.';
        const cache = new Map();
        return new Proxy(target, {
            get(t, k) {
                const v = t[k];
                if (typeof v !== 'function' || k === 'constructor') return v;
                let w = cache.get(k);
                if (w !== undefined) return w;
                const name = prefix + String(k);
                w = function (...args) {
                    let r = v.apply(t, args);
                    if (k === 'getExtension' && r) r = rec.wrapExt(String(args[0]), r);
                    // Handles are named by the call that made them, the same
                    // way in both browsers.
                    else if (r && typeof r === 'object' && !rec.ids.has(r)) {
                        const ks = String(k);
                        if (ks.startsWith('create') || ks === 'fenceSync') rec.nameHandle(r, ks === 'fenceSync' ? 'Sync' : ks.slice(6));
                        else if (ks === 'getUniformLocation') rec.nameHandle(r, 'Loc');
                    }
                    rec.log(name, args, r);
                    return r;
                };
                cache.set(k, w);
                return w;
            },
            set(t, k, v) { t[k] = v; return true; },
        });
    };

    const extProxies = new Map();
    rec.wrapExt = (name, ext) => {
        let p = extProxies.get(name);
        if (p === undefined) {
            p = rec.wrap(ext, name + '.');
            extProxies.set(name, p);
            rec.exts.set(p, name);
        }
        return p;
    };

    rec.text = () => {
        const parts = [rec.out.join('\n')];
        for (const [h, s] of rec.strings) parts.push('#### str ' + h + '\n' + s);
        return parts.join('\n') + '\n';
    };
    return rec;
}
