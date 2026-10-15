// bro.profiler names a hot function of another module that an OSR'd loop
// calls, by name and with self samples, and names the script's own top level
// after the script: as a helmapps test is (it imports the app's modules and
// its top level loops over frames), where every sample is under that loop's
// OSR code. The top level of a script that imports a module the page already
// loaded was labelled after that module's synthesized bindings
// ("lib/icons.js (external module bindings)"), not after the script.

import { crunchInModule } from './hot_module.js';

const supported = (process.platform === 'win32' && process.arch === 'x64') ||
                  (process.platform === 'linux' && process.arch === 'x64') ||
                  (process.platform === 'darwin' && process.arch === 'arm64');
if (!supported) {
    skipTest('no sampling profiler on this platform');
} else {
    bro.profiler.start({ hz: 2000 });
    // One long loop at the top level: run once, it is entered through OSR.
    const until = Date.now() + 600;
    let sink = 0;
    while (Date.now() < until) sink = (sink + crunchInModule(20000)) | 0;
    const p = bro.profiler.stop({ report: true });
    const top = () => p.functions.slice(0, 8).map((f) => `${f.name} [${f.tier}] ${f.self}/${f.total}`).join(', ');

    const hot = p.functions.filter((f) => f.name === 'crunchInModule');
    assert(hot.length > 0, `the other module's hot function is in the profile by name; top: ${top()}`);
    const self = hot.reduce((s, f) => s + f.self, 0);
    assert(self > p.samples / 2, `it owns most self samples: ${self}/${p.samples}; top: ${top()}`);
    assert(hot.every((f) => f.tier !== 'native' && /hot_module\.js$/.test(f.file || '')),
           `as a JS function of hot_module.js: ${JSON.stringify(hot)}`);

    // The script's own top level: by the script's file, whatever it imports.
    const entry = p.functions.filter((f) => /^__bronze_dyn_entry_\d+/.test(f.name));
    assert(entry.length > 0, `the script's top level is in the profile; top: ${top()}`);
    assert(entry.every((f) => /test_profiler_modules\.js$/.test(f.file || '')),
           `the top level is named after the script: ${JSON.stringify(entry.map((f) => [f.name, f.file, f.line]))}`);
    console.log(p.report.split('\n').slice(0, 8).join('\n'));
    console.log(`profiler modules: crunchInModule ${self}/${p.samples} self (sink ${sink})`);
}
