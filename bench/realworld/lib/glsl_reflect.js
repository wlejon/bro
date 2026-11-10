// GLSL ES reflection for lib/glstub.js: what a driver answers about a linked
// program (active uniforms, attributes, uniform blocks), read from the shader
// SOURCE the library passed. Runs once per link, never per frame.
//
// The source is preprocessed (#define / #undef / #if / #ifdef / #ifndef /
// #elif / #else / #endif with the full #if expression grammar; object-like
// macros are evaluated in #if and in array sizes, never expanded in the body),
// then the global declarations are read: structs, `uniform` declarations
// (primitive, array, struct and struct-array, uniform blocks with or without
// an instance name) and, in the vertex shader, `in` / `attribute` inputs.
//
// Activity approximates a compiler's dead-code elimination: a uniform or an
// attribute is active when its name occurs inside some function body (any
// function, called or not), and a struct member when its member name does.
// Names follow the GL rules three.js' WebGLUniforms and pixi's
// getUniformData parse: `a`, `a[0]` (size n) for an array, `s.m`, `s[2].m`.

import { GL, GLSL_TYPES } from './glconst.js';

const PRECISION = new Set(['lowp', 'mediump', 'highp']);
const QUALIFIERS = new Set(['flat', 'smooth', 'centroid', 'invariant', 'noperspective', 'lowp', 'mediump', 'highp']);

