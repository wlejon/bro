// The bro core probe: `bro` and `__bro` on bronze's native mechanism —
// bro.time, bro.appDir / bro.userDataDir / bro.resolvePath, bro.window,
// bro.settings, and the panels' __bro.* — as a COMPILED app reads them.
//
// Every `bro.time.scale` here is an ordinary property read of a plain object
// whose accessor bro_core.js defined over `__bro_native.time.scale`; the
// point of the probe is that the public shape is JavaScript all the way to
// the native, so aliasing, enumeration and typeof behave as a program
// expects. Values that vary by machine (paths, display names, refresh rates)
// are pinned by type, not by value; the settings file is shared by every
// check in this tree, so the probe keeps to its own `probe.*` category and
// clears it on both ends.

function say(label, value) { console.log('APP ' + label + '=' + value); }
function j(v) { return JSON.stringify(v); }

say('roots', typeof bro + ',' + typeof __bro + ',' + typeof bro.time + ',' + typeof bro.settings + ',' + typeof bro.window);

// ---------------------------------------------------------------------------
// bro.time — one engine clock: scale (clamped [0, 100], non-finite ignored)
// and the pause flag.
// ---------------------------------------------------------------------------
say('time.scale.default', bro.time.scale);
bro.time.scale = 0.5;
say('time.scale.set', bro.time.scale);
bro.time.scale = 250;
say('time.scale.clamped', bro.time.scale);
bro.time.scale = NaN;
say('time.scale.nanIgnored', bro.time.scale);
bro.time.scale = 1;
say('time.paused.default', bro.time.paused);
bro.time.paused = true;
say('time.paused.set', bro.time.paused);
bro.time.paused = false;
say('time.now.type', typeof bro.time.now);
const t = bro.time;
say('time.alias', t.scale === 1 && t.paused === false);
say('time.keys', Object.keys(bro.time).sort().join(','));

// ---------------------------------------------------------------------------
// bro.appDir / bro.userDataDir / bro.resolvePath
// ---------------------------------------------------------------------------
say('paths.appDir.type', typeof bro.appDir + ',' + (bro.appDir.length > 0));
say('paths.userDataDir.type', typeof bro.userDataDir + ',' + (bro.userDataDir.length > 0));
say('paths.resolvePath.underAppDir', bro.resolvePath('x.txt').indexOf(bro.appDir) === 0);
say('paths.resolvePath.tail', bro.resolvePath('x.txt').slice(-5));
let pathsErr = null;
try { bro.resolvePath(); } catch (e) { pathsErr = e; }
say('paths.resolvePath.typeError', pathsErr instanceof TypeError);

// ---------------------------------------------------------------------------
// bro.settings — the text store, typed by content on the way out; a custom
// category the engine does not own; actions; the change callback.
// ---------------------------------------------------------------------------
bro.settings.reset('probe');
say('settings.get.absent', bro.settings.get('probe.volume'));

const changes = [];
bro.settings.onChange((category, key) => { changes.push(category + ':' + key); });
let onChangeErr = null;
try { bro.settings.onChange('not a function'); } catch (e) { onChangeErr = e; }
say('settings.onChange.typeError', onChangeErr instanceof TypeError);

bro.settings.set('probe.volume', 0.5);
bro.settings.set('probe.flag', true);
bro.settings.set('probe.name', 'hello');
bro.settings.set('probe.obj', { a: 1, b: 'x' });
say('settings.get.number', typeof bro.settings.get('probe.volume') + ',' + bro.settings.get('probe.volume'));
say('settings.get.bool', typeof bro.settings.get('probe.flag') + ',' + bro.settings.get('probe.flag'));
say('settings.get.string', typeof bro.settings.get('probe.name') + ',' + bro.settings.get('probe.name'));
say('settings.get.object', j(bro.settings.get('probe.obj')));
say('settings.getAll.custom', j(bro.settings.getAll('probe')));
say('settings.getAll.audio.keys', Object.keys(bro.settings.getAll('audio')).sort().join(','));
say('settings.getAll.all.keys', Object.keys(bro.settings.getAll()).sort().join(','));
say('settings.getDefaults.graphics', bro.settings.getDefaults('graphics').width + 'x' + bro.settings.getDefaults('graphics').height);
say('settings.get.engineKey', typeof bro.settings.get('graphics.vsync') + ',' + typeof bro.settings.get('audio.masterVolume') + ',' + typeof bro.settings.get('appearance.colorScheme'));

