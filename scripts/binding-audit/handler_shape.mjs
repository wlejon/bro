// Static handler-shape diff: old QuickJS bindings vs new bronze bindings.
//   node scripts/binding-audit/handler_shape.mjs [--missing] [--filter REGEX] [--verbose]
// For every registered JS method on both sides it extracts, from the handler
// body: option keys read, keys written to result objects, string enums
// compared, highest positional arg touched, argc thresholds, throw count.
// Handlers are paired by registered name (ties broken by file-name overlap)
// and only differing shapes are printed. Nothing is executed.
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join, basename } from 'node:path';

const OLD_DIR = 'D:/projects/bro-quickjs-oracle/src/js';
const NEW_ROOTS = [
  'D:/projects/bro/src/bronze_host',
  'D:/projects/broaudio/src/api', 'D:/projects/brosoundml/src/api', 'D:/projects/brolm/src/api',
  'D:/projects/brodiffusion/src/api', 'D:/projects/brovisionml/src/api', 'D:/projects/bromesh/src/api',
  'D:/projects/brogameagent/src/api', 'D:/projects/broimage/src/api', 'D:/projects/brotensor/src/api',
  'D:/projects/broflora/src/api', 'D:/projects/brokit/src/api',
];
const args = process.argv.slice(2);
const showMissing = args.includes('--missing');
const verbose = args.includes('--verbose');
const fi = args.indexOf('--filter');
const filter = fi >= 0 ? new RegExp(args[fi + 1]) : null;

function walk(dir, out = []) {
  let ents = [];
  try { ents = readdirSync(dir); } catch { return out; }
  for (const e of ents) {
    const p = join(dir, e);
    const st = statSync(p);
    if (st.isDirectory()) { if (!/build|third_party|node_modules|tests?$/.test(e)) walk(p, out); }
    else if (/\.(cpp|cc|h|js)$/.test(e)) out.push(p);
  }
  return out;
}

// Balanced-brace body starting at the first '{' at or after `from`.
function bodyFrom(src, from) {
  const open = src.indexOf('{', from);
  if (open < 0) return null;
  let depth = 0, i = open, inStr = null, inLine = false, inBlock = false;
  for (; i < src.length; i++) {
    const c = src[i], n = src[i + 1];
    if (inLine) { if (c === '\n') inLine = false; continue; }
    if (inBlock) { if (c === '*' && n === '/') { inBlock = false; i++; } continue; }
    if (inStr) { if (c === '\\') { i++; continue; } if (c === inStr) inStr = null; continue; }
    if (c === '/' && n === '/') { inLine = true; continue; }
    if (c === '/' && n === '*') { inBlock = true; continue; }
    if (c === '"' || c === "'") { inStr = c; continue; }
    if (c === '{') depth++;
    else if (c === '}') { depth--; if (depth === 0) return { start: open, end: i + 1, text: src.slice(open, i + 1) }; }
  }
  return null;
}

