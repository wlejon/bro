// An external module ENTRY of the page (`<script type="module" src>`), for
// tests/headless/test_module_entry_shared.js and test_module_live_bindings.js.
//
// A driver script that imports this file must get the instance the page ran,
// not a second boot of it: `__moduleEntryRuns` counts evaluations of this top
// level. index.html names this file twice, and one src is one module.
//
// It also checks the in-page half of live bindings: this module imports `y`
// and `ns` from ./live.js, and reads both after live.js's own `bump()` has
// reassigned `y` (a page-to-page import inside one compilation unit).
import { y, bump, rebuild } from "./live.js";
import * as ns from "./live.js";

globalThis.__moduleEntryRuns = (globalThis.__moduleEntryRuns || 0) + 1;

export let entryState = "booted";

export function setEntryState(v) {
  entryState = v;
}

rebuild();
setTimeout(() => {
  bump();
  globalThis.__moduleEntryInPage = { y, nsY: ns.y };
}, 50);
