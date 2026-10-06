/**
 * @file docs/keys-api.js
 * @summary Documentation and examples for the `bro.keys` JavaScript API.
 */

/**
 * `bro.keys` provides a desktop-grade keybinding and chord dispatch engine:
 * - Chords and multi-chord key sequences (e.g. "Ctrl+K Ctrl+S", "Ctrl+Shift+P")
 * - VS Code-compatible `when` clause context expressions with boolean algebra, comparisons, and regex matching
 * - VS Code keybindings.json import and export
 * - Layout-aware chord parsing and event matching
 *
 * Mounted automatically in Bronze when `BRO_WITH_KEYS` is enabled.
 */

// ============================================================================
// 1. Static Helpers
// ============================================================================

// Parse chord strings into structured objects
const chord = bro.keys.parseChord('Ctrl+Shift+P');
// { key: 'P', modifiers: ['ctrl', 'shift'], ctrl: true, shift: true, alt: false, meta: false }

// Format chord object back into canonical string
const str = bro.keys.formatChord(chord); // 'Ctrl+Shift+P'

// Parse and format multi-chord sequences
const seq = bro.keys.parseSequence('Ctrl+K Ctrl+C');
const seqStr = bro.keys.formatSequence(seq); // 'Ctrl+K Ctrl+C'

// Validate VS Code when expression syntax
const isValid = bro.keys.validateWhen('editorTextFocus && !editorReadonly && lineCount > 10'); // true

// ============================================================================
// 2. Dispatch Engine (`bro.keys.Engine`)
// ============================================================================

const engine = new bro.keys.Engine({
    chordTimeoutMs: 5000 // timeout for multi-chord sequences
});

// Add keybindings
engine.addBinding({
    key: 'Ctrl+Shift+P',
    command: 'workbench.action.showCommands',
    when: '!terminalFocus'
});

engine.addBinding({
    key: 'Ctrl+K Ctrl+S',
    command: 'workbench.action.openGlobalKeybindings'
});

// Import keybindings from JSON string
engine.loadJson(`[
    {
        "key": "ctrl+f",
        "command": "actions.find",
        "when": "editorFocus"
    }
]`);

// Set context state for `when` expressions
engine.setContext('editorFocus', true);
engine.setContext('editorReadonly', false);

// Feed DOM-style KeyboardEvent to the engine
const result = engine.feed({
    key: 'f',
    code: 'KeyF',
    ctrlKey: true,
    shiftKey: false,
    altKey: false,
    metaKey: false,
    type: 'keydown'
});

switch (result.type) {
    case 'match':
        console.log('Executed:', result.command, result.args);
        break;
    case 'pending':
        console.log('Waiting for next chord in sequence:', result.sequence);
        break;
    case 'cancelled':
        console.log('Chord cancelled:', result.reason);
        break;
    case 'unhandled':
        // Pass event through to default handler
        break;
}