// first argument may itself be a call (o.get(), opts.get(), self_) — allow one level of parens
const READ_RE = /\b(?!JS_Set|JS_Define|set|Set|define)(?:JS_GetPropertyStr|JS_GetProperty[A-Za-z]*|[A-Za-z_:]*(?:[gG]et|[pP]rop|[oO]pt|[rR]ead|[hH]as)[A-Za-z0-9_]*)\s*\(\s*(?:(?:[^;()"]|\([^;()"]*\))*?,\s*)?"([A-Za-z_$][A-Za-z0-9_$]*)"\s*[,)]/g;
const WRITE_RE = /\b(?:JS_SetPropertyStr|JS_DefinePropertyValueStr|[A-Za-z_]*\.set|[A-Za-z_]*\.define|setProperty|setProp[A-Za-z]*)\s*\(\s*(?:(?:[^;()"]|\([^;()"]*\))*?,\s*)?"([A-Za-z_$][A-Za-z0-9_$]*)"\s*[,)]/g;
const ENUM_RE = /(?:strcmp\s*\(\s*[^,]+,\s*"([A-Za-z0-9_-]+)"|==\s*"([A-Za-z0-9_-]+)"|"([A-Za-z0-9_-]+)"\s*==|case\s+"([A-Za-z0-9_-]+)")/g;

function shape(body) {
  const keys = new Set(), out = new Set(), enums = new Set();
  let m;
  for (const re of [READ_RE]) { re.lastIndex = 0; while ((m = re.exec(body))) keys.add(m[1]); }
  WRITE_RE.lastIndex = 0; while ((m = WRITE_RE.exec(body))) out.add(m[1]);
  ENUM_RE.lastIndex = 0; while ((m = ENUM_RE.exec(body))) enums.add(m[1] || m[2] || m[3] || m[4]);
  let maxArg = -1;
  for (const re of [/\b(?:a|args|argv)\[(\d+)\]/g, /[A-Za-z0-9_]*(?:At|Arg|arg)\(\s*(?:a|args)\s*,\s*(\d+)/g, /\b\w+\.(?:get|has|is)(?:Double|Int|Uint|Bool|String|Str|Float|Value|Object|Function|Array|Arg|F32|I32|U32|Number|TypedArray|Buffer)\w*\(\s*(\d+)/g]) {
    re.lastIndex = 0; while ((m = re.exec(body))) maxArg = Math.max(maxArg, +m[1]);
  }
  const argcChecks = new Set();
  for (const re of [/\bargc\s*([<>=!]+)\s*(\d+)/g, /\b(?:a|args)\.size\(\)\s*([<>=!]+)\s*(\d+)/g]) {
    re.lastIndex = 0; while ((m = re.exec(body))) argcChecks.add(m[1] + m[2]);
  }
  const throws = (body.match(/\bJS_Throw|\bthrow(?:Error|TypeError|RangeError)\b|\bthrow\s+/g) || []).length;
  // type dispatch on positional args (string vs number vs object overloads)
  const typeChecks = new Set();
  for (const re of [/JS_Is(String|Number|Object|Function|Array|Bool|Undefined|Null)\s*\(\s*ctx\s*,\s*argv\[(\d+)\]/g, /JS_Is(String|Number|Object|Function|Array|Bool|Undefined|Null)\s*\(\s*argv\[(\d+)\]/g, /\bis(String|Number|Object|Function|Array|Bool|Undefined|Null)\s*\(\s*(?:a|args)\[(\d+)\]/g]) {
    re.lastIndex = 0; while ((m = re.exec(body))) typeChecks.add(m[1].toLowerCase() + '@' + m[2]);
  }
  return { keys, out, enums, maxArg, argcChecks, throws, typeChecks };
}

// ---- OLD: static JSValue js_x(...) functions + registration lines ----
function parseOld(file) {
  const src = readFileSync(file, 'utf8');
  const fns = new Map(); // fn symbol -> body
  const fnRe = /^static\s+(?:JSValue|int|void)\s+(js_[A-Za-z0-9_]+|[A-Za-z0-9_]+)\s*\(\s*JSContext\s*\*\s*\w+/gm;
  let m;
  while ((m = fnRe.exec(src))) { const b = bodyFrom(src, m.index); if (b) fns.set(m[1], b.text); }
  const handlers = [];
  // class scope hint: old symbols are js_<class>_<method>; lambdas fall back to the enclosing function
  const scopes = [];
  const topRe = /^(?:static\s+)?[A-Za-z_][\w:<>*& ]*\s+([A-Za-z_]\w*)\s*\([^;]*\)\s*\{/gm;
  while ((m = topRe.exec(src))) scopes.push([m.index, m[1]]);
  const scopeAt = i => { let s = ''; for (const [at, n] of scopes) { if (at > i) break; s = n; } return s; };
  const symScope = s => s.replace(/^js_/, '').replace(/_[A-Za-z0-9]+$/, '');
  // .method_raw("name", fn, argc) / .method("name", fn[, argc]) / JS_CFUNC_DEF("name", argc, fn)
  const regRe = /\.(?:method_raw|method|getset|prop|getter|setter)\s*\(\s*"([^"]+)"\s*,\s*([A-Za-z0-9_:<>]+|\[)/g;
  while ((m = regRe.exec(src))) {
    const name = m[1];
    if (m[2] === '[') { const b = bodyFrom(src, m.index); if (b) handlers.push({ name, file, argc: null, body: b.text, scope: scopeAt(m.index) }); continue; }
    const argcM = /,\s*(\d+)\s*\)/.exec(src.slice(m.index, src.indexOf('\n', m.index) + 1));
    handlers.push({ name, file, argc: argcM ? +argcM[1] : null, body: fns.get(m[2]) || '' , sym: m[2], scope: symScope(m[2]) });
  }
  // free functions: .function("name", fn, argc) and JS_NewCFunction[2](ctx, fn, "name", argc)
  const fnRegRe = /\.function\s*\(\s*"([^"]+)"\s*,\s*([A-Za-z0-9_:<>]+)\s*(?:,\s*(\d+))?/g;
  while ((m = fnRegRe.exec(src))) handlers.push({ name: m[1], file, argc: m[3] != null ? +m[3] : null, body: fns.get(m[2]) || '', sym: m[2], scope: symScope(m[2]) });
  const ncRe = /JS_NewCFunction2?\s*\(\s*ctx\s*,\s*([A-Za-z0-9_]+)\s*,\s*"([^"]+)"\s*,\s*(\d+)/g;
  while ((m = ncRe.exec(src))) handlers.push({ name: m[2], file, argc: +m[3], body: fns.get(m[1]) || '', sym: m[1], scope: symScope(m[1]) });
  const cfRe = /JS_CFUNC_DEF\s*\(\s*"([^"]+)"\s*,\s*(\d+)\s*,\s*([A-Za-z0-9_]+)/g;
  while ((m = cfRe.exec(src))) handlers.push({ name: m[1], file, argc: +m[2], body: fns.get(m[3]) || '', sym: m[3], scope: symScope(m[3]) });
  const gsRe = /JS_CGETSET_DEF\s*\(\s*"([^"]+)"\s*,\s*([A-Za-z0-9_]+)\s*,\s*([A-Za-z0-9_]+)/g;
  while ((m = gsRe.exec(src))) handlers.push({ name: m[1], file, argc: null, body: (fns.get(m[2]) || '') + (fns.get(m[3]) || ''), accessor: true, scope: symScope(m[2]) });
  return handlers;
}

// ---- NEW: b.def("name", argc, [](...) { ... }) / b.accessor("name", g, s) / brosurface natives ----
// brosurface JS wrappers: fn(Class.prototype, "name", function name(...) { ... }) — the
// option-bag parsing lives here, the C++ body gets flattened scalars.
function parseWrapper(file) {
  const src = readFileSync(file, 'utf8');
  const handlers = [];
  let m;
  const re = /\b(?:fn|accessor)\(\s*([A-Za-z0-9_]+)(?:\.prototype)?\s*,\s*"([^"]+)"\s*,/g;
  while ((m = re.exec(src))) {
    const b = bodyFrom(src, m.index); if (!b) continue;
    const keys = new Set(), out = new Set(), enums = new Set();
    let k;
    const kr = /\b(?!this\b|__bro_native\b|Math\b|Object\b|Array\b|JSON\b|console\b|globalThis\b|prototype\b|Number\b|String\b)[A-Za-z_$][\w$]*\??\.([A-Za-z_$][\w$]*)\b/g;
    while ((k = kr.exec(b.text))) if (!/^(prototype|length|call|apply|bind|push|slice|map|forEach|isArray|from|of|toString|byteLength|buffer|byteOffset|constructor)$/.test(k[1])) keys.add(k[1]);
    const or = /[{,]\s*([A-Za-z_$][\w$]*)\s*:/g;
    while ((k = or.exec(b.text))) out.add(k[1]);
    const er = /(?:===|==|!==|!=)\s*"([A-Za-z0-9_-]+)"|case\s+"([A-Za-z0-9_-]+)"/g;
    while ((k = er.exec(b.text))) enums.add(k[1] || k[2]);
    const argsM = /function\s*\w*\s*\(([^)]*)\)/.exec(src.slice(m.index, b.start + 1));
    const nParams = argsM ? argsM[1].split(',').filter(s => s.trim()).length : 0;
    handlers.push({ name: m[2], cls: m[1], file, argc: null, body: b.text, wrapper: true, scope: m[1], pre: { keys, out, enums, maxArg: nParams - 1, throws: (b.text.match(/\bthrow\b/g) || []).length } });
  }
  return handlers;
}

function parseNew(file) {
  if (file.endsWith('.js')) return parseWrapper(file);
  const src = readFileSync(file, 'utf8');
  const handlers = [];
  let m;
  // named handler functions: [static] [ev::]Value sym(Value self, std::span<const Value> a) { ... }
  const fns = new Map();
  const fnRe = /^(?:static\s+)?(?:ev::)?Value\s+([A-Za-z_]\w*)\s*\(\s*(?:ev::)?Value\b/gm;
  while ((m = fnRe.exec(src))) { const b = bodyFrom(src, m.index); if (b) fns.set(m[1], b.text); }
  // enclosing top-level function name = class scope hint for lambdas
  const scopes = [];
  const topRe = /^(?:static\s+)?[A-Za-z_][\w:<>*& ]*\s+([A-Za-z_]\w*)\s*\([^;]*\)\s*(?:const\s*)?\{/gm;
  while ((m = topRe.exec(src))) scopes.push([m.index, m[1]]);
  const scopeAt = i => { let s = ''; for (const [at, n] of scopes) { if (at > i) break; s = n; } return s; };
  const defRe = /\b\w+\.def\s*\(\s*"([^"]+)"\s*,\s*(\d+)\s*,\s*([A-Za-z_][\w:]*|\[)/g;
  while ((m = defRe.exec(src))) {
    if (m[3] !== '[') { handlers.push({ name: m[1], file, argc: +m[2], body: fns.get(m[3].replace(/^.*::/, '')) || '', sym: m[3], scope: m[3] }); continue; }
    const b = bodyFrom(src, m.index); if (b) handlers.push({ name: m[1], file, argc: +m[2], body: b.text, scope: scopeAt(m.index) });
  }
  // local registration helpers: defFv("uniform1fv", ...), defMat(...), defStrAttr("href", ...) — body is the helper's, not visible here
  const helperRe = /\bdef[A-Z]\w*\s*\(\s*"([^"]+)"/g;
  while ((m = helperRe.exec(src))) handlers.push({ name: m[1], file, argc: null, body: '', helper: true, scope: scopeAt(m.index) });
  const accRe = /\b\w+\.accessor\s*\(\s*"([^"]+)"\s*,/g;
  while ((m = accRe.exec(src))) {
    // getter/setter are lambdas inline or named symbols; take up to two bodies within the call
    const callEnd = src.indexOf(');', m.index);
    const b1 = bodyFrom(src, m.index);
    let text = b1 && b1.start < callEnd ? b1.text : '';
    if (b1 && b1.end < callEnd) { const b2 = bodyFrom(src, b1.end); if (b2 && b2.start < callEnd) text += b2.text; }
    handlers.push({ name: m[1], file, argc: null, body: text, accessor: true, scope: scopeAt(m.index) });
  }
  // brosurface hand-written bodies: <ret> bro_<sub>_<Class>_<method>(...) { }
  const natRe = /^[A-Za-z_][\w:<>*& ]*\s+bro_[a-z0-9]+_([A-Za-z0-9]+)_([A-Za-z0-9_]+)\s*\(/gm;
  while ((m = natRe.exec(src))) { const b = bodyFrom(src, m.index); if (b) handlers.push({ name: m[2], cls: m[1], file, argc: null, body: b.text, native: true, scope: m[1] }); }
  return handlers;
}

const oldH = walk(OLD_DIR).flatMap(parseOld);
const newH = NEW_ROOTS.flatMap(r => walk(r)).flatMap(parseNew);
for (const h of [...oldH, ...newH]) {
  h.s = shape(h.body);
  if (h.pre) { for (const f of ['keys', 'out', 'enums']) h.s[f] = h.pre[f]; h.s.maxArg = h.pre.maxArg; h.s.throws = h.pre.throws; }
}
// A brosurface method = C++ native body + JS wrapper of the same (class, name): merge into one shape.
{
  const natives = newH.filter(h => h.native), wrappers = newH.filter(h => h.wrapper);
  const wmap = new Map(wrappers.map(w => [w.cls + '#' + w.name, w]));
  for (const n of natives) {
    const w = wmap.get(n.cls + '#' + n.name); if (!w) continue;
    for (const f of ['keys', 'out', 'enums', 'typeChecks', 'argcChecks']) for (const x of w.s[f] || []) n.s[f].add(x);
    n.s.maxArg = Math.max(n.s.maxArg, w.s.maxArg); n.s.throws += w.s.throws; w.merged = true;
  }
}
const newHAll = newH.filter(h => !h.merged);

const stem = f => basename(f).replace(/\.(cpp|cc|h)$/, '').replace(/_bindings.*$/, '').replace(/^(host_|native_|api_?)/, '');
const tokens = f => new Set(stem(f).split(/[_\W]+/).filter(t => t.length > 2));
const camel = s => (s || '').replace(/([a-z0-9])([A-Z])/g, '$1_$2').toLowerCase().split(/[_\W]+/).filter(t => t.length > 2 && !/^(install|make|host|value|impl|native|ctor|class|define|js|api|get|set)$/.test(t));
function affinity(o, n) {
  const a = tokens(o.file), b = new Set([...tokens(n.file), ...n.file.toLowerCase().split(/[\\/]/).map(x => x.replace(/^bro/, ''))]);
  let s = 0; for (const t of a) if (b.has(t)) s++;
  // class-scope overlap (js_model_generate ↔ installLMModel / hostLMModel...) outweighs file affinity
  const os = camel(o.scope), ns = new Set(camel(n.scope));
  for (const t of os) if (ns.has(t)) s += 3;
  if (n.file.endsWith('.js') && !o.file.includes('scene') && !o.file.includes('physics')) s -= 2;
  return s;
}
// Which new roots an old binding file may pair with (sibling ports never cross siblings).
const ROOT_OF = [
  [/^audio|^mic/, /broaudio|bronze_host/], [/^(tts|rave|stt|diar|wake|kws|sense|gesture|listen)/, /brosoundml/],
  [/^lm/, /brolm/], [/^(diffusion|triposplat)/, /brodiffusion/], [/^vision/, /brovisionml/],
  [/^(mesh|rigging)/, /bromesh/], [/^ai_/, /brogameagent/], [/^(image|media|video|imagebitmap)/, /broimage|bronze_host/],
  [/^tensor/, /brotensor/], [/^flora/, /broflora/], [/./, /bronze_host|brokit/],
];
const allowedRoot = oldFile => ROOT_OF.find(([re]) => re.test(basename(oldFile)))[1];
const byName = new Map();
for (const h of newHAll) { if (!byName.has(h.name)) byName.set(h.name, []); byName.get(h.name).push(h); }

const diff = (A, B) => [...A].filter(x => !B.has(x));
const rel = f => f.replace(/^D:\/projects\//, '').replace(/\\/g, '/');
let printed = 0, missing = [];
for (const o of oldH) {
  if (filter && !filter.test(o.name) && !filter.test(o.file)) continue;
  const rootRe = allowedRoot(o.file);
  const cands = (byName.get(o.name) || []).filter(c => rootRe.test(c.file));
  if (!cands.length) { missing.push(o); continue; }
  const n = cands.map(c => [affinity(o, c), c]).sort((x, y) => y[0] - x[0])[0][1];
  const lines = [];
  const NOISE = /^(length|__\w+|byteLength|buffer|byteOffset|constructor|prototype)$/;
  const kOld = diff(o.s.keys, n.s.keys).filter(k => !NOISE.test(k)), kNew = diff(n.s.keys, o.s.keys).filter(k => !NOISE.test(k));
  if (kOld.length) lines.push(`  keys read only in OLD: ${kOld.join(', ')}`);
  if (kNew.length && verbose) lines.push(`  keys read only in NEW: ${kNew.join(', ')}`);
  const oOld = diff(o.s.out, n.s.out).filter(k => !NOISE.test(k));
  if (oOld.length) lines.push(`  result keys only in OLD: ${oOld.join(', ')}`);
  const eOld = diff(o.s.enums, n.s.enums);
  if (eOld.length) lines.push(`  string enums only in OLD: ${eOld.join(', ')}`);
  if (o.s.maxArg > n.s.maxArg) lines.push(`  positional args: OLD touches argv[${o.s.maxArg}], NEW stops at [${n.s.maxArg}]`);
  if (o.argc != null && n.argc != null && o.argc !== n.argc) lines.push(`  declared length: OLD ${o.argc} NEW ${n.argc}`);
  const tOld = diff(o.s.typeChecks, n.s.typeChecks);
  if (tOld.length) lines.push(`  type dispatch only in OLD: ${tOld.join(', ')}`);
  if (o.s.throws > 0 && n.s.throws === 0 && o.body.length > 0 && n.body.length > 0) lines.push(`  OLD throws (${o.s.throws}x), NEW never throws`);
  if (!lines.length) continue;
  printed++;
  console.log(`\n${o.name}  [${rel(o.file)}${o.scope ? ' ' + o.scope : ''} → ${rel(n.file)}${n.scope ? ' ' + n.scope : ''}]`);
  for (const l of lines) console.log(l);
}
console.log(`\n# ${oldH.length} old handlers, ${newH.length} new handlers, ${printed} pairs differ, ${missing.length} old names with no new handler of that name`);
if (showMissing) {
  const byFile = new Map();
  for (const o of missing) { const k = rel(o.file); if (!byFile.has(k)) byFile.set(k, []); byFile.get(k).push(o.name); }
  for (const [f, names] of [...byFile.entries()].sort()) console.log(`\n## ${f} (${names.length})\n  ${[...new Set(names)].join(', ')}`);
}
