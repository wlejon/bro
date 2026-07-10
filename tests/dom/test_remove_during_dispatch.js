// Regression: an element removed from inside its own event handler must not
// leave a dangling wrapper in __bro_elem_map. A real mouse-driven double-click
// dispatches through the engine's full propagation path; when the handler
// removes (frees) the target subtree, the dispatch keeps unwinding that path
// and re-wraps the now-freed target. wrapElement() must refuse to resurrect a
// doomed (unowned) node into the map — otherwise the entry outlives the node
// and the next sweepOrphanedWrappers() dereferences freed memory and crashes
// (seen as a double-click crash in the OpenRouter model picker).

const root = document.getElementById('root');

function keyCount() {
    return globalThis.__bro_elem_map
        ? Object.keys(globalThis.__bro_elem_map).length
        : 0;
}

function openOverlay() {
    const overlay = document.createElement('div');
    overlay.id = 'ov';
    overlay.style.cssText = 'position:fixed;left:0;top:0;width:400px;';
    for (let i = 0; i < 20; i++) {
        const row = document.createElement('div');
        row.className = 'row';
        row.style.cssText = 'height:20px;';
        // Inner element structure, like the picker's rows.
        row.innerHTML = '<span class="id">row-' + i + '</span>';
        row.addEventListener('click', () => { row.classList.add('sel'); });
        row.addEventListener('dblclick', () => { overlay.remove(); });
        overlay.appendChild(row);
    }
    root.appendChild(overlay);
    return overlay;
}

// Baseline map size (wrappers for the static test-app DOM).
flush();
const baseline = keyCount();

// Several open → real-double-click-to-remove cycles. The map size is sampled
// right after each dblclick and BEFORE any periodic sweep runs — the source
// leak (wrapElement resurrecting the freed target) shows up here as unbounded
// growth; the sweep backstop would otherwise mask it by cleaning up later.
for (let c = 0; c < 5; c++) {
    openOverlay();
    flush();
    const row = document.querySelector('#ov .row');
    assert(row !== null, 'cycle ' + c + ': a row exists to click');
    const r = row.getBoundingClientRect();
    const cx = r.left + r.width / 2;
    const cy = r.top + r.height / 2;

    mouseMove(cx, cy);
    // Two clicks at the same spot within the double-click window -> dblclick,
    // whose handler removes the overlay (freeing this very row) mid-dispatch.
    click(cx, cy);
    click(cx, cy);
    flush();

    assert(document.getElementById('ov') === null,
        'cycle ' + c + ': overlay removed by dblclick');

    // The freed overlay + rows + inner spans must have had their wrappers
    // dropped — nothing resurrected. If the freed target were re-wrapped into
    // the map, this count would climb by one (or more) every cycle.
    const now = keyCount();
    assert(now <= baseline + 2,
        'cycle ' + c + ': no dangling wrappers retained after remove-during-' +
        'dispatch (baseline=' + baseline + ', now=' + now + ')');
}

// The periodic sweep must also survive iterating the map without faulting on
// any stale pointer (the debug-build crash site).
advanceTime(1500);
advanceTime(1500);
flush();

console.log('PASS: remove-during-dispatch leaves no dangling wrappers ' +
    '(baseline=' + baseline + ', final=' + keyCount() + ')');
