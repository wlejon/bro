// The page's external module script in a subdirectory, for
// tests/engine/test_page_module_script_url.js: its import.meta.url names this
// file, and './helper.js' resolves next to it, not next to index.html.
import { where } from './helper.js';

const moduleScoped = 'module_sub';  // module scope: must not become a global
window.__moduleSub = {
    metaUrl: import.meta.url,
    helper: where,
    leaked: typeof globalThis.moduleScoped !== 'undefined',
    classicRanFirst: typeof window.__lifecycle === 'object',
};
void moduleScoped;
