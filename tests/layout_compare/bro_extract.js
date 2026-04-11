// Extracts DOM properties from the loaded app using bro-headless.
// Usage: bro-headless --no-gpu --width 800 --height 600 <case-dir> bro_extract.js
// Output path is passed via BRO_EXTRACT_OUTPUT env var or defaults to 'bro_output.json'.

// --- inline the extraction function ---
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

function extractDOM() {
    var elements = document.querySelectorAll('*');
    var results = [];

    for (var i = 0; i < elements.length; i++) {
        var el = elements[i];
        var tag = el.tagName;

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

// --- run extraction and write output ---
flush();

var result = extractDOM();
var outputPath = (typeof __BRO_OUTPUT !== 'undefined') ? __BRO_OUTPUT : 'bro_output.json';

var fs = globalThis.__brokit_fs;
fs.writeFileSync(outputPath, JSON.stringify(result, null, 2), 'utf8');
console.log('Bro: ' + result.length + ' elements -> ' + outputPath);
