// Diff two surface_dump.js outputs (old = QuickJS oracle, new = bronze).
//   node scripts/binding-audit/surface_diff.mjs old.txt new.txt [--added] [--group PREFIX]
// Reports: members present in old but not new (collapsed under a missing
// parent), members whose kind/arity changed, and (with --added) new members.
import { readFileSync } from 'node:fs';

const [oldFile, newFile, ...flags] = process.argv.slice(2);
const showAdded = flags.includes('--added');
const groupIdx = flags.indexOf('--group');
const onlyGroup = groupIdx >= 0 ? flags[groupIdx + 1] : null;

function load(f) {
  const m = new Map();
  for (const line of readFileSync(f, 'utf8').split(/\r?\n/)) {
    if (!line || line === 'END') continue;
    const sp = line.indexOf(' ');
    if (sp < 0) continue;
    m.set(line.slice(0, sp), line.slice(sp + 1));
  }
  return m;
}
const oldM = load(oldFile), newM = load(newFile);

function parentOf(p) {
  const i = Math.max(p.lastIndexOf('.'), p.lastIndexOf('#'));
  return i > 0 ? p.slice(0, i) : null;
}
function groupOf(p) {
  // global.X -> global.X ; bro.tts.x -> bro.tts ; inst.AudioContext#x -> inst.AudioContext ; inst.el.div#x -> inst.el.div
  const hash = p.indexOf('#');
  if (hash > 0) return p.slice(0, hash);
  const parts = p.split('.');
  if (parts[0] === 'global') return parts.slice(0, 2).join('.');
  return parts.slice(0, Math.min(parts.length - 1, 3)).join('.');
}

// ES builtins we do not blame the binding layer for.
const BUILTIN = new Set(['Object','Function','Array','Number','parseFloat','parseInt','Infinity','NaN','undefined','Boolean','String','Symbol','Date','Promise','RegExp','Error','AggregateError','EvalError','RangeError','ReferenceError','SyntaxError','TypeError','URIError','globalThis','JSON','Math','Intl','ArrayBuffer','Atomics','Uint8Array','Int8Array','Uint16Array','Int16Array','Uint32Array','Int32Array','Float32Array','Float64Array','Uint8ClampedArray','BigUint64Array','BigInt64Array','DataView','Map','BigInt','Set','WeakMap','WeakSet','Proxy','Reflect','FinalizationRegistry','WeakRef','decodeURI','decodeURIComponent','encodeURI','encodeURIComponent','escape','unescape','eval','isFinite','isNaN','SharedArrayBuffer','Iterator','Float16Array']);
function isBuiltin(p) {
  const m = /^global\.([A-Za-z0-9_$]+)/.exec(p);
  return m && BUILTIN.has(m[1]);
}

// A resolved probe (gen_probe.mjs) answers every old path: a kind, or a
// `!reason` when the name does not resolve. A path the probe skipped (not in
// the new file at all) is "not probed", never "missing".
const missing = [], changed = [], added = [];
const missingSet = new Set();
let notProbed = 0;
const failReason = new Map();
for (const [p, k] of oldM) {
  if (onlyGroup && !p.startsWith(onlyGroup)) continue;
  if (!newM.has(p)) { notProbed++; continue; }
  const nk = newM.get(p);
  if (nk.startsWith('!')) { missing.push(p); missingSet.add(p); failReason.set(p, nk); }
  else if (nk !== k) changed.push([p, k, nk]);
}
if (showAdded) for (const p of newM.keys()) if (!oldM.has(p) && (!onlyGroup || p.startsWith(onlyGroup))) added.push(p);

// collapse missing under a missing parent
const collapsed = new Map(); // root missing path -> child count
for (const p of missing) {
  let q = p, root = p;
  while ((q = parentOf(q)) && missingSet.has(q)) root = q;
  if (root === p) { if (!collapsed.has(p)) collapsed.set(p, 0); }
  else collapsed.set(root, (collapsed.get(root) || 0) + 1);
}

function byGroup(items, keyFn) {
  const g = new Map();
  for (const it of items) { const k = groupOf(keyFn(it)); if (!g.has(k)) g.set(k, []); g.get(k).push(it); }
  return [...g.entries()].sort((a, b) => a[0].localeCompare(b[0]));
}

// `global.Class#member` is a prototype-chain read of the class; the new
// bindings put many members on instances, so those rows are only meaningful
// when no `inst.` row covers them. They go to a low-priority section.
const protoOnly = [...collapsed.entries()].filter(([p]) => /^global\.[^.#]+#/.test(p));
const primary = [...collapsed.entries()].filter(([p]) => !/^global\.[^.#]+#/.test(p));

console.log(`old: ${oldM.size} members · new: ${newM.size} members · not probed: ${notProbed}`);
console.log(`\n## MISSING in new (${missing.length} members, ${collapsed.size} after collapsing under missing parents; ${protoOnly.length} prototype-only rows listed last)\n`);
for (const [grp, items] of byGroup(primary, e => e[0])) {
  const bi = isBuiltin(grp);
  console.log(`### ${grp}${bi ? '  (ES builtin)' : ''}  — ${items.length}`);
  for (const [p, n] of items) console.log(`  ${p}  ${oldM.get(p)}${n ? `  (+${n} children)` : ''}${failReason.get(p) !== '!missing' ? '  ' + failReason.get(p) : ''}`);
}

// Element instances: members missing on EVERY tag are generic Element gaps;
// the rest are per-tag (the old runtime exposed one mega-class on all tags,
// so a <div> losing play() is expected, but <video> losing it is not).
const elTags = new Map();
for (const p of missing) {
  const m = /^inst\.el\.([a-z]+)#(.+)$/.exec(p);
  if (!m) continue;
  if (!elTags.has(m[1])) elTags.set(m[1], new Set());
  elTags.get(m[1]).add(m[2]);
}
if (elTags.size) {
  const tags = [...elTags.keys()];
  const generic = [...elTags.get(tags[0])].filter(n => tags.every(t => elTags.get(t).has(n))).sort();
  console.log(`\n## ELEMENT members missing on every tag (${generic.length})\n  ${generic.map(n => n + ' ' + oldM.get('inst.el.' + tags[0] + '#' + n)).join('\n  ')}`);
  console.log(`\n## ELEMENT members missing per tag (beyond the generic set)`);
  for (const t of tags) {
    const extra = [...elTags.get(t)].filter(n => !generic.includes(n)).sort();
    if (extra.length) console.log(`  ${t}: ${extra.join(', ')}`);
  }
}

console.log(`\n## MISSING prototype-only rows (global.Class#member; check the inst. row first)\n`);
for (const [grp, items] of byGroup(protoOnly, e => e[0])) {
  console.log(`### ${grp}  — ${items.length}`);
  for (const [p, n] of items) console.log(`  ${p}  ${oldM.get(p)}${n ? `  (+${n} children)` : ''}`);
}
console.log(`\n## CHANGED kind/arity (${changed.length})\n`);
for (const [grp, items] of byGroup(changed, e => e[0])) {
  console.log(`### ${grp}  — ${items.length}`);
  for (const [p, a, b] of items) console.log(`  ${p}  old=${a}  new=${b}`);
}
if (showAdded) {
  console.log(`\n## ADDED in new (${added.length})\n`);
  for (const [grp, items] of byGroup(added, p => p)) console.log(`### ${grp} — ${items.length}\n  ${items.join('\n  ')}`);
}
