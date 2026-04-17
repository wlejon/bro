// Tests for apps/lib/history.js.
//
// Run: bro-headless apps/lib-tests apps/lib-tests/test_history.js

'use strict';

let tests = 0, failed = 0;
function t(name, fn) {
    tests++;
    try { fn(); console.log('  ok   ' + name); }
    catch (e) {
        failed++;
        console.log('  FAIL ' + name + ': ' + (e && e.message ? e.message : e));
        if (e && e.stack) console.log(e.stack);
    }
}
function eq(a, b, msg) {
    const ja = JSON.stringify(a), jb = JSON.stringify(b);
    if (ja !== jb) throw new Error((msg || 'eq') + ': ' + ja + ' !== ' + jb);
}
function truthy(v, msg) { if (!v) throw new Error(msg || 'expected truthy'); }
function falsy(v, msg)  { if (v)  throw new Error(msg || 'expected falsy'); }

// ---------- basic do / undo / redo ----------

t('do applies immediately, undo reverts, redo reapplies', () => {
    const h = new History();
    let x = 0;
    h.do('set x=1', () => x = 1, () => x = 0);
    eq(x, 1);
    eq(h.canUndo(), true);
    eq(h.canRedo(), false);
    h.undo();
    eq(x, 0);
    eq(h.canUndo(), false);
    eq(h.canRedo(), true);
    h.redo();
    eq(x, 1);
});

t('record without execute — caller already applied', () => {
    const h = new History();
    let x = 5;
    x = 7;
    h.record('set 7', () => x = 7, () => x = 5);
    eq(x, 7);
    h.undo(); eq(x, 5);
    h.redo(); eq(x, 7);
});

t('multiple commands: undo all, redo all', () => {
    const h = new History();
    let x = 0;
    h.do('+1', () => x += 1, () => x -= 1);
    h.do('+1', () => x += 1, () => x -= 1);
    h.do('+1', () => x += 1, () => x -= 1);
    eq(x, 3);
    eq(h.size(), 3);
    h.undo(); h.undo(); h.undo();
    eq(x, 0);
    eq(h.canUndo(), false);
    h.redo(); h.redo(); h.redo();
    eq(x, 3);
});

t('new record clears future (redo invalidated)', () => {
    const h = new History();
    let x = 0;
    h.do('+1', () => x += 1, () => x -= 1);
    h.undo(); eq(x, 0);
    eq(h.canRedo(), true);
    h.do('+10', () => x += 10, () => x -= 10);
    eq(h.canRedo(), false);
});

t('undo on empty stack returns false', () => {
    const h = new History();
    eq(h.undo(), false);
    eq(h.redo(), false);
});

t('clear empties both stacks', () => {
    const h = new History();
    let x = 0;
    h.do('a', () => x = 1, () => x = 0);
    h.clear();
    eq(h.canUndo(), false);
    eq(h.canRedo(), false);
    eq(h.size(), 0);
});

// ---------- limit ----------

t('limit evicts oldest entries', () => {
    const h = new History({ limit: 3 });
    for (let i = 0; i < 5; i++) h.record('e' + i, () => {}, () => {});
    eq(h.size(), 3);
    const labels = h.entries().map(e => e.label);
    eq(labels, ['e2', 'e3', 'e4']);
});

// ---------- transactions ----------

t('transaction groups N commands into one undo', () => {
    const h = new History();
    let a = 0, b = 0;
    h.transaction('pair', () => {
        h.do('a', () => a = 1, () => a = 0);
        h.do('b', () => b = 1, () => b = 0);
    });
    eq(h.size(), 1);
    eq(a, 1); eq(b, 1);
    h.undo();
    eq(a, 0); eq(b, 0);
    h.redo();
    eq(a, 1); eq(b, 1);
});

t('transaction children undo in reverse order', () => {
    const h = new History();
    const log = [];
    h.transaction('g', () => {
        h.do('1', () => log.push('do1'), () => log.push('undo1'));
        h.do('2', () => log.push('do2'), () => log.push('undo2'));
        h.do('3', () => log.push('do3'), () => log.push('undo3'));
    });
    eq(log, ['do1', 'do2', 'do3']);
    h.undo();
    eq(log, ['do1', 'do2', 'do3', 'undo3', 'undo2', 'undo1']);
});

