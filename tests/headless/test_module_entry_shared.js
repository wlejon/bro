// A driver script importing the page's module ENTRY — the file a
// `<script type="module" src>` names — gets the instance the page ran.
//
// Only a page's non-entry modules used to be published to the realm's module
// registry, so `import "/app/module_entry/main.js"` compiled the entry into
// this unit and ran it again: a second boot into the same DOM. The page names
// that file in two tags as well, and one src is one module (HTML's module map),
// so its top level has run exactly once when this script starts.
import { entryState, setEntryState } from "/app/module_entry/main.js";
import * as entryNs from "/app/module_entry/main.js";

assert(
  globalThis.__moduleEntryRuns === 1,
  "module_entry/main.js top level ran " + globalThis.__moduleEntryRuns +
    " times; the page's two tags and this import must share one instance"
);

assert(entryState === "booted", "entryState is " + entryState);

// A call into the page's instance, then a read of the binding it reassigned.
setEntryState("driven");
assert(entryState === "driven", "the entry's `let` did not update here: " + entryState);
assert(entryNs.entryState === "driven", "namespace read saw " + entryNs.entryState);

console.log("MODULE ENTRY SHARED OK");
