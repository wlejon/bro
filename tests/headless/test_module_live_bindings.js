// A page module's `let` exports are LIVE bindings in a driver script, as an
// import is everywhere (ECMA-262 16.2.1.6.1): named imports and `import * as`
// both read the exporting module's current value.
//
// The driver binds the page's instance through the realm's module registry,
// and those bindings used to be a snapshot taken when this script started:
// `y` stayed 1 after the page's timer bumped it, and after this script's own
// `rebuild()` the imported `c` was still the page's first object.
import { y, c, rebuild, readC, readY, who } from "/app/module_entry/live.js";
import * as ns from "/app/module_entry/live.js";

assert(y === 1 && ns.y === 1, "before the page's timer: y=" + y + " ns.y=" + ns.y);
const first = c;
assert(first && first.n === 1, "the page's rebuild() at load is not visible");

// The page's own setTimeout(bump, 50) reassigns `y` in the page's module.
advanceTime(100);
assert(readY() === 2, "the page's timer did not run (readY()=" + readY() + ")");
assert(y === 2, "named import `y` is a snapshot: " + y);
assert(ns.y === 2, "namespace import `ns.y` is a snapshot: " + ns.y);

// A write this script causes, through the page's function.
rebuild();
assert(c === readC(), "named import `c` is not the page's current object");
assert(c !== first && c.n === 2, "c.n = " + (c && c.n));
assert(ns.c === c, "ns.c disagrees with c");

// A closure reads the binding when it runs, not when it was made.
const peek = () => y;
assert(peek() === 2, "peek() = " + peek());

// Calling an import binding passes `this` undefined, as calling any binding does.
assert(who() === undefined, "who() saw a receiver");

// The in-page half: a page module importing live.js saw the same bump.
const inPage = globalThis.__moduleEntryInPage;
assert(inPage && inPage.y === 2 && inPage.nsY === 2,
  "page-to-page import is not live: " + JSON.stringify(inPage));

console.log("MODULE LIVE BINDINGS OK");
