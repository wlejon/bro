// process.env carries any Unicode value: set here, read here, and seen by a
// child, whether passed in options.env or inherited. On Windows the
// environment is UTF-16; reading it through the ANSI APIs turned "×4" into
// the bytes D7 34 and a CJK or astral character into '?'.
const { spawnSync } = require('child_process');

const VALUE = 'a ×4 é 日本 🐦';
const codes = (s) => [...s].map((c) => c.codePointAt(0).toString(16)).join(' ');
const want = codes(VALUE);
const same = (got, what) => assert(got === want, `${what}: got [${got}], want [${want}]`);

process.env.BRO_TEST_ENV_UNICODE = VALUE;
same(codes(process.env.BRO_TEST_ENV_UNICODE || ''), 'read back what was set');
assert(Object.keys(process.env).includes('BRO_TEST_ENV_UNICODE'), 'the key is listed');

process.env['BRO_TEST_ENV_ÜNÏ'] = 'key';
assert(Object.keys(process.env).includes('BRO_TEST_ENV_ÜNÏ'), 'a non-ASCII key is listed as itself');
assert(process.env['BRO_TEST_ENV_ÜNÏ'] === 'key', 'and reads by it');
delete process.env['BRO_TEST_ENV_ÜNÏ'];

// What a child sees: it prints the code points in ASCII, so the pipe's
// encoding cannot change the answer.
function childSees(name, env) {
    const expr = `console.log('CODES[' + [...(process.env.${name} || '')].map((c) => c.codePointAt(0).toString(16)).join(' ') + ']')`;
    const r = spawnSync(process.execPath, [bro.appDir, '-e', expr], { encoding: 'utf8', timeout: 60000, env });
    const m = /CODES\[([^\]]*)\]/.exec(`${r.stdout || ''}${r.stderr || ''}`);
    assert(m, `the child ran and printed its codes (status ${r.status})`);
    return m ? m[1] : '';
}

same(childSees('BRO_TEST_ENV_UNICODE'), 'a child inherits the value');
same(childSees('BRO_TEST_ENV_PASSED', { ...process.env, BRO_TEST_ENV_PASSED: VALUE }),
     'a child given the value in options.env reads it');

delete process.env.BRO_TEST_ENV_UNICODE;
assert(process.env.BRO_TEST_ENV_UNICODE === undefined, 'deleted');
