// Error.stack names the file each position is in, across module imports.
//
// A driver script and the modules it imports compile into one program whose
// top level holds every module's top-level statements. Its frames all named
// ONE file (the first import's) with the position's own line, so a
// ReferenceError at line 31 of this script read `.../module_throws.js:31`.
// And an imported file's functions were named by the linker's rename of their
// binding (`mod1.throwsHere`) rather than as the source spelled them.
import { instance } from "/app/module_instance.js";
import { topLevelStack, throwsHere } from "/app/module_throws.js";

function frames(stack) {
  const out = [];
  for (const line of stack.split("\n").slice(1)) {
    const m = line.match(/([A-Za-z_]+\.js):(\d+):\d+\)?$/);
    if (!m) continue;
    const named = line.match(/at (\S+) \(/);
    out.push((named ? named[1] : "<top>") + "@" + m[1] + ":" + m[2]);
  }
  return out;
}

assert(instance && typeof instance.bumps === "number", "the page module is shared");

const imported = frames(topLevelStack);
assert(imported[0] === "<top>@module_throws.js:4",
       "an imported module's top level names its own file: " + imported.join(","));

let own = null;
try {
  notDefinedAnywhere(1);
} catch (e) {
  own = frames(e.stack);
}
assert(own && own[0] === "<top>@test_error_stack_module_file.js:31",
       "this script's top level names this script: " + (own && own.join(",")));

let nested = null;
try {
  throwsHere();
} catch (e) {
  nested = frames(e.stack);
}
assert(nested && nested[0] === "throwsHere@module_throws.js:8" &&
         nested[1] === "<top>@test_error_stack_module_file.js:40",
       "a frame per file, each with its own name: " + (nested && nested.join(",")));
assert(throwsHere.name === "throwsHere", "an imported function's name: " + throwsHere.name);
