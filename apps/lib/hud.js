// hud.js — DOM-based HUD helpers (text updates, show/hide, toasts).
//
// Usage:
//   Hud.text("#score", "1200");
//   Hud.show("#game-hud"); Hud.hide("#game-hud");
//   Hud.toast("Direct hit!", 1200);       // auto-hides after ms
//   Hud.toast("Fault!", 1200, { id: "alert", className: "alert" });

(function (global) {
    'use strict';

    const toasts = new Map(); // id → timer

    function q(sel) {
        if (!sel) return null;
        if (sel instanceof Element) return sel;
        if (typeof sel === 'string') return document.querySelector(sel);
        return null;
    }

    function text(sel, value) {
        const el = q(sel);
        if (el) el.textContent = value;
    }

    function html(sel, value) {
        const el = q(sel);
        if (el) el.innerHTML = value;
    }

    function show(sel, display) {
        const el = q(sel);
        if (el) el.style.display = display || 'block';
    }

    function hide(sel) {
        const el = q(sel);
        if (el) el.style.display = 'none';
    }

    // One-shot notification overlay. If `opts.container` is provided,
    // the toast node is created inside it; otherwise it's appended to
    // document.body at fixed position. Returns the element.
    function toast(message, durationMs, opts) {
        opts = opts || {};
        const id = opts.id || 'hud-toast-default';
        let el = document.getElementById(id);
        if (!el) {
            el = document.createElement('div');
            el.id = id;
            el.className = opts.className || 'hud-toast';
            if (!opts.className) {
                el.style.position = 'fixed';
                el.style.top = '20%';
                el.style.left = '50%';
                el.style.transform = 'translate(-50%, -50%)';
                el.style.padding = '10px 18px';
                el.style.background = 'rgba(0,0,0,0.75)';
                el.style.color = '#fff';
                el.style.font = '18px monospace';
                el.style.borderRadius = '4px';
                el.style.pointerEvents = 'none';
                el.style.zIndex = '1000';
            }
            (opts.container || document.body).appendChild(el);
        }
        el.textContent = message;
        el.style.display = 'block';

        const prev = toasts.get(id);
        if (prev) clearTimeout(prev);
        const t = setTimeout(() => {
            el.style.display = 'none';
            toasts.delete(id);
        }, durationMs || 1200);
        toasts.set(id, t);
        return el;
    }

    global.Hud = { text, html, show, hide, toast, q };
})(typeof window !== 'undefined' ? window : globalThis);
