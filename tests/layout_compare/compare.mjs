// Compares two DOM extraction JSON files and reports differences.
// Usage: node compare.mjs <chrome.json> <bro.json> [--tolerance=N]
//
// Numeric tolerance (default 2px) allows for minor rounding differences.
// Reports: per-element diffs, summary stats, and exit code 1 if any failures.

import { readFileSync } from 'fs';

const args = process.argv.slice(2);
if (args.length < 2) {
    console.error('Usage: node compare.mjs <chrome.json> <bro.json> [--tolerance=N]');
    process.exit(1);
}

const chromePath = args[0];
const broPath = args[1];
let TOLERANCE = 2; // pixels

for (const arg of args.slice(2)) {
    const m = arg.match(/^--tolerance=(\d+(?:\.\d+)?)$/);
    if (m) TOLERANCE = parseFloat(m[1]);
}

const chrome = JSON.parse(readFileSync(chromePath, 'utf8'));
const bro = JSON.parse(readFileSync(broPath, 'utf8'));

// Build lookup by selector (with fallback to index for duplicates)
function buildIndex(elements) {
    const map = new Map();
    for (const el of elements) {
        const key = el.id ? `#${el.id}` : el.selector;
        if (!map.has(key)) {
            map.set(key, el);
        } else {
            // Duplicate selector — use index suffix
            let n = 2;
            while (map.has(`${key}[${n}]`)) n++;
            map.set(`${key}[${n}]`, el);
        }
    }
    return map;
}

function parseNumeric(val) {
    if (typeof val === 'number') return val;
    if (typeof val === 'string') {
        const n = parseFloat(val);
        if (!isNaN(n)) return n;
    }
    return null;
}

function normalizeColor(val) {
    if (!val || typeof val !== 'string') return val;
    // Normalize rgba(r, g, b, 1) -> rgb(r, g, b)
    const m = val.match(/^rgba?\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*(?:,\s*1(?:\.0*)?\s*)?\)$/);
    if (m) return `rgb(${m[1]}, ${m[2]}, ${m[3]})`;
    return val;
}

const COLOR_PROPS = new Set(['color', 'backgroundColor']);

function compareValues(prop, chromeVal, broVal) {
    if (chromeVal === broVal) return null;

    // Normalize colors
    if (COLOR_PROPS.has(prop)) {
        if (normalizeColor(chromeVal) === normalizeColor(broVal)) return null;
    }

    // Numeric comparison with tolerance
    const cn = parseNumeric(chromeVal);
    const bn = parseNumeric(broVal);
    if (cn !== null && bn !== null) {
        if (Math.abs(cn - bn) <= TOLERANCE) return null;
        return { chrome: chromeVal, bro: broVal, delta: Math.round((bn - cn) * 100) / 100 };
    }

    // String comparison — normalize "0px" vs "0", "auto" equivalences
    const cs = String(chromeVal).replace(/^0px$/, '0');
    const bs = String(broVal).replace(/^0px$/, '0');
    if (cs === bs) return null;

    return { chrome: chromeVal, bro: broVal };
}

// --- Compare ---
const chromeIndex = buildIndex(chrome);
const broIndex = buildIndex(bro);

let totalElements = 0;
let elementsWithDiffs = 0;
let totalDiffs = 0;
let missingInBro = 0;
let extraInBro = 0;

const diffDetails = [];

for (const [key, cEl] of chromeIndex) {
    totalElements++;
    const bEl = broIndex.get(key);

    if (!bEl) {
        missingInBro++;
        diffDetails.push({ element: key, error: 'MISSING in bro' });
        continue;
    }

    const elDiffs = {};
    let hasDiff = false;

    // Compare rect
    for (const rp of ['x', 'y', 'width', 'height']) {
        const d = compareValues(`rect.${rp}`, cEl.rect[rp], bEl.rect[rp]);
        if (d) {
            elDiffs[`rect.${rp}`] = d;
            hasDiff = true;
            totalDiffs++;
        }
    }

    // Compare dimensions
    for (const dp of ['offsetWidth', 'offsetHeight', 'clientWidth', 'clientHeight',
                       'scrollWidth', 'scrollHeight']) {
        const cv = cEl.dimensions[dp];
        const bv = bEl.dimensions[dp];
        if (cv !== undefined && bv !== undefined) {
            const d = compareValues(dp, cv, bv);
            if (d) {
                elDiffs[dp] = d;
                hasDiff = true;
                totalDiffs++;
            }
        }
    }

    // Compare styles
    const allStyleKeys = new Set([
        ...Object.keys(cEl.styles || {}),
        ...Object.keys(bEl.styles || {})
    ]);
    for (const sp of allStyleKeys) {
        const cv = (cEl.styles || {})[sp];
        const bv = (bEl.styles || {})[sp];
        if (cv === undefined || bv === undefined) {
            // Only flag if Chrome has the property and bro doesn't (or vice versa)
            // Skip if both are effectively empty
            if (cv !== undefined && cv !== '' && bv === undefined) {
                elDiffs[`style.${sp}`] = { chrome: cv, bro: '(missing)' };
                hasDiff = true;
                totalDiffs++;
            }
        } else {
            const d = compareValues(sp, cv, bv);
            if (d) {
                elDiffs[`style.${sp}`] = d;
                hasDiff = true;
                totalDiffs++;
            }
        }
    }

    if (hasDiff) {
        elementsWithDiffs++;
        diffDetails.push({ element: key, diffs: elDiffs });
    }
}

// Check for extra elements in bro not in chrome
for (const [key] of broIndex) {
    if (!chromeIndex.has(key)) {
        extraInBro++;
        diffDetails.push({ element: key, error: 'EXTRA in bro (not in Chrome)' });
    }
}

// --- Report ---
console.log('=== Layout Comparison Report ===');
console.log(`Chrome elements: ${chrome.length}`);
console.log(`Bro elements:    ${bro.length}`);
console.log(`Tolerance:       ${TOLERANCE}px`);
console.log('');
console.log(`Elements compared: ${totalElements}`);
console.log(`Elements with diffs: ${elementsWithDiffs}`);
console.log(`Total property diffs: ${totalDiffs}`);
console.log(`Missing in bro: ${missingInBro}`);
console.log(`Extra in bro: ${extraInBro}`);

if (diffDetails.length > 0) {
    console.log('\n--- Details ---');
    for (const d of diffDetails) {
        if (d.error) {
            console.log(`\n  ${d.element}: ${d.error}`);
        } else {
            console.log(`\n  ${d.element}:`);
            for (const [prop, diff] of Object.entries(d.diffs)) {
                if (diff.delta !== undefined) {
                    console.log(`    ${prop}: chrome=${diff.chrome} bro=${diff.bro} (delta: ${diff.delta > 0 ? '+' : ''}${diff.delta})`);
                } else {
                    console.log(`    ${prop}: chrome="${diff.chrome}" bro="${diff.bro}"`);
                }
            }
        }
    }
}

const matchRate = totalElements > 0
    ? Math.round((totalElements - elementsWithDiffs - missingInBro) / totalElements * 100)
    : 0;
console.log(`\n=== Match rate: ${matchRate}% (${totalElements - elementsWithDiffs - missingInBro}/${totalElements} elements) ===`);

process.exit(elementsWithDiffs > 0 || missingInBro > 0 ? 1 : 0);
