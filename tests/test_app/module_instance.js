// A module the PAGE evaluates, so a headless driver script can import the same
// specifier and be handed the page's instance rather than a second one.
//
// bronze compiles a whole module graph into one flattened program, so the page
// and a driver are two compilation units. Without the realm's module registry
// (bronze: runtime/module_registry.h) this file's top level runs once per unit,
// and a test that imports an app's module measures its own copy of the app.
// `evaluations` is what makes a second evaluation visible.
//
// Read by tests/headless/test_module_instance_sharing.js.
export const instance = {
  evaluations: (globalThis.__moduleInstanceEvaluations =
    (globalThis.__moduleInstanceEvaluations || 0) + 1),
  bumps: 0,
};

export function bump() {
  return ++instance.bumps;
}

// The identity half: a test compares the object it imported against this one.
globalThis.__pageModuleInstance = instance;
