// screens.js — screen/scene state machine with menu navigation.
//
// Distilled from apps/blockfall/screens.js. Each "screen" is a plain
// object with lifecycle hooks; the manager routes key + mouse events and
// handles the usual arrow-up / arrow-down / Enter / Escape menu flow.
//
// HTML convention (matches blockfall / others):
//   <div id="overlay">
//       <div id="screen-title">
//           <div class="menu-items">
//               <div class="menu-item">Play</div>
//               <div class="menu-item">Quit</div>
//           </div>
//       </div>
//       <div id="screen-gameover"> ... </div>
//   </div>
//
// Usage:
//   const S = Screens.create({ overlay: "#overlay" });
//   S.define("title", {
//       enter(ctx)   { /* show HUD, init */ },
//       exit()       {},
//       keydown(key) { S.menuNav("title", key, (idx) => switchFn(idx)); },
//   });
//   S.switchTo("title");
//   window.addEventListener("keydown", (e) => S.keydown(e.key));
//
// Optional shared background — many apps run the same animated bg on
// title/howto/gameover. Pass it once and the manager runs it for the
// listed screens (before each screen's own update/draw):
//
//   Screens.create({
//       backgroundScreens: ['title', 'howto', 'gameover'],
//       backgroundInit:    (W, H) => initStarfield(W, H),
//       backgroundUpdate:  (dt, W, H) => stepStarfield(dt, W, H),
//       backgroundDraw:    (ctx, W, H) => drawStarfield(ctx, W, H),
//   });
//
// Optional HUD auto-toggle — show a HUD element only on listed screens
// (default ['playing']):
//
//   Screens.create({ hudSelector: '#hud', hudFor: ['playing', 'paused'] });

