// ============================================================================
// Game UI Demo — app logic
// ============================================================================

(function() {
'use strict';

var root = document.getElementById('root');

// Start on main menu
root.pushScreen('main-menu');

// =========================================================================
// Screen navigation via ui-action events
// =========================================================================

root.addEventListener('ui-action', function(e) {
    var action = e.detail.action;
    console.log('[ui-action] ' + action);

    switch (action) {
        // Main menu
        case 'new-game':    root.pushScreen('new-game'); break;
        case 'settings':    root.pushScreen('settings'); break;
        case 'credits':     root.pushScreen('credits'); break;
        case 'continue':    root.toast('No save data found', 'warning'); break;

        // Quit flow
        case 'quit':
            document.getElementById('quit-dialog').open();
            break;
        case 'confirm-quit':
            root.toast('Goodbye!', 'success');
            break;
        case 'cancel':
            // Dialog handles its own close
            break;

        // New game
        case 'start-game':
            startGame();
            break;

        // Pause menu
        case 'resume':
            root.popScreen();
            break;
        case 'main-menu':
            document.getElementById('main-menu-dialog').open();
            break;
        case 'confirm-main-menu':
            // Pop back to main menu
            while (root._screenStack.length > 1) root.popScreen();
            root.replaceScreen('main-menu');
            break;

        // Theme demo
        case 'theme-dark':     root.applyTheme('dark'); root.toast('Theme: Dark', 'success'); break;
        case 'theme-midnight': root.applyTheme('midnight'); root.toast('Theme: Midnight', 'success'); break;
        case 'theme-ember':    root.applyTheme('ember'); root.toast('Theme: Ember', 'success'); break;
        case 'theme-forest':   root.applyTheme('forest'); root.toast('Theme: Forest', 'success'); break;
        case 'theme-ice':      root.applyTheme('ice'); root.toast('Theme: Ice', 'success'); break;
        case 'back':           root.popScreen(); break;
    }
});

// =========================================================================
// Settings change logging
// =========================================================================

root.addEventListener('ui-change', function(e) {
    var d = e.detail;
    if (d.name) {
        console.log('[setting] ' + d.name + ' = ' + d.value);
    }
});

// =========================================================================
// Tab switching with Tab / Shift+Tab
// =========================================================================

root.addEventListener('keydown', function(e) {
    if (e.key === 'Tab') {
        var tabs = document.getElementById('settings-tabs');
        if (tabs && tabs.parentElement && tabs.parentElement.parentElement &&
            tabs.parentElement.parentElement.hasAttribute('active')) {
            e.preventDefault();
            if (e.shiftKey) tabs.prevTab();
            else tabs.nextTab();
        }
    }
});

// =========================================================================
// Simulated game start / loading screen
// =========================================================================

function startGame() {
    root.pushScreen('loading');

    var progress = document.getElementById('load-progress');
    var detail = document.getElementById('loading-detail');

    var steps = [
        { pct: 10,  text: 'Generating star map...' },
        { pct: 25,  text: 'Placing celestial bodies...' },
        { pct: 40,  text: 'Spawning NPCs...' },
        { pct: 55,  text: 'Compiling shaders...' },
        { pct: 70,  text: 'Loading textures...' },
        { pct: 85,  text: 'Initializing physics...' },
        { pct: 95,  text: 'Preparing warp drive...' },
        { pct: 100, text: 'Ready!' },
    ];

    var stepIdx = 0;
    var interval = setInterval(function() {
        if (stepIdx >= steps.length) {
            clearInterval(interval);
            // "Game" started — show pause menu to demonstrate it
            setTimeout(function() {
                root.replaceScreen('pause-menu');
                root.toast('Press Esc to return to menus', 'success', 4000);
            }, 400);
            return;
        }
        var step = steps[stepIdx];
        progress.setAttribute('value', String(step.pct));
        detail.textContent = step.text;
        stepIdx++;
    }, 300);
}

// =========================================================================
// Theme selector access — double-tap T on main menu
// =========================================================================

var lastT = 0;
root.addEventListener('keydown', function(e) {
    if (e.key === 't' || e.key === 'T') {
        var now = Date.now();
        if (now - lastT < 400) {
            root.pushScreen('theme-demo');
        }
        lastT = now;
    }
});

console.log('Game UI demo loaded! Use arrow keys + Enter to navigate.');
console.log('Double-tap T on any screen to open theme selector.');

})();
