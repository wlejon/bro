// A driver script's `import "/app/..."` must get the PAGE's module instance.
//
// bronze compiles a module graph by flattening it into one program, so the
// page's scripts are one compilation unit and this file is another. Before the
// realm's module registry (bronze: runtime/module_registry.h) the second unit
// compiled `/app/module_instance.js` into itself and ran its top level again —
// a test then measured its own private copy of the app while the app kept
// running on the first one. The page bumps once at load; if this file were
// looking at a second instance it would see `bumps === 0` and a different
// object.
//
// The live-binding note below is the one gap in the module registry a test is
// likely to meet.
import { instance, bump } from "/app/module_instance.js";

assert(
  globalThis.__moduleInstanceEvaluations === 1,
  "module_instance.js top level ran " + globalThis.__moduleInstanceEvaluations +
    " times; the page's unit and this one must share the instance"
);

assert(
  instance === globalThis.__pageModuleInstance,
  "the imported object is not the one the page's module published"
);

assert(
  instance.bumps === 1,
  "expected the page's single bump() to be visible here, saw " + instance.bumps
);

// A call through the shared binding mutates the state the page holds.
bump();
assert(
  globalThis.__pageModuleInstance.bumps === 2,
  "a bump() from the driver did not reach the page's instance (saw " +
    globalThis.__pageModuleInstance.bumps + ")"
);

// A namespace import over the same specifier is the same instance again: the
// namespace this unit declares is built from the bindings the registry lookup
// produced, not from a second evaluation.
import * as ns from "/app/module_instance.js";
assert(ns.instance === instance, "import * as gave a different instance");
assert(typeof ns.bump === "function", "import * as lost a function export");

console.log("MODULE INSTANCE SHARING OK");