t('single-child transaction collapses to the child', () => {
    const h = new History();
    let x = 0;
    h.transaction('solo', () => {
        h.do('a', () => x = 1, () => x = 0);
    });
    // The recorded entry is the child cmd ("a"), not a group.
    eq(h.entries()[0].kind, 'cmd');
    eq(h.entries()[0].label, 'a');
});

t('empty transaction records nothing', () => {
    const h = new History();
    h.transaction('empty', () => {});
    eq(h.size(), 0);
});

t('transaction throw aborts without recording', () => {
    const h = new History();
    let x = 0;
    try {
        h.transaction('bad', () => {
            h.do('a', () => x = 1, () => x = 0);
            throw new Error('boom');
        });
    } catch (e) { /* swallowed */ }
    // The mutation happened, but nothing was recorded — the caller is
    // responsible for rolling back on throw. Our contract is "no history
    // entry on abort", which this asserts.
    eq(h.size(), 0);
});

t('nested transactions', () => {
    const h = new History();
    const log = [];
    h.transaction('outer', () => {
        h.do('a', () => log.push('do-a'), () => log.push('un-a'));
        h.transaction('inner', () => {
            h.do('b', () => log.push('do-b'), () => log.push('un-b'));
            h.do('c', () => log.push('do-c'), () => log.push('un-c'));
        });
        h.do('d', () => log.push('do-d'), () => log.push('un-d'));
    });
    eq(h.size(), 1);
    h.undo();
    // Reverse traversal of the tree: d, [inner: c, b], a.
    eq(log.slice(4), ['un-d', 'un-c', 'un-b', 'un-a']);
});

// ---------- coalescing ----------

t('coalesce merges consecutive matching entries', () => {
    const h = new History();
    let pos = 0;
    const start = pos;
    h.coalesce((a, b) => a.label === b.label && a.label === 'drag');
    // 5 simulated drag frames.
    for (let i = 1; i <= 5; i++) {
        const from = pos, to = i;
        h.record('drag', () => pos = to, () => pos = from);
        pos = to;
    }
    h.endCoalesce();
    eq(pos, 5);
    eq(h.size(), 1);    // all 5 merged into one
    h.undo();
    eq(pos, start);     // undoFn still points at original start
    h.redo();
    eq(pos, 5);         // doFn is the final frame's
});

t('coalesce respects predicate — non-matching entries stay separate', () => {
    const h = new History();
    h.coalesce((a, b) => a.label === b.label);
    h.record('a', () => {}, () => {});
    h.record('a', () => {}, () => {});  // merges
    h.record('b', () => {}, () => {});  // new entry
    h.record('b', () => {}, () => {});  // merges with prev b
    eq(h.size(), 2);
});

// ---------- marks / rewindTo ----------

t('mark + rewindTo', () => {
    const h = new History();
    let x = 0;
    h.do('a', () => x = 1, () => x = 0);
    h.mark('saved');
    h.do('b', () => x = 2, () => x = 1);
    h.do('c', () => x = 3, () => x = 2);
    eq(x, 3);
    const n = h.rewindTo('saved');
    eq(n, 2);
    eq(x, 1);
    eq(h.size(), 1);
});

t('rewindTo unknown mark returns false', () => {
    const h = new History();
    eq(h.rewindTo('nope'), false);
});

// ---------- events ----------

t('change / record / undo / redo events', () => {
    const h = new History();
    const seen = [];
    h.on('record', e => seen.push(['record', e.label]));
    h.on('undo',   e => seen.push(['undo',   e.label]));
    h.on('redo',   e => seen.push(['redo',   e.label]));
    h.on('change', () => seen.push(['change']));
    let x = 0;
    h.do('a', () => x = 1, () => x = 0);
    h.undo();
    h.redo();
    // Record emits 'record' + 'change'; undo/redo each emit their event +
    // 'change'. Order: record, change, undo, change, redo, change.
    eq(seen, [
        ['record', 'a'], ['change'],
        ['undo',   'a'], ['change'],
        ['redo',   'a'], ['change'],
    ]);
});

