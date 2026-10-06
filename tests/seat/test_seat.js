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

    // 2. Lock & Unlock
    const lockRes = bro.seat.lock();
    assert(typeof lockRes === 'boolean', 'lock returns boolean');

    const unlockRes = bro.seat.unlock();
    assert(typeof unlockRes === 'boolean', 'unlock returns boolean');

    // 3. Inhibitors
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

    const runPromise = bro.seat.runAutostart();
    assert(typeof runPromise === 'object' && typeof runPromise.then === 'function', 'runAutostart returns Promise');
    runPromise.catch(() => {});

    console.log('test_seat.js PASSED');
}
