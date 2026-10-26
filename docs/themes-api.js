/**
 * @file docs/themes-api.js
 * @summary Documentation and examples for the `bro.themes` JavaScript API.
 */

/**
 * `bro.themes` provides desktop and terminal color scheme utilities:
 * - WCAG 2.1 and APCA perceptual contrast measurement and automatic contrast adjustment
 * - Theme import/export across formats (VS Code, Windows Terminal, iTerm2, Kitty, Alacritty, Ghostty, Base16, Base24)
 * - Color spaces (sRGB, Linear RGB, Oklab, Oklch, Lab, XYZ, HSL, HSV) and gamut fitting
 *
 * Mounted automatically in Bronze when `BRO_WITH_THEMES` is enabled.
 */

// ============================================================================
// 1. Contrast & Compliance
// ============================================================================

// Measure contrast ratio using WCAG 2.1 (returns number in [1.0, 21.0])
const ratio = bro.themes.contrast('#ffffff', '#000000'); // 21.0 (alias: wcagContrast)

// Measure contrast using APCA (Accessible Perceptual Contrast Algorithm)
// Returns signed Lc contrast (positive for dark-on-light, negative for light-on-dark)
const apca = bro.themes.contrast('#ffffff', '#000000', { method: 'apca' });
const apcaAbs = bro.themes.apcaContrast('#ffffff', '#000000', { absolute: true });

// Check compliance levels (`isLargeText` may be passed via options object or boolean 3rd arg)
const level = bro.themes.wcagLevel('#ffffff', '#000000', { isLargeText: false }); // 'aaa' | 'aa' | 'fail'
const passesAa = bro.themes.meetsAa('#ffffff', '#333333', { isLargeText: false }); // true
const passesAaa = bro.themes.meetsAaa('#ffffff', '#333333', { isLargeText: false }); // true / false

// Automatic contrast adjustment in Oklch preserving perceived hue:
const adjustedHex = bro.themes.adjustContrast('#555555', '#444444', { minRatio: 4.5 });

// Adjust all ANSI/UI colors in a theme object to meet a minimum contrast ratio against its background:
// const adjustedTheme = bro.themes.adjustPaletteContrast(theme, { minRatio: 4.5 });

// Relative luminance
const lum = bro.themes.relativeLuminance('#ffffff'); // 1.0

// ============================================================================
// 2. Theme Import & Export
// ============================================================================

// Detect theme format from content or file name
const format = bro.themes.detectFormat(contentString, 'theme.json'); // 'vscode' | 'iterm' | ...

// Import a theme (aliases: importTheme, parseTheme)
const theme = bro.themes.import(jsonOrXmlString, { format: 'auto' });
// theme object structure:
// {
//   name: "MyTheme",
//   author: "Author",
//   variant: "dark",
//   isDark: true,
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

// Export a theme to another format (aliases: exportTheme, serializeTheme)
const wtJson = bro.themes.export(theme, { format: 'windows_terminal' });
const itermXml = bro.themes.export(theme, { format: 'iterm' });

// Read / write a theme file (paths resolve like fs.*). load asks to read the
// path and save to write it (docs/apps.md "Permissions"): one the app may not
// touch throws an Error with code 'EACCES' and `path` (named PermissionRevoked
// for a grant taken back), and nothing is read or written.
const fromFile = bro.themes.load('themes/dark.json', { format: 'auto', colorFormat: 'hex' });
const wrote = bro.themes.save(theme, 'themes/copy.json', { format: 'windows-terminal' }); // true when written

// ============================================================================
// 3. Color Space & Gamut Utilities
// ============================================================================

// Parsing and formatting (aliases: fromHexString, Color)
const c = bro.themes.parseColor('#ff8800'); // { r: 255, g: 136, b: 0, a: 255 }
const hex = bro.themes.toHexString(c); // "#ff8800" (alias: formatColor)

// Color space conversion
const oklab = bro.themes.srgbToOklab(c); // { l, a, b, alpha }
const oklch = bro.themes.srgbToOklch(c); // { l, c, h, alpha }
const fitted = bro.themes.fitOklchToGamut(oklch);
const backToRgb = bro.themes.oklchToSrgb(fitted);
