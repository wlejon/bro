// Shared layout helper for system settings panels (system/nav.html and
// system/settings/*.html). The preferences modal is a fixed-size card
// centered in the viewport, split into a sidebar (owned by nav.html) and a
// content region (owned by each settings/*.html panel). Every panel needs
// the same card geometry to line up with the shell, so it lives here once
// instead of being copy-pasted per file.
(function() {
    var CARD_W = 720, CARD_H = 520, SIDEBAR_W = 180, HEADER_H = 44;

    function cardOrigin(vp) {
        return {
            left: Math.max(0, Math.floor((vp.width - CARD_W) / 2)),
            top: Math.max(0, Math.floor((vp.height - CARD_H) / 2))
        };
    }

    function cardRect(vp) {
        var o = cardOrigin(vp);
        return { left: o.left, top: o.top, width: CARD_W, height: CARD_H };
    }

    function contentRect(vp) {
        var o = cardOrigin(vp);
        return {
            left: o.left + SIDEBAR_W, top: o.top + HEADER_H,
            width: CARD_W - SIDEBAR_W, height: CARD_H - HEADER_H
        };
    }

    function applyRect(el, rect) {
        el.style.setProperty('left', rect.left + 'px');
        el.style.setProperty('top', rect.top + 'px');
        el.style.setProperty('width', rect.width + 'px');
        el.style.setProperty('height', rect.height + 'px');
    }

    window.PanelLayout = {
        CARD_W: CARD_W, CARD_H: CARD_H, SIDEBAR_W: SIDEBAR_W, HEADER_H: HEADER_H,

        // Raw geometry, for callers composing their own layout.
        cardRect: cardRect,
        contentRect: contentRect,

        // nav.html: position the modal card, and size the full-viewport
        // backdrop if one is passed.
        positionCard: function(cardEl, backdropEl) {
            var vp = __bro.settingsUI.getViewport();
            if (backdropEl) {
                backdropEl.style.setProperty('width', vp.width + 'px');
                backdropEl.style.setProperty('height', vp.height + 'px');
            }
            applyRect(cardEl, cardRect(vp));
        },

        // settings/*.html: position the panel's content region (inside the
        // card, to the right of the sidebar and below the header).
        positionContent: function(panelEl) {
            applyRect(panelEl, contentRect(__bro.settingsUI.getViewport()));
        },

        // Call fn() immediately, then wire it as the engine's per-panel
        // resize hook.
        onResize: function(fn) {
            fn();
            window.__onResize = fn;
        }
    };
})();
