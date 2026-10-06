/**
 * @file docs/themes-api.js
 * @summary Documentation and examples for the `bro.themes` JavaScript API.
 */

/**
 * `bro.themes` provides desktop and terminal color scheme utilities:
 * - WCAG 2.1 and APCA perceptual contrast measurement
 * - Theme import/export across formats (VS Code, Windows Terminal, iTerm2, Kitty, Alacritty, Ghostty, Base16, Base24)
 * - Color spaces (sRGB, Linear RGB, Oklab, Oklch, Lab, XYZ, HSL, HSV) and gamut fitting
 *
 * Mounted automatically in Bronze when `BRO_WITH_THEMES` is enabled.
 */

// ============================================================================
// 1. Contrast & Compliance
// ============================================================================

// Measure contrast ratio using WCAG 2.1 (returns number in [1.0, 21.0])
const ratio = bro.themes.contrast('#ffffff', '#000000'); // 21.0

// Measure contrast using APCA (Accessible Perceptual Contrast Algorithm)
// Returns signed Lc contrast (positive for dark-on-light, negative for light-on-dark)
const apca = bro.themes.contrast('#ffffff', '#000000', { method: 'apca' });

// Check compliance levels
const level = bro.themes.wcagLevel('#ffffff', '#000000'); // 'aaa' | 'aa' | 'fail'
const passesAa = bro.themes.meetsAa('#ffffff', '#333333'); // true
const passesAaa = bro.themes.meetsAaa('#ffffff', '#333333', { isLargeText: false }); // true / false

// Automatic contrast adjustment in Oklch preserving perceived hue:
const adjustedHex = bro.themes.adjustContrast('#555555', '#444444', { minRatio: 4.5 });

// Relative luminance
const lum = bro.themes.relativeLuminance('#ffffff'); // 1.0

// ============================================================================
// 2. Theme Import & Export
// ============================================================================

// Detect theme format from content or file name
const format = bro.themes.detectFormat(contentString, 'theme.json'); // 'vscode' | 'iterm' | ...

// Import a theme
const theme = bro.themes.import(jsonOrXmlString, { format: 'auto' });
// theme object structure:
// {
//   name: "MyTheme",
//   author: "Author",
//   ui: {
//     background: "#282a36",
//     foreground: "#f8f8f2",
//     cursor: "#f8f8f2",
//     selectionBackground: "#44475a",
//     ...
//   },
//   ansi: [
//     "#000000", "#ff5555", "#50fa7b", "#f1fa8c", ... 16 colors
//   ]
// }

// Export a theme to another format
const wtJson = bro.themes.export(theme, { format: 'windows_terminal' });
const itermXml = bro.themes.export(theme, { format: 'iterm' });

// ============================================================================
// 3. Color Space Utilities
// ============================================================================

// Parsing and formatting
const c = bro.themes.parseColor('#ff8800'); // { r: 255, g: 136, b: 0, a: 255 }
const hex = bro.themes.toHexString(c); // "#ff8800"

// Color space conversion
const oklab = bro.themes.srgbToOklab(c); // { l, a, b, alpha }
const oklch = bro.themes.srgbToOklch(c); // { l, c, h, alpha }
const backToRgb = bro.themes.oklchToSrgb(oklch);