function stripComments(s) {
    return s.replace(/\/\*[\s\S]*?\*\//g, ' ').replace(/\/\/[^\n]*/g, '');
}

// ---- #if expressions ---------------------------------------------------------

const EXPR_TOKEN = /\s*(0[xX][0-9a-fA-F]+|\d+(?:\.\d*)?(?:[eE][-+]?\d+)?[uUlLfF]*|[A-Za-z_]\w*|&&|\|\||==|!=|<=|>=|<<|>>|[-+*\/%<>!~&|^()?:])/y;

function exprTokens(text) {
    const out = [];
    EXPR_TOKEN.lastIndex = 0;
    let m;
    while (EXPR_TOKEN.lastIndex < text.length && (m = EXPR_TOKEN.exec(text)) !== null) out.push(m[1]);
    return out;
}

const BINARY = {
    '||': 1, '&&': 2, '|': 3, '^': 4, '&': 5, '==': 6, '!=': 6, '<': 7, '>': 7, '<=': 7, '>=': 7,
    '<<': 8, '>>': 8, '+': 9, '-': 9, '*': 10, '/': 10, '%': 10,
};

function applyBinary(op, a, b) {
    switch (op) {
        case '||': return (a || b) ? 1 : 0;
        case '&&': return (a && b) ? 1 : 0;
        case '|': return a | b;
        case '^': return a ^ b;
        case '&': return a & b;
        case '==': return a === b ? 1 : 0;
        case '!=': return a !== b ? 1 : 0;
        case '<': return a < b ? 1 : 0;
        case '>': return a > b ? 1 : 0;
        case '<=': return a <= b ? 1 : 0;
        case '>=': return a >= b ? 1 : 0;
        case '<<': return a << b;
        case '>>': return a >> b;
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b;
        case '/': return b === 0 ? 0 : Math.trunc(a / b);
        case '%': return b === 0 ? 0 : a % b;
    }
    return 0;
}

export function evalExpr(text, macros, depth = 0) {
    const toks = exprTokens(text);
    let p = 0;
    const atom = () => {
        const t = toks[p++];
        if (t === undefined) return 0;
        if (t === '(') { const v = ternary(); if (toks[p] === ')') p++; return v; }
        if (t === '!') return atom() ? 0 : 1;
        if (t === '-') return -atom();
        if (t === '+') return atom();
        if (t === '~') return ~atom();
        if (t === 'defined') {
            let name;
            if (toks[p] === '(') { name = toks[p + 1]; p += 3; } else name = toks[p++];
            return macros.has(name) ? 1 : 0;
        }
        if (/^\d/.test(t)) return /^0[xX]/.test(t) ? parseInt(t, 16) : parseFloat(t);
        const body = macros.get(t);
        if (typeof body === 'string' && depth < 16) return evalExpr(body, macros, depth + 1);
        return 0;
    };
    const binary = (minPrec) => {
        let lhs = atom();
        for (;;) {
            const op = toks[p];
            const prec = BINARY[op];
            if (prec === undefined || prec < minPrec) return lhs;
            p++;
            const rhs = binary(prec + 1);
            lhs = applyBinary(op, lhs, rhs);
        }
    };
    const ternary = () => {
        const c = binary(1);
        if (toks[p] !== '?') return c;
        p++;
        const a = ternary();
        if (toks[p] === ':') p++;
        const b = ternary();
        return c ? a : b;
    };
    return ternary();
}

// ---- preprocessor ------------------------------------------------------------

function preprocess(src) {
    const lines = stripComments(src).replace(/\\\r?\n/g, ' ').split('\n');
    const macros = new Map();
    const stack = [];
    let active = true;
    const out = [];
    for (const line of lines) {
        const m = /^\s*#\s*(\w+)\s*(.*)$/.exec(line);
        if (!m) { if (active) out.push(line); continue; }
        const d = m[1], rest = m[2].trim();
        if (d === 'ifdef' || d === 'ifndef' || d === 'if') {
            let cond;
            if (d === 'if') cond = active && evalExpr(rest, macros) !== 0;
            else {
                const name = (/^\w+/.exec(rest) || [''])[0];
                cond = active && (macros.has(name) === (d === 'ifdef'));
            }
            stack.push({ parent: active, taken: cond });
            active = cond;
        } else if (d === 'elif') {
            const top = stack[stack.length - 1];
            if (!top) continue;
            if (!top.parent || top.taken) active = false;
            else { active = evalExpr(rest, macros) !== 0; if (active) top.taken = true; }
        } else if (d === 'else') {
            const top = stack[stack.length - 1];
            if (!top) continue;
            active = top.parent && !top.taken;
            top.taken = true;
        } else if (d === 'endif') {
            const top = stack.pop();
            active = top ? top.parent : true;
        } else if (d === 'define' && active) {
            const dm = /^([A-Za-z_]\w*)(\()?\s*(.*)$/.exec(rest);
            if (dm) macros.set(dm[1], dm[2] ? null : dm[3]);
        } else if (d === 'undef' && active) {
            macros.delete((/^\w+/.exec(rest) || [''])[0]);
        }
        // #version / #extension / #pragma / #precision / #error / #line: nothing to keep
    }
    return { text: out.join('\n'), macros };
}

// ---- declarations --------------------------------------------------------------

const TOKEN = /[A-Za-z_]\w*|\d+(?:\.\d*)?(?:[eE][-+]?\d+)?[uUfF]?|\.\d+|\S/g;

function parseShader(src, isVertex) {
    const { text, macros } = preprocess(src);
    const toks = text.match(TOKEN) || [];
    const n = toks.length;
    const structs = new Map();
    const uniforms = [];   // { name, type, size, block }
    const blocks = [];     // { name, instance, members: [{ name, type, size }] }
    const attribs = [];    // { name, type, size, location }
    const used = new Set();
    let paren = 0;
    let layoutLocation = -1;

    const arraySize = (k) => {
        // toks[k] === '[': returns [size, index after ']']
        let j = k + 1, expr = '';
        while (j < n && toks[j] !== ']') expr += toks[j++] + ' ';
        return [Math.max(1, evalExpr(expr, macros) | 0), j + 1];
    };
    // `type name [size] (, name [size])* ;` from k; returns [decls, next index]
    const declarators = (k) => {
        while (PRECISION.has(toks[k])) k++;
        const type = toks[k++];
        const out = [];
        while (k < n) {
            const name = toks[k++];
            let size = 0;
            if (toks[k] === '[') [size, k] = arraySize(k);
            out.push({ name, type, size });
            if (toks[k] === ',') { k++; continue; }
            while (k < n && toks[k] !== ';') k++;
            return [out, k + 1];
        }
        return [out, k];
    };
    const members = (k) => {
        // toks[k] === '{'
        k++;
        const out = [];
        while (k < n && toks[k] !== '}') {
            if (toks[k] === 'layout') { k++; let d = 0; do { if (toks[k] === '(') d++; else if (toks[k] === ')') d--; k++; } while (k < n && d > 0); continue; }
            const [ds, next] = declarators(k);
            for (const d of ds) out.push(d);
            k = next;
        }
        return [out, k + 1];
    };

    let i = 0;
    while (i < n) {
        const t = toks[i];
        if (t === '{') {
            let d = 0, j = i;
            for (; j < n; j++) {
                const s = toks[j];
                if (s === '{') d++;
                else if (s === '}') { if (--d === 0) break; }
                else used.add(s);
            }
            i = j + 1;
            continue;
        }
        if (t === '(') { paren++; i++; continue; }
        if (t === ')') { paren--; i++; continue; }
        if (paren > 0) { i++; continue; }
        if (t === 'struct' && toks[i + 2] === '{') {
            const [ms, next] = members(i + 2);
            structs.set(toks[i + 1], ms);
            i = next;
            while (i < n && toks[i] !== ';') i++;
            i++;
            continue;
        }
        if (t === 'layout' && toks[i + 1] === '(') {
            let j = i + 2;
            layoutLocation = -1;
            while (j < n && toks[j] !== ')') {
                if (toks[j] === 'location' && toks[j + 1] === '=') layoutLocation = Number(toks[j + 2]);
                j++;
            }
            i = j + 1;
            continue;
        }
        if (t === 'uniform') {
            let k = i + 1;
            while (PRECISION.has(toks[k])) k++;
            if (toks[k + 1] === '{') {
                const blockName = toks[k];
                const [ms, next] = members(k + 1);
                let instance = '';
                k = next;
                if (toks[k] !== ';') instance = toks[k];
                while (k < n && toks[k] !== ';') k++;
                blocks.push({ name: blockName, members: ms, instance });
                i = k + 1;
            } else {
                const [ds, next] = declarators(k);
                for (const d of ds) uniforms.push(d);
                i = next;
            }
            layoutLocation = -1;
            continue;
        }
        if (isVertex && (t === 'in' || t === 'attribute')) {
            let k = i + 1;
            while (QUALIFIERS.has(toks[k])) k++;
            const [ds, next] = declarators(k);
            for (const d of ds) attribs.push({ name: d.name, type: d.type, size: d.size, location: layoutLocation });
            layoutLocation = -1;
            i = next;
            continue;
        }
        if (t === ';') layoutLocation = -1;
        i++;
    }
    return { structs, uniforms, blocks, attribs, used };
}

// ---- std140 --------------------------------------------------------------------

function std140(type, structs) {
    const p = GLSL_TYPES[type];
    if (p) return [p[1], p[2]];
    const ms = structs.get(type);
    if (!ms) return [16, 16];
    let off = 0;
    for (const m of ms) off = place(off, m, structs);
    return [(off + 15) & ~15, 16];
}

function place(off, m, structs) {
    let [size, align] = std140(m.type, structs);
    if (m.size) { align = 16; size = ((size + 15) & ~15) * m.size; }
    off = (off + align - 1) & ~(align - 1);
    return off + size;
}

// ---- the program -------------------------------------------------------------

// Returns { uniforms: [{ name, size, type, block }], attribs: [{ name, size, type, location }],
//           blocks: [{ name, size, members: [uniform index] }] }.
export function reflectProgram(vsSource, fsSource, bound) {
    const vs = parseShader(vsSource || '', true);
    const fs = parseShader(fsSource || '', false);
    const structs = new Map([...vs.structs, ...fs.structs]);
    const used = (name) => vs.used.has(name) || fs.used.has(name);

    const uniforms = [];
    const seen = new Set();
    const expand = (name, type, size, block) => {
        const ms = structs.get(type);
        if (ms) {
            const count = size || 1;
            for (let k = 0; k < count; k++) {
                const base = size ? name + '[' + k + ']' : name;
                for (const m of ms) if (used(m.name)) expand(base + '.' + m.name, m.type, m.size, block);
            }
            return;
        }
        const p = GLSL_TYPES[type];
        uniforms.push({ name: size ? name + '[0]' : name, size: size || 1, type: p ? p[0] : GL.FLOAT_VEC4, block });
    };
    for (const u of vs.uniforms.concat(fs.uniforms)) {
        if (seen.has(u.name) || !used(u.name)) continue;
        seen.add(u.name);
        expand(u.name, u.type, u.size, -1);
    }

    const blocks = [];
    for (const b of vs.blocks.concat(fs.blocks)) {
        if (seen.has('block:' + b.name)) continue;
        if (!(b.instance ? used(b.instance) : b.members.some((m) => used(m.name)))) continue;
        seen.add('block:' + b.name);
        const index = blocks.length;
        const first = uniforms.length;
        let off = 0;
        for (const m of b.members) {
            off = place(off, m, structs);
            expand(b.instance ? b.name + '.' + m.name : m.name, m.type, m.size, index);
        }
        const members = [];
        for (let k = first; k < uniforms.length; k++) members.push(k);
        blocks.push({ name: b.name, size: (off + 15) & ~15, members });
    }

    const attribs = [];
    const taken = new Set();
    const aseen = new Set();
    for (const a of vs.attribs) {
        if (aseen.has(a.name) || !vs.used.has(a.name)) continue;
        aseen.add(a.name);
        const p = GLSL_TYPES[a.type];
        const locs = (p ? p[3] : 1) * (a.size || 1);
        let loc = bound && bound.has(a.name) ? bound.get(a.name) : a.location;
        attribs.push({ name: a.name, size: a.size || 1, type: p ? p[0] : GL.FLOAT_VEC4, location: loc, locs });
        if (loc >= 0) for (let k = 0; k < locs; k++) taken.add(loc + k);
    }
    let next = 0;
    for (const a of attribs) {
        if (a.location >= 0) continue;
        for (;;) {
            let free = true;
            for (let k = 0; k < a.locs; k++) if (taken.has(next + k)) { free = false; break; }
            if (free) break;
            next++;
        }
        a.location = next;
        for (let k = 0; k < a.locs; k++) taken.add(next + k);
        next += a.locs;
    }
    return { uniforms, attribs, blocks };
}