bro.settings.defineAction('probe_jump', [' ', 'gamepad:south'], { deadzone: 0.25 });
say('settings.getActionKeys', j(bro.settings.getActionKeys('probe_jump')));
say('settings.getKeyAction', bro.settings.getKeyAction(' ') + ',' + bro.settings.getKeyAction('zzz'));
say('settings.getAppActions', j(bro.settings.getAppActions()));
say('settings.getActions.hasEngine', bro.settings.getActions().some((a) => a.action === 'system_toggle_perf'));
say('settings.isActionPressed', bro.settings.isActionPressed('probe_jump'));
say('settings.getActionStrength', bro.settings.getActionStrength('probe_jump'));
bro.settings.rebindAction('probe_jump', ['w']);
say('settings.rebindAction', j(bro.settings.getActionKeys('probe_jump')));
bro.settings.resetAction('probe_jump');
say('settings.resetAction', j(bro.settings.getActionKeys('probe_jump')));
say('settings.getDisplayModes', Array.isArray(bro.settings.getDisplayModes()));

// ---------------------------------------------------------------------------
// bro.window — the hidden headless window's reads; the moves are no-ops.
// ---------------------------------------------------------------------------
say('window.state', bro.window.state);
say('window.flags', bro.window.borderless + ',' + bro.window.alwaysOnTop);
say('window.getMinSize', j(bro.window.getMinSize()));
say('window.getMaxSize', j(bro.window.getMaxSize()));
const pos = bro.window.getPosition();
say('window.getPosition.type', typeof pos.x + ',' + typeof pos.y);
const displays = bro.window.getDisplays();
say('window.getDisplays.nonEmpty', displays.length > 0);
const d0 = displays[0];
say('window.getDisplays.shape', Object.keys(d0).sort().join(','));
say('window.getDisplays.types', typeof d0.id + ',' + typeof d0.name + ',' + typeof d0.width + ',' + typeof d0.isPrimary);
say('window.getDisplays.primary', displays.some((d) => d.isPrimary));
say('window.moveToDisplay', bro.window.moveToDisplay(d0.id));
say('window.methods', ['minimize', 'maximize', 'restore', 'setPosition', 'setMinSize', 'setMaxSize'].map((m) => typeof bro.window[m]).join(','));

// ---------------------------------------------------------------------------
// __bro — what the system panels read.
// ---------------------------------------------------------------------------
say('dunder.perf.types', ['fps', 'frameTime', 'js', 'layout', 'raster', 'gpu', 'draw'].map((k) => typeof __bro.perf[k]).join(','));
say('dunder.perf.windows', j(__bro.perf.windows));
say('dunder.perf.scene', typeof __bro.perf.scene === 'object' ? typeof __bro.perf.scene.meshDrawn : 'absent');
say('dunder.viewport', __bro.viewport.width + 'x' + __bro.viewport.height);
say('dunder.bronze.types', ['heapUsedBytes', 'heapCommittedBytes', 'heapReservedBytes', 'gcCollections', 'gcPauseNs', 'shapeTransitions'].map((k) => typeof __bro.bronze[k]).join(','));
say('dunder.menu.getHeight', __bro.menu.getHeight());
say('dunder.menu.getTree', Array.isArray(__bro.menu.getTree()));
say('dunder.splash.dismiss', typeof __bro.splash.dismiss);
say('dunder.settingsUI.isVisible', __bro.settingsUI.isVisible());
say('dunder.settingsUI.getActivePanel', j(__bro.settingsUI.getActivePanel()));
say('dunder.settingsUI.getViewport', j(__bro.settingsUI.getViewport()));
say('dunder.settingsUI.getSettingsPanels', __bro.settingsUI.getSettingsPanels().map((p) => p.name + '/' + p.label).join(','));
say('dunder.settingsUI.getAllPanels', __bro.settingsUI.getAllPanels().map((p) => p.name).join(','));
say('dunder.inspector.getLayout', j(__bro.inspector.getLayout()));
const tree = __bro.inspector.getAppTree();
say('dunder.inspector.getAppTree', tree.tag + ':' + tree.children.map((c) => c.tag).join(','));
say('dunder.inspector.getAppTree.shape', Object.keys(tree).sort().join(','));
say('dunder.inspector.getAppChildren', __bro.inspector.getAppChildren(tree.children[0].id).map((c) => c.tag).join(','));
say('dunder.inspector.getSelected.none', j(__bro.inspector.getSelected()));
__bro.inspector.select(tree.children[1].id);
say('dunder.inspector.getSelected', __bro.inspector.getSelected().tag);
__bro.inspector.setDock('bottom');
say('dunder.inspector.setDock', __bro.inspector.getLayout().dock);
__bro.inspector.setDock('right');

// The change callback is delivered from the frame seam, never from inside
// the set() that made the change: nothing has arrived yet, and everything
// has by the next frame.
say('settings.onChange.sync', changes.length);
requestAnimationFrame(() => {
    say('settings.onChange.frame', changes.join(','));
    bro.settings.reset('probe');
    say('settings.reset', bro.settings.get('probe.volume') === undefined && bro.settings.get('probe.obj') === undefined);
    say('done', true);
});
