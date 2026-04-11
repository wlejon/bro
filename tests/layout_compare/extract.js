// Shared DOM property extraction function.
// Works in both Chrome (via Puppeteer page.evaluate) and bro-headless.
// Returns an array of element descriptors with layout and style properties.

function extractDOM() {
    var STYLE_PROPS = [
        'display', 'position', 'float',
        'width', 'height', 'minWidth', 'minHeight', 'maxWidth', 'maxHeight',
        'marginTop', 'marginRight', 'marginBottom', 'marginLeft',
        'paddingTop', 'paddingRight', 'paddingBottom', 'paddingLeft',
        'borderTopWidth', 'borderRightWidth', 'borderBottomWidth', 'borderLeftWidth',
        'top', 'right', 'bottom', 'left',
        'flexDirection', 'flexWrap', 'justifyContent', 'alignItems', 'alignSelf',
        'flexGrow', 'flexShrink', 'flexBasis',
        'gap', 'rowGap', 'columnGap',
        'overflow', 'overflowX', 'overflowY',
        'boxSizing',
        'verticalAlign', 'textAlign', 'whiteSpace', 'textOverflow',
        'fontSize', 'fontWeight', 'lineHeight',
        'color', 'backgroundColor',
        'visibility', 'opacity', 'zIndex'
    ];

    var elements = document.querySelectorAll('*');
    var results = [];

    for (var i = 0; i < elements.length; i++) {
        var el = elements[i];
        var tag = el.tagName;

        // Skip head and its children — not visual
        if (tag === 'HEAD' || tag === 'META' || tag === 'TITLE' ||
            tag === 'STYLE' || tag === 'LINK' || tag === 'SCRIPT') continue;

        var rect = el.getBoundingClientRect();
        var cs = getComputedStyle(el);

        var styles = {};
        for (var j = 0; j < STYLE_PROPS.length; j++) {
            var prop = STYLE_PROPS[j];
            var val = cs[prop];
            if (val !== undefined && val !== '') {
                styles[prop] = val;
            }
        }

        // Build a selector path for identification
        var selector = tag.toLowerCase();
        if (el.id) selector += '#' + el.id;
        else if (el.className && typeof el.className === 'string')
            selector += '.' + el.className.trim().split(/\s+/).join('.');

        results.push({
            selector: selector,
            tag: tag,
            id: el.id || null,
            className: (typeof el.className === 'string') ? el.className : '',
            rect: {
                x: Math.round(rect.x * 100) / 100,
                y: Math.round(rect.y * 100) / 100,
                width: Math.round(rect.width * 100) / 100,
                height: Math.round(rect.height * 100) / 100
            },
            dimensions: {
                offsetWidth: el.offsetWidth,
                offsetHeight: el.offsetHeight,
                clientWidth: el.clientWidth,
                clientHeight: el.clientHeight,
                scrollWidth: el.scrollWidth,
                scrollHeight: el.scrollHeight
            },
            styles: styles
        });
    }

    return results;
}
