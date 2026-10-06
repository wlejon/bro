// Headless test for bro.a11y
assert(typeof bro.a11y === 'object', 'bro.a11y namespace exists');
if (!bro.a11y.available) {
    skipTest('bro.a11y is compiled out of this build');
} else {
    // 1. Announcements
    assert(typeof bro.a11y.announce === 'function', 'announce is function');
    const announced = bro.a11y.announce('Test announcement', { priority: 'polite' });
    assert(typeof announced === 'boolean', 'announce returns boolean');

    // 2. Tree inspection
    assert(typeof bro.a11y.getRootNode === 'function', 'getRootNode is function');
    const root = bro.a11y.getRootNode();
    assert(root === null || typeof root === 'object', 'getRootNode returns null or object');

    // 3. Custom roles
    assert(typeof bro.a11y.registerCustomRole === 'function', 'registerCustomRole is function');
    assert(typeof bro.a11y.getCustomRoles === 'function', 'getCustomRoles is function');
    const regOk = bro.a11y.registerCustomRole('customWidget', {
        baseRole: 'button',
        description: 'Custom widget',
    });
    assert(regOk === true, 'registerCustomRole returns true');
    const customRoles = bro.a11y.getCustomRoles();
    assert(Array.isArray(customRoles), 'getCustomRoles returns array');
    assert(customRoles.some(r => r.name === 'customWidget'), 'customWidget in customRoles');
    assert(bro.a11y.hasCustomRole('customWidget') === true, 'hasCustomRole returns true');

    // 4. Events subscription
    let receivedAnnounce = false;
    const handle = bro.a11y.on('announce', (ev) => {
        receivedAnnounce = true;
    });
    assert(typeof handle === 'object' && handle !== null, 'on returns handle');
    assert(typeof handle.remove === 'function', 'handle has remove method');
    handle.remove();

    console.log('test_a11y.js PASSED');
}
