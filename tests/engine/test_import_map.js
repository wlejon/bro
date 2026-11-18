// A page's <script type="importmap"> (app_loader.cpp, eval.cpp
// moduleRootsFor): a bare specifier mapped to a file, a prefix ("wildpkg/")
// mapped to a folder, for static and dynamic import; a scope's entries
// (applied page-wide when no top-level key is remapped); targets and the
// module's own relative imports resolving against <base href>; and a script
// in a comment (an IE conditional comment, a commented-out tag) not running.

const el = document.createElement('iframe');
el.setAttribute('src', 'importmap_child');
document.body.appendChild(el);
flush();
advanceTime(16);

const cw = el.contentWindow;
assert(cw, 'frame loaded');
assert(cw.__result === 'x,y,scoped,base,y', 'imports resolved through the import map, got ' + cw.__result);
assert(cw.__ieRan === undefined && cw.__commentRan === undefined, 'scripts in comments did not run');
assert(cw.__inlineStyle === '<!-- not a comment inside a script -->', 'a script\'s text keeps its "<!--"');

console.log('test_import_map: OK');