(function (global) {
    'use strict';

    function create(opts) {
        opts = opts || {};
        const overlaySel = opts.overlay || '#overlay';
        const prefix     = opts.prefix  || 'screen-';
        const itemClass  = opts.itemClass || 'menu-item';
        const itemsSel   = opts.itemsSelector || '.menu-items';
        const onMenuMove   = opts.onMenuMove   || null; // () => SFX
        const onMenuSelect = opts.onMenuSelect || null;

        // Optional shared animated background for one or more screens.
        // The manager calls backgroundUpdate/backgroundDraw automatically
        // before the active screen's own update/draw, so callers don't
        // have to wire the same code into every title/howto/gameover.
        const bgScreens     = opts.backgroundScreens || null;
        const bgInit        = opts.backgroundInit    || null;
        const bgUpdate      = opts.backgroundUpdate  || null;
        const bgDraw        = opts.backgroundDraw    || null;
        let   bgInited      = false;

        // Optional HUD auto-toggle. If hudSelector is set, the manager
        // shows it on screens listed in hudFor (default ['playing']) and
        // hides it on all others.
        const hudSelector   = opts.hudSelector || null;
        const hudFor        = opts.hudFor || ['playing'];
        let   hudEl         = null;

        let overlayEl = null;
        let current   = null;
        let currentName = '';
        let activeScreenId = '';
        let menuIndex = 0;
        let mouseAttached = false;

        function getOverlay() {
            if (!overlayEl) {
                overlayEl = (overlaySel instanceof Element)
                    ? overlaySel : document.querySelector(overlaySel);
            }
            return overlayEl;
        }

        function showOverlay(screenId) {
            const ov = getOverlay();
            if (!ov) return;
            for (let i = 0; i < ov.children.length; i++) {
                ov.children[i].style.display = 'none';
            }
            const el = document.getElementById(prefix + screenId);
            if (el) el.style.display = 'block';
            ov.style.display = '';
            activeScreenId = screenId;
        }

        function hideOverlay() {
            const ov = getOverlay();
            if (ov) ov.style.display = 'none';
            activeScreenId = '';
        }

        function getMenuItems(screenId) {
            const el = document.getElementById(prefix + screenId);
            if (!el) return [];
            const out = [];
            const containers = el.querySelectorAll(itemsSel);
            for (const c of containers) {
                for (const child of c.children) {
                    if (child.classList.contains(itemClass) &&
                        !child.classList.contains('disabled')) {
                        out.push(child);
                    }
                }
            }
            return out;
        }

        function updateSelection(screenId) {
            const items = getMenuItems(screenId);
            for (let i = 0; i < items.length; i++) {
                items[i].classList.toggle('selected', i === menuIndex);
            }
        }

        function setMenuIndex(i) {
            menuIndex = i;
            if (activeScreenId) updateSelection(activeScreenId);
        }

        function menuNav(screenId, key, onSelect, extra) {
            extra = extra || {};
            const items = getMenuItems(screenId);
            if (key === 'ArrowUp') {
                if (items.length) {
                    menuIndex = (menuIndex - 1 + items.length) % items.length;
                    updateSelection(screenId);
                    if (onMenuMove) onMenuMove();
                }
            } else if (key === 'ArrowDown') {
                if (items.length) {
                    menuIndex = (menuIndex + 1) % items.length;
                    updateSelection(screenId);
                    if (onMenuMove) onMenuMove();
                }
            } else if (key === 'Enter' || key === ' ') {
                if (onMenuSelect) onMenuSelect();
                if (onSelect) onSelect(menuIndex, items[menuIndex]);
            } else if (key === 'ArrowLeft' && extra.onAdjust) {
                extra.onAdjust(-1);
            } else if (key === 'ArrowRight' && extra.onAdjust) {
                extra.onAdjust(1);
            } else if (key === 'Escape' && extra.onBack) {
                extra.onBack();
            }
        }

        function ensureMouse() {
            if (mouseAttached) return;
            const ov = getOverlay();
            if (!ov) return;
            mouseAttached = true;
            ov.addEventListener('mousemove', (e) => {
                if (!activeScreenId) return;
                const t = findItem(e.target, ov);
                if (!t) return;
                const items = getMenuItems(activeScreenId);
                const i = items.indexOf(t);
                if (i >= 0 && menuIndex !== i) {
                    menuIndex = i;
                    updateSelection(activeScreenId);
                    if (onMenuMove) onMenuMove();
                }
            });
            ov.addEventListener('click', (e) => {
                if (!activeScreenId) return;
                const t = findItem(e.target, ov);
                if (!t) return;
                const items = getMenuItems(activeScreenId);
                const i = items.indexOf(t);
                if (i < 0) return;
                menuIndex = i;
                updateSelection(activeScreenId);
                if (current && current.keydown) current.keydown('Enter');
            });
        }

        function findItem(target, ov) {
            while (target && target !== ov) {
                if (target.classList && target.classList.contains(itemClass)) {
                    return target;
                }
                target = target.parentNode;
            }
            return null;
        }

        const screens = {};

        function define(name, def) { screens[name] = def || {}; }

        function switchTo(name, payload) {
            const next = screens[name];
            if (!next) {
                console.warn('Screens.switchTo: unknown screen', name);
                return;
            }
            if (current && current.exit) current.exit();
            current     = next;
            currentName = name;
            menuIndex   = 0;
            ensureMouse();
            if (current.enter) current.enter(payload);
            if (activeScreenId) updateSelection(activeScreenId);
            applyHudToggle();
        }

        function applyHudToggle() {
            if (!hudSelector) return;
            if (!hudEl) hudEl = document.querySelector(hudSelector);
            if (!hudEl) return;
            hudEl.style.display = hudFor.indexOf(currentName) >= 0 ? '' : 'none';
        }

        function bgActive() {
            return bgScreens && bgScreens.indexOf(currentName) >= 0;
        }

        return {
            define, switchTo,
            name:    () => currentName,
            current: () => current,
            showOverlay, hideOverlay,
            getMenuItems, updateSelection, setMenuIndex,
            menuNav,
            keydown: (key) => { if (current && current.keydown) current.keydown(key); },
            keyup:   (key) => { if (current && current.keyup)   current.keyup(key); },
            update:  (dt, w, h) => {
                if (bgActive()) {
                    if (!bgInited && bgInit) { bgInit(w, h); bgInited = true; }
                    if (bgUpdate) bgUpdate(dt, w, h);
                }
                if (current && current.update) current.update(dt, w, h);
            },
            draw:    (ctx, w, h) => {
                if (bgActive() && bgDraw) bgDraw(ctx, w, h);
                if (current && current.draw) current.draw(ctx, w, h);
            },
        };
    }

    global.Screens = { create };
})(typeof window !== 'undefined' ? window : globalThis);