t('off() removes listener', () => {
    const h = new History();
    let n = 0;
    const fn = () => n++;
    h.on('change', fn);
    h.do('a', () => {}, () => {});
    h.off('change', fn);
    h.do('b', () => {}, () => {});
    eq(n, 1);
});

t('on() returns unsubscribe', () => {
    const h = new History();
    let n = 0;
    const unsub = h.on('change', () => n++);
    h.do('a', () => {}, () => {});
    unsub();
    h.do('b', () => {}, () => {});
    eq(n, 1);
});

t('onChange option', () => {
    let n = 0;
    const h = new History({ onChange: () => n++ });
    h.do('a', () => {}, () => {});
    eq(n, 1);
});

// ---------- no recording during undo/redo ----------

t('record() called during undoFn is ignored', () => {
    const h = new History();
    let x = 0;
    h.record('weird',
        () => { x = 1; h.record('nope', () => {}, () => {}); },
        () => { x = 0; h.record('nope', () => {}, () => {}); });
    h.redo = h.redo; // no-op; just asserting shape
    eq(h.size(), 1);
    // Execute the doFn/undoFn paths via redo/undo cycle.
    // Starts with nothing in future — do redo path is n/a; trigger via undo.
    h.undo();   // runs undoFn which calls record() — should be ignored
    eq(h.size(), 0, 'past was drained by undo');
});

// ---------- wrap ----------

t('wrap records setter writes', () => {
    const h = new History();
    const obj = { color: 'red' };
    const unwrap = h.wrap(obj, 'color');
    obj.color = 'green';
    obj.color = 'blue';
    eq(obj.color, 'blue');
    eq(h.size(), 2);
    h.undo(); eq(obj.color, 'green');
    h.undo(); eq(obj.color, 'red');
    h.redo(); eq(obj.color, 'green');
    unwrap();
    // After unwrap, assignments are plain — no recording.
    obj.color = 'purple';
    eq(h.size(), 1);   // the 'green' entry from past still there
});

t('wrap + custom label', () => {
    const h = new History();
    const obj = { x: 0 };
    h.wrap(obj, 'x', 'Set X');
    obj.x = 10;
    eq(h.entries()[0].label, 'Set X');
});

// ---------- snapshot ----------

t('snapshot captures before/after', () => {
    const h = new History();
    const state = { items: ['a', 'b'] };
    h.snapshot('Rebuild',
        () => state.items,
        s => { state.items = s; },
        () => { state.items.push('c'); state.items.push('d'); });
    eq(state.items, ['a', 'b', 'c', 'd']);
    h.undo();
    eq(state.items, ['a', 'b']);
    h.redo();
    eq(state.items, ['a', 'b', 'c', 'd']);
});

t('snapshot clones defensively — mutation after record does not bleed', () => {
    const h = new History();
    const state = { v: [1, 2, 3] };
    h.snapshot('reset',
        () => state.v,
        s => { state.v = s; },
        () => { state.v = [9]; });
    state.v.push(42);           // mutate live — history snapshots must be frozen
    h.undo();
    eq(state.v, [1, 2, 3]);
    h.redo();
    eq(state.v, [9]);
});

// ---------- entries() ----------

t('entries() returns ordered metadata oldest → newest', () => {
    const h = new History();
    h.record('a', () => {}, () => {});
    h.record('b', () => {}, () => {});
    h.record('c', () => {}, () => {});
    const labels = h.entries().map(e => e.label);
    eq(labels, ['a', 'b', 'c']);
});

t('entries() describes groups', () => {
    const h = new History();
    h.transaction('pair', () => {
        h.do('a', () => {}, () => {});
        h.do('b', () => {}, () => {});
    });
    const e = h.entries()[0];
    eq(e.kind, 'group');
    eq(e.count, 2);
});

// ---------- end ----------

console.log(`\n${tests - failed}/${tests} passed`);
if (failed > 0) throw new Error(`${failed} test(s) failed`);
