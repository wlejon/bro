// Headless test for bro.themes
assert(typeof bro.themes === 'object', 'bro.themes namespace exists');
assert(bro.themes.available === true, 'bro.themes.available is true');

// 1. Contrast calculations
const whiteBlack = bro.themes.contrast('#ffffff', '#000000');
assert(Math.abs(whiteBlack - 21.0) < 0.01, 'white on black contrast is 21:1');

const blackBlack = bro.themes.contrast('#000000', '#000000');
assert(Math.abs(blackBlack - 1.0) < 0.01, 'black on black contrast is 1:1');

// 2. Luminance & Compliance
const lWhite = bro.themes.relativeLuminance('#ffffff');
const lBlack = bro.themes.relativeLuminance('#000000');
assert(Math.abs(lWhite - 1.0) < 1e-4, 'white relative luminance is 1.0');
assert(Math.abs(lBlack) < 1e-4, 'black relative luminance is 0.0');

assert(bro.themes.wcagLevel('#ffffff', '#000000') === 'aaa', 'white on black is AAA');
assert(bro.themes.meetsAa('#ffffff', '#000000') === true, 'white on black meets AA');
assert(bro.themes.meetsAaa('#ffffff', '#000000') === true, 'white on black meets AAA');

// 3. APCA
const apcaDarkOnLight = bro.themes.contrast('#000000', '#ffffff', { method: 'apca' });
assert(apcaDarkOnLight > 0, 'APCA dark text on light bg is positive');

// 4. Contrast Adjustment
const orig = '#555555';
const bg = '#444444';
const beforeCr = bro.themes.contrast(orig, bg);
assert(beforeCr < 4.5, 'original contrast is below 4.5');
const adjustedHex = bro.themes.adjustContrast(orig, bg, { minRatio: 4.5 });
const afterCr = bro.themes.contrast(adjustedHex, bg);
assert(afterCr >= 4.5, 'adjusted contrast is at least 4.5');

// 5. Theme Import / Export round-trip
const sampleTheme = {
    name: "HeadlessDark",
    author: "Test",
    ui: {
        background: "#1e1e1e",
        foreground: "#d4d4d4",
        cursor: "#aeafad",
        selectionBackground: "#264f78"
    },
    ansi: [
        "#000000", "#cd3131", "#0dbc79", "#e5e510",
        "#2472c8", "#bc3fbc", "#11a8cd", "#e5e5e5",
        "#666666", "#f14c4c", "#23d18b", "#f5f543",
        "#3b8eea", "#d670d6", "#29b8db", "#ffffff"
    ]
};

const exportedJson = bro.themes.export(sampleTheme, { format: 'windows_terminal' });
assert(typeof exportedJson === 'string' && exportedJson.length > 0, 'exported theme string non-empty');
const imported = bro.themes.import(exportedJson, { format: 'windows_terminal' });
assert(imported && imported.name === 'HeadlessDark', 'imported theme matches name');
assert(imported.ui && imported.ui.background === '#1e1e1e', 'imported background matches');

// 6. Color space transforms
const c = bro.themes.parseColor('#ff5500');
assert(c.r === 255 && c.g === 85 && c.b === 0, 'parseColor matches RGB');
const oklch = bro.themes.srgbToOklch(c);
assert(typeof oklch.l === 'number' && typeof oklch.c === 'number' && typeof oklch.h === 'number', 'oklch fields exist');

console.log('test_themes.js PASSED');
