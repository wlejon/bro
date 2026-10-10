// Child process for tests/conf/test_conf_write_cost.js: a second bro on the
// same BRO_APP_HOME. It sets a burst of values and exits without calling
// bro.conf.flush(), so what the parent then reads from the settings file is
// what engine teardown persisted.
const n = Number(process.env.BRO_TEST_CONF_CHILD_N || 5);
for (let i = 1; i <= n; ++i) {
    bro.conf.set('perf.ui.child', 'from-child-' + i);
}
assert(bro.conf.get('perf.ui.child') === 'from-child-' + n, 'the child reads its own last set');
