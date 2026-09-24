// A page's <script type="module" src="module_sub/main.js"> is its own module:
// import.meta.url names that file and its relative imports resolve from its
// directory. Page module scripts used to be compiled into one program under
// index.html's name, so both answered for the app root.

const m = window.__moduleSub;
assert(m && typeof m === 'object', 'module_sub/main.js ran');
assert(/module_sub\/main\.js$/.test(m.metaUrl), 'import.meta.url names the module file, got ' + m.metaUrl);
assert(m.helper === 'module_sub/helper.js', "'./helper.js' resolved next to the module, got " + m.helper);
assert(m.leaked === false, 'module-scope bindings stay out of the global scope');
assert(m.classicRanFirst === true, 'the classic scripts ran before the (deferred) module');
