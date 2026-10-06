// Headless test for bro.keys
assert(typeof bro.keys === 'object', 'bro.keys namespace exists');
if (!bro.keys.available) {
    skipTest('bro.keys is compiled out of this build');
} else {
    // 1. Static chord parsing and formatting
    const chord = bro.keys.parseChord('Ctrl+Shift+P');
    assert(chord && chord.key === 'P', 'parseChord extracts key P');
    assert(chord.ctrl === true && chord.shift === true && chord.alt === false, 'parseChord extracts modifiers');

    const formatted = bro.keys.formatChord(chord);
    assert(formatted === 'Ctrl+Shift+P', 'formatChord produces canonical representation');

    const seq = bro.keys.parseSequence('Ctrl+K Ctrl+S');
    assert(Array.isArray(seq) && seq.length === 2, 'parseSequence produces 2 chords');
    assert(bro.keys.formatSequence(seq) === 'Ctrl+K Ctrl+S', 'formatSequence roundtrips');

    // 2. Validate when expressions
    assert(bro.keys.validateWhen('editorTextFocus && !editorReadonly') === true, 'valid when expression passes');
    assert(bro.keys.validateWhen('lineCount > 10') === true, 'numeric when comparison passes');
    assert(bro.keys.validateWhen('((unclosedParen') === false, 'invalid when syntax rejected');

    // 3. Engine creation and bindings
    const engine = new bro.keys.Engine();
    assert(typeof engine === 'object', 'Engine instantiated');

    engine.addBinding({
        key: 'Ctrl+Shift+P',
        command: 'command.palette',
        when: '!terminalFocus'
    });

    engine.addBinding({
        key: 'Ctrl+K Ctrl+S',
        command: 'keyboard.shortcuts'
    });

    // Set context
    engine.setContext('terminalFocus', false);
    assert(engine.getContext('terminalFocus') === false, 'getContext returns boolean false');

    const keydown = (key, code, ctrlKey, shiftKey) => ({
        key, code, ctrlKey, shiftKey, altKey: false, metaKey: false, type: 'keydown'
    });

    // Feed matching key event
    const matchResult = engine.feed(keydown('P', 'KeyP', true, true));
    assert(matchResult.type === 'match', 'feed produces match result');
    assert(matchResult.command === 'command.palette', 'command matches');

    // Feed multi-chord sequence
    const chord1Result = engine.feed(keydown('k', 'KeyK', true, false));
    assert(chord1Result.type === 'pending', 'first chord enters pending state');
    assert(engine.isPendingChord() === true, 'engine reports pending chord');

    const chord2Result = engine.feed(keydown('s', 'KeyS', true, false));
    assert(chord2Result.type === 'match', 'second chord completes match');
    assert(chord2Result.command === 'keyboard.shortcuts', 'multi-chord command matches');
    assert(engine.isPendingChord() === false, 'engine no longer pending');

    // Unhandled key
    const unhandled = engine.feed(keydown('z', 'KeyZ', false, false));
    assert(unhandled.type === 'unhandled', 'unbound key reports unhandled');

    // JSON load
    engine.loadJson(JSON.stringify([
        {
            key: 'ctrl+b',
            command: 'workbench.action.toggleSidebar'
        }
    ]));

    const sidebarResult = engine.feed(keydown('b', 'KeyB', true, false));
    assert(sidebarResult.type === 'match' && sidebarResult.command === 'workbench.action.toggleSidebar', 'json binding matched');

    console.log('test_keys.js PASSED');
}
