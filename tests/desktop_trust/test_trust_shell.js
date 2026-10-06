// Test that an app requesting shell privileges receives them when trusted
// and is denied them when not trusted.

const privilegedNamespaces = ['displays', 'cred', 'seat', 'portal', 'sys', 'compositor', 'wl'];
const isTrusted = typeof bro.appDir === 'string' && bro.appDir.includes('trusted_app');

for (const ns of privilegedNamespaces) {
    assert(typeof bro[ns] === 'object', `bro.${ns} object exists`);
    if (isTrusted) {
        // Granted unless this build compiled it out (Linux-only namespaces on
        // other platforms, or a profile without it).
        const compiledOut = bro[ns].available === false &&
            String(bro[ns].reason).startsWith('this build was compiled without');
        assert(bro[ns].available === true || compiledOut,
               `bro.${ns} is granted to a trusted shell app, got reason: "${bro[ns].reason}"`);
    } else {
        assert(bro[ns].available === false, `bro.${ns}.available is false for untrusted app`);
    }
}

if (isTrusted) {
    console.log('test_trust_shell.js trusted shell check PASSED');
} else {
    console.log('test_trust_shell.js untrusted check PASSED');
}

