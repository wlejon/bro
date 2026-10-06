// Test for privileged desktop trust model
const privilegedNamespaces = ['displays', 'cred', 'seat', 'portal', 'sys', 'compositor', 'wl'];

for (const ns of privilegedNamespaces) {
    assert(typeof bro[ns] === 'object', `bro.${ns} object exists`);
    assert(bro[ns].available === false, `bro.${ns}.available is false for untrusted app`);
    assert(typeof bro[ns].reason === 'string' && bro[ns].reason.length > 0,
           `bro.${ns}.reason says why`);

    let threw = false;
    let errMsg = '';
    try {
        bro[ns].getSnapshot();
    } catch (e) {
        threw = true;
        errMsg = e.message;
    }
    assert(threw, `calling into bro.${ns} threw`);
    assert(errMsg.includes(bro[ns].reason),
           `error for bro.${ns} carries the reason, got: "${errMsg}"`);
}

// Ordinary namespaces are not gated: each is available unless this build
// compiled it out.
for (const ns of ['apps', 'vfs', 'keys', 'themes', 'search', 'thumb', 'conf']) {
    const n = bro[ns];
    if (n.available === false) {
        assert(n.reason.startsWith('this build was compiled without'),
               `bro.${ns} is unavailable only because it is compiled out, got: "${n.reason}"`);
    } else {
        assert(n.available === true, `bro.${ns} is available to an untrusted app`);
    }
}

// A trusted shell app receives the privileged namespaces.
const cp = require('child_process');
const fs = require('fs');
const candidates = [
    process.env.BRO_HEADLESS,
    './build/Release/bro-headless.exe',
    './build/bro-headless',
    './build-release/bro-headless',
].filter(Boolean);
const headlessBin = candidates.find(p => fs.existsSync(p));
assert(headlessBin, `a bro-headless binary to run the trusted app (tried ${candidates.join(', ')})`);

const out = cp.execFileSync(headlessBin, [
    'tests/desktop_trust/trusted_app',
    '--test',
    'tests/desktop_trust/test_trust_shell.js'
], {
    env: { ...process.env, BRO_TRUSTED: '1' },
    encoding: 'utf8'
});
assert(out.includes('test_trust_shell.js trusted shell check PASSED'),
       `trusted shell check passed, got: ${out}`);

console.log('test_trust.js PASSED');
