// Build a name-driven probe script from an old-runtime dump:
//   node scripts/binding-audit/gen_probe.mjs old.txt > probe.js
//   bro-headless <appdir> probe.js 2>&1 | grep -o "RES .*" | sed 's/^RES //' > new_resolved.txt
//   node scripts/binding-audit/surface_diff.mjs old.txt new_resolved.txt
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const BUILTIN = new Set(['Object','Function','Array','Number','parseFloat','parseInt','Infinity','NaN','undefined','Boolean','String','Symbol','Date','Promise','RegExp','Error','AggregateError','EvalError','RangeError','ReferenceError','SyntaxError','TypeError','URIError','globalThis','JSON','Math','Intl','ArrayBuffer','Atomics','Uint8Array','Int8Array','Uint16Array','Int16Array','Uint32Array','Int32Array','Float32Array','Float64Array','Uint8ClampedArray','BigUint64Array','BigInt64Array','DataView','Map','BigInt','Set','WeakMap','WeakSet','Proxy','Reflect','FinalizationRegistry','WeakRef','decodeURI','decodeURIComponent','encodeURI','encodeURIComponent','escape','unescape','eval','isFinite','isNaN','SharedArrayBuffer','Iterator','Float16Array','DisposableStack','AsyncDisposableStack','SuppressedError','InternalError']);
const oldFile = process.argv[2];
const filter = process.argv[3] ? new RegExp(process.argv[3]) : null;   // optional path regex
const paths = [];
for (const line of readFileSync(oldFile, 'utf8').split(/\r?\n/)) {
  if (!line || line === 'END') continue;
  const sp = line.indexOf(' ');
  if (sp < 0) continue;
  const p = line.slice(0, sp), k = line.slice(sp + 1);
  if (k.startsWith('!')) continue;          // construction failures are not members
  if (/^global\.__/.test(p)) continue;      // private bridge names, expected to differ
  // ES builtins are bronze's business, not the binding layer's — and walking
  // some of them (Object.getPrototypeOf(Atomics)) hits bronze::fatal.
  const m = /^global\.([A-Za-z0-9_$]+)/.exec(p);
  if (m && BUILTIN.has(m[1])) continue;
  if (filter && !filter.test(p)) continue;
  paths.push(p);
}
const dump = readFileSync(join(dirname(fileURLToPath(import.meta.url)), 'surface_dump.js'), 'utf8');
// bronze resolves some host globals only as bare identifiers at compile time
// (globalThis['advanceTime'] is undefined while advanceTime works), so every
// single-segment global is also probed as a bare name. These lines come after
// resolveAll(), so they override the lookup-based answer for the same path.
const bare = [];
for (const p of paths) {
  const m = /^global\.([A-Za-z_$][A-Za-z0-9_$]*)$/.exec(p);
  if (!m) continue;
  const n = m[1];
  bare.push(`try { var v_${n} = ${n}; console.log('RES global.${n} ' + (typeof v_${n} === 'function' ? 'fn ' + v_${n}.length : typeof v_${n} === 'object' ? (v_${n} === null ? 'null' : 'obj') : typeof v_${n})); } catch (e) { console.log('RES global.${n} !missing'); }`);
}
// globalThis.*, not `var`: bronze compiles the driver script as a module, so a
// top-level var would be module-scoped and invisible to the dump's lookup.
process.stdout.write('globalThis.__SURF_PATHS = ' + JSON.stringify(paths) + ';\n' + dump + '\n' + bare.join('\n') + '\nconsole.log("SURF END2");\n');
