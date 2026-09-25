// Reassigned exports, for tests/headless/test_module_live_bindings.js. An
// import is a LIVE view of the exporting module's binding (ECMA-262
// 16.2.1.6.1), so every reassignment below has to show through a driver's
// `import { y, c }` and `import * as ns`, not only through the accessors.
export let y = 1;
export let c = null;

export function bump() {
  y++;
}

export function rebuild() {
  c = { n: (c ? c.n : 0) + 1 };
}

export function readC() {
  return c;
}

export function readY() {
  return y;
}

// `this` of a call through an import binding is undefined, not a namespace.
export function who() {
  return this;
}
