// Extracts DOM properties from a test case using Chrome via Puppeteer.
// Usage: node chrome_extract.mjs <case-dir> <output-json>
// Example: node chrome_extract.mjs cases/box-model output/box-model.chrome.json

import puppeteer from 'puppeteer';
import { readFileSync, writeFileSync } from 'fs';
import { resolve } from 'path';
import { pathToFileURL } from 'url';

const args = process.argv.slice(2);
if (args.length < 2) {
    console.error('Usage: node chrome_extract.mjs <case-dir> <output-json>');
    process.exit(1);
}

const caseDir = resolve(args[0]);
const outputPath = resolve(args[1]);
const htmlPath = resolve(caseDir, 'index.html');

// Read the shared extraction function
const extractSrc = readFileSync(
    resolve(import.meta.dirname, 'extract.js'), 'utf8'
);

const WIDTH = 800;
const HEIGHT = 600;

const browser = await puppeteer.launch({
    headless: true,
    args: [
        `--window-size=${WIDTH},${HEIGHT}`,
        '--no-sandbox',
        '--disable-setuid-sandbox',
        '--force-device-scale-factor=1'
    ]
});

const page = await browser.newPage();
await page.setViewport({ width: WIDTH, height: HEIGHT, deviceScaleFactor: 1 });

const fileUrl = pathToFileURL(htmlPath).href;
await page.goto(fileUrl, { waitUntil: 'load' });

// Inject and run the extraction function
const result = await page.evaluate((src) => {
    eval(src);
    return extractDOM();
}, extractSrc);

writeFileSync(outputPath, JSON.stringify(result, null, 2));
console.log(`Chrome: ${result.length} elements -> ${outputPath}`);

await browser.close();
