#pragma once
// Script elements inserted by script run (host_inserted_scripts.cpp): an
// inline classic script at once, one with a src on a later task, then `load`
// or `error`.

namespace bro::dom { class Element; }

namespace bro::bronze_host {

// `root` was just connected (hostInsertNode): run every classic script in it,
// root included, that has not started.
void runInsertedScripts(dom::Element* root);

}  // namespace bro::bronze_host
