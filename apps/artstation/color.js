// Color utilities — generation, conversion, palette manipulation.
//
// All `hex` strings are "#rrggbb"; HSL uses h:[0,360), s,l:[0,1].

const color = (() => {

    function hexToRgb(hex) {
        const s = hex.replace('#','');
        if (s.length === 3) {
            return {
                r: parseInt(s[0]+s[0], 16),
                g: parseInt(s[1]+s[1], 16),
                b: parseInt(s[2]+s[2], 16),
            };
        }
        const n = parseInt(s, 16);
        return { r:(n>>16)&255, g:(n>>8)&255, b:n&255 };
    }

    function rgbToHex(r, g, b) {
        const c = v => Math.max(0, Math.min(255, Math.round(v))).toString(16).padStart(2,'0');
        return '#' + c(r) + c(g) + c(b);
    }

    function rgbToHsl(r, g, b) {
        r /= 255; g /= 255; b /= 255;
        const max = Math.max(r, g, b), min = Math.min(r, g, b);
        let h, s; const l = (max + min) / 2;
        if (max === min) { h = 0; s = 0; }
        else {
            const d = max - min;
            s = l > 0.5 ? d / (2 - max - min) : d / (max + min);
            switch (max) {
                case r: h = (g - b) / d + (g < b ? 6 : 0); break;
                case g: h = (b - r) / d + 2; break;
                default: h = (r - g) / d + 4;
            }
            h *= 60;
        }
        return { h, s, l };
    }

    function hslToRgb(h, s, l) {
        h = ((h % 360) + 360) % 360;
        if (s === 0) {
            const v = l * 255; return { r: v, g: v, b: v };
        }
        const q = l < 0.5 ? l * (1 + s) : l + s - l * s;
        const p = 2 * l - q;
        const hk = h / 360;
        const tc = t => {
            if (t < 0) t += 1; if (t > 1) t -= 1;
            if (t < 1/6) return p + (q - p) * 6 * t;
            if (t < 1/2) return q;
            if (t < 2/3) return p + (q - p) * (2/3 - t) * 6;
            return p;
        };
        return {
            r: 255 * tc(hk + 1/3),
            g: 255 * tc(hk),
            b: 255 * tc(hk - 1/3),
        };
    }

    function hexToHsl(hex) {
        const { r, g, b } = hexToRgb(hex);
        return rgbToHsl(r, g, b);
    }

    function hslToHex(h, s, l) {
        const { r, g, b } = hslToRgb(h, s, l);
        return rgbToHex(r, g, b);
    }

    // Lighten / darken by absolute lightness amount (0..1).
    function lighten(hex, amt) {
        const { h, s, l } = hexToHsl(hex);
        return hslToHex(h, s, Math.min(1, l + amt));
    }
    function darken(hex, amt) {
        const { h, s, l } = hexToHsl(hex);
        return hslToHex(h, s, Math.max(0, l - amt));
    }
    function saturate(hex, amt) {
        const { h, s, l } = hexToHsl(hex);
        return hslToHex(h, Math.min(1, s + amt), l);
    }
    function shiftHue(hex, deg) {
        const { h, s, l } = hexToHsl(hex);
        return hslToHex(h + deg, s, l);
    }

    // Linear interpolation between two hex colors, t in [0,1].
    function mix(hexA, hexB, t) {
        const a = hexToRgb(hexA), b = hexToRgb(hexB);
        return rgbToHex(
            a.r + (b.r - a.r) * t,
            a.g + (b.g - a.g) * t,
            a.b + (b.b - a.b) * t,
        );
    }

    // 5-stop ramp from `base`: [shadow2, shadow1, base, hi1, hi2].
    // Spread is the lightness delta between adjacent stops.
    function ramp(hex, spread) {
        spread = spread === undefined ? 0.12 : spread;
        return [
            darken(hex, spread * 2),
            darken(hex, spread),
            hex,
            lighten(hex, spread),
            lighten(hex, spread * 2),
        ];
    }

    // Harmonies: pick N evenly-spaced hues anchored at `hex`.
    function analogous(hex, n, spread) {
        n = n || 3; spread = spread || 30;
        const { h, s, l } = hexToHsl(hex);
        const out = [];
        const start = h - spread * (n - 1) / 2;
        for (let i = 0; i < n; i++) out.push(hslToHex(start + i * spread, s, l));
        return out;
    }
    function complementary(hex) {
        const { h, s, l } = hexToHsl(hex);
        return [hex, hslToHex(h + 180, s, l)];
    }
    function triadic(hex) {
        const { h, s, l } = hexToHsl(hex);
        return [hex, hslToHex(h + 120, s, l), hslToHex(h + 240, s, l)];
    }

    // Apply a hex color and a 0..1 alpha to the canvas as fill / stroke.
    // Avoids re-stringifying rgba() at every call site.
    function withAlpha(hex, a) {
        const { r, g, b } = hexToRgb(hex);
        return `rgba(${r|0},${g|0},${b|0},${a})`;
    }

    return {
        hexToRgb, rgbToHex, rgbToHsl, hslToRgb, hexToHsl, hslToHex,
        lighten, darken, saturate, shiftHue, mix, withAlpha,
        ramp, analogous, complementary, triadic,
    };
})();

if (typeof window !== 'undefined') window.color = color;
