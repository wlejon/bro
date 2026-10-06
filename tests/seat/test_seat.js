// Headless test for bro.seat
assert(typeof bro.seat === 'object', 'bro.seat namespace exists');
if (!bro.seat.available) {
    skipTest('bro.seat is unavailable in this environment');
} else {
    // 1. Session state
    const session = bro.seat.getSessionState();
    assert(typeof session === 'object' && session !== null, 'getSessionState returns object');
    assert(typeof session.active === 'boolean', 'session.active is boolean');
    assert(typeof session.locked === 'boolean', 'session.locked is boolean');
    assert(typeof session.id === 'string', 'session.id is string');

    // 2. Lock & Unlock: not called. They lock the real session of whoever
    // runs the tests; broseat's own tests cover them on a private bus.
    assert(typeof bro.seat.lock === 'function', 'lock is function');
    assert(typeof bro.seat.unlock === 'function', 'unlock is function');

    // 3. Inhibitors (an idle inhibitor held by this process, released below
    // and with the process)
    const inhId = bro.seat.inhibit('idle', 'Automated test inhibition');
    assert(typeof inhId === 'number', 'inhibit returns number');

    const inhibitors = bro.seat.listInhibitors();
    assert(Array.isArray(inhibitors), 'listInhibitors returns array');
    if (inhId > 0) {
        assert(inhibitors.some(i => i.id === inhId), 'new inhibitor appears in list');
        const uninhRes = bro.seat.uninhibit(inhId);
        assert(uninhRes === true, 'uninhibit returns true');
    }

    // 4. Autostart
    const autostart = bro.seat.listAutostart();
    assert(Array.isArray(autostart), 'listAutostart returns array');

    // runAutostart launches every autostart entry of the real account, so it
    // is not called here.
    assert(typeof bro.seat.runAutostart === 'function', 'runAutostart is function');

    console.log('test_seat.js PASSED');
}
