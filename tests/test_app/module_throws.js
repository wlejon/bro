// A module the page does NOT load, so a driver script importing it compiles it
// into the driver's own unit: its top level runs inside the driver's merged
// top level. Read by tests/headless/test_error_stack_module_file.js.
export const topLevelStack = new Error("module_throws top level").stack;

export function throwsHere() {
  const before = 1;
  return before + missingInModuleThrows;
}
