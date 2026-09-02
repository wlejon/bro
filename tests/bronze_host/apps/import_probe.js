// A non-constant `import()` in COMPILED code, answered by the page's QuickJS
// realm (src/bronze_host/host_interp.h, the third seam). bronze resolves
// every import whose specifier it can read at compile time into the compiled
// graph; one it cannot — built from a variable, as below — reaches the
// runtime's dynamic-import host, and bro's answer is the module loader the
// page's own `<script type=module>` goes through: the specifier resolves
// against the importer's `import.meta.url`, the module loads and evaluates
// in the interpreted realm, and its namespace comes back wrapped.
//
// Every line is `APP <name>=<value>`, every expectation derived from the
// module beside this file before the first run. What only this check
// catches: a specifier resolved against the wrong base (the app dir, the
// working directory), a namespace that comes back copied rather than live,
// and a failed load that resolves to undefined instead of rejecting.

function say(label, value) {
    console.log('APP ' + label + '=' + value);
}

// Assembled at run time so the compiler cannot fold it into the graph.
const parts = ['import_mods', 'greeter'];
const spec = './' + parts.join('/') + '.js';

let first = null;

import(spec)
    .then(function (ns) {
        first = ns;
        say('nsType', typeof ns);
        say('greeting', ns.greeting);
        say('greet', ns.greet('bronze'));
        say('default', ns.default);
        // The loader caches by normalized path, so a second import of the
        // same spelling is the same namespace — and the bridge's identity
        // table hands back the same wrapper.
        return import('./' + parts.join('/') + '.js');
    })
    .then(function (again) {
        say('sameNamespace', again === first);
        // A file that is not there rejects with an Error naming the loader's
        // own complaint, and never resolves.
        return import('./' + parts[0] + '/nope.js').then(
            function () { say('missingRejected', false); },
            function (e) {
                say('missingRejected', true);
                say('missingIsError', e instanceof Error);
                say('missingMessage', String(e.message).indexOf('could not load module') >= 0);
            });
    })
    .then(function () {
        say('done', 'true');
    });
