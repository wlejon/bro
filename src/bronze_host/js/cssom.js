// The CSSOM, as JavaScript: document.styleSheets, StyleSheetList,
// CSSStyleSheet (a <style>'s or <link>'s `sheet`, and constructable sheets with
// replace / replaceSync), the CSSRule family (CSSStyleRule, CSSMediaRule,
// CSSSupportsRule, CSSKeyframesRule / CSSKeyframeRule, CSSFontFaceRule,
// CSSImportRule; other at-rules as CSSGroupingRule / CSSRule keeping their
// text), CSSStyleDeclaration for a rule's style, MediaList, and
// adoptedStyleSheets on Document and ShadowRoot.
//
// The sheets are the document's (dom/document_cssom.cpp): a sheet object reads
// its element's CSS text through `__bro_cssom` (host_cssom_hooks.cpp) and
// parses it into rules here, and every change made through the CSSOM
// (insertRule, deleteRule, a rule's selectorText or style, replaceSync,
// disabled, adoptedStyleSheets) writes the sheet's text back, which rebuilds the
// cascade, so the page restyles. A <style> element's own text is left as it
// was, as in a browser. Serialization follows Chromium's shape
// (`a { color: red; }`), with the values as written.
(function () {
    'use strict';

    const g = globalThis;
    const hooks = __bro_cssom;

    function domException(message, name) {
        const D = g.DOMException;
        if (typeof D === 'function') return new D(message, name);
        const e = new Error(message);
        e.name = name;
        return e;
    }

    // --- Parsing ------------------------------------------------------------

    // Index past a comment or string starting at i, or i when there is none.
    function skipQuotedOrComment(text, i) {
        const c = text[i];
        if (c === '/' && text[i + 1] === '*') {
            const end = text.indexOf('*/', i + 2);
            return end < 0 ? text.length : end + 2;
        }
        if (c === '"' || c === "'") {
            let j = i + 1;
            while (j < text.length && text[j] !== c) {
                if (text[j] === '\\') j++;
                j++;
            }
            return j + 1;
        }
        return i;
    }

    function stripComments(text) {
        let out = '';
        let i = 0;
        while (i < text.length) {
            if (text[i] === '/' && text[i + 1] === '*') {
                i = skipQuotedOrComment(text, i);
                continue;
            }
            const j = skipQuotedOrComment(text, i);
            if (j !== i) {
                out += text.slice(i, j);
                i = j;
                continue;
            }
            out += text[i++];
        }
        return out;
    }

    // Top-level statements of a rule list: [{prelude, body}] where body is
    // the text inside the braces, or null for a statement ending in ';'.
    function splitRules(text) {
        const out = [];
        let i = 0;
        let start = 0;
        const n = text.length;
        while (i < n) {
            const j = skipQuotedOrComment(text, i);
            if (j !== i) { i = j; continue; }
            const c = text[i];
            if (c === ';') {
                const prelude = stripComments(text.slice(start, i)).trim();
                if (prelude) out.push({ prelude: prelude, body: null });
                start = ++i;
                continue;
            }
            if (c === '{') {
                let depth = 1;
                let k = i + 1;
                while (k < n && depth > 0) {
                    const m = skipQuotedOrComment(text, k);
                    if (m !== k) { k = m; continue; }
                    if (text[k] === '{') depth++;
                    else if (text[k] === '}') depth--;
                    k++;
                }
                out.push({ prelude: stripComments(text.slice(start, i)).trim(), body: text.slice(i + 1, Math.max(i + 1, k - 1)) });
                start = i = k;
                continue;
            }
            i++;
        }
        return out;
    }

    // `prop: value [!important]` declarations of a block, and any nested
    // rules (CSS nesting) as raw text.
    function splitDeclarations(text) {
        const decls = [];
        let nested = '';
        let i = 0;
        let start = 0;
        let paren = 0;
        const n = text.length;
        const flush = (end) => {
            const raw = stripComments(text.slice(start, end)).trim();
            const colon = raw.indexOf(':');
            if (colon > 0) {
                const name = raw.slice(0, colon).trim();
                let value = raw.slice(colon + 1).trim();
                let important = false;
                const bang = /!\s*important\s*$/i.exec(value);
                if (bang) {
                    important = true;
                    value = value.slice(0, bang.index).trim();
                }
                if (name && value) decls.push({ name: name.startsWith('--') ? name : name.toLowerCase(), value: value, important: important });
            }
        };
        while (i < n) {
            const j = skipQuotedOrComment(text, i);
            if (j !== i) { i = j; continue; }
            const c = text[i];
            if (c === '(') paren++;
            else if (c === ')') paren = Math.max(0, paren - 1);
            else if (c === ';' && paren === 0) {
                flush(i);
                start = ++i;
                continue;
            } else if (c === '{' && paren === 0) {
                // A nested rule: everything from the statement start to its
                // closing brace.
                let depth = 1;
                let k = i + 1;
                while (k < n && depth > 0) {
                    const m = skipQuotedOrComment(text, k);
                    if (m !== k) { k = m; continue; }
                    if (text[k] === '{') depth++;
                    else if (text[k] === '}') depth--;
                    k++;
                }
                nested += (nested ? ' ' : '') + stripComments(text.slice(start, k)).trim();
                start = i = k;
                continue;
            }
            i++;
        }
        flush(n);
        return { decls: decls, nested: nested };
    }

    const collapse = (s) => s.replace(/\s+/g, ' ').trim();

    // --- CSSStyleDeclaration ---------------------------------------------------

    function camelToCss(name) {
        if (name === 'cssFloat') return 'float';
        if (name.startsWith('webkit')) name = 'Webkit' + name.slice(6);
        return name.replace(/[A-Z]/g, (m) => '-' + m.toLowerCase());
    }

    // A rule's declarations, in order. `onChange` writes the sheet back.
    class CSSStyleDeclaration {
        constructor(decls, parentRule, onChange) {
            this._decls = decls || [];
            this._parentRule = parentRule || null;
            this._onChange = onChange || null;
            // camelCase and dashed property access (style.color,
            // style.backgroundColor, style['background-color']).
            return new Proxy(this, {
                get(target, key, receiver) {
                    if (typeof key !== 'string' || key in target) return Reflect.get(target, key, receiver);
                    if (/^\d+$/.test(key)) return target.item(Number(key));
                    if (/^[a-zA-Z-]+$/.test(key)) return target.getPropertyValue(key.includes('-') ? key : camelToCss(key));
                    return undefined;
                },
                set(target, key, value, receiver) {
                    if (typeof key !== 'string' || key in target || key.startsWith('_')) return Reflect.set(target, key, value, receiver);
                    if (/^[a-zA-Z-]+$/.test(key)) {
                        target.setProperty(key.includes('-') ? key : camelToCss(key), value);
                        return true;
                    }
                    return Reflect.set(target, key, value, receiver);
                },
            });
        }
        get length() { return this._decls.length; }
        get parentRule() { return this._parentRule; }
        item(i) { const d = this._decls[i]; return d ? d.name : ''; }
        _find(name) {
            const n = name.startsWith('--') ? name : String(name).toLowerCase();
            for (let i = 0; i < this._decls.length; i++) if (this._decls[i].name === n) return i;
            return -1;
        }
        getPropertyValue(name) { const i = this._find(name); return i < 0 ? '' : this._decls[i].value; }
        getPropertyPriority(name) { const i = this._find(name); return i >= 0 && this._decls[i].important ? 'important' : ''; }
        setProperty(name, value, priority) {
            name = String(name);
            if (value === null || value === undefined || String(value).trim() === '') {
                this.removeProperty(name);
                return;
            }
            const d = { name: name.startsWith('--') ? name : name.toLowerCase(), value: String(value).trim(),
                        important: String(priority || '').toLowerCase() === 'important' };
            const i = this._find(d.name);
            if (i < 0) this._decls.push(d);
            else this._decls[i] = d;
            if (this._onChange) this._onChange();
        }
        removeProperty(name) {
            const i = this._find(String(name));
            if (i < 0) return '';
            const old = this._decls[i].value;
            this._decls.splice(i, 1);
            if (this._onChange) this._onChange();
            return old;
        }
        get cssText() {
            return this._decls.map((d) => d.name + ': ' + d.value + (d.important ? ' !important' : '') + ';').join(' ');
        }
        set cssText(text) {
            this._decls = splitDeclarations(String(text)).decls;
            if (this._onChange) this._onChange();
        }
        get cssFloat() { return this.getPropertyValue('float'); }
        set cssFloat(v) { this.setProperty('float', v); }
        [Symbol.iterator]() { return this._decls.map((d) => d.name)[Symbol.iterator](); }
    }

    // --- MediaList -----------------------------------------------------------

    class MediaList {
        constructor(text, onChange) {
            this._items = MediaList._split(text);
            this._onChange = onChange || null;
        }
        static _split(text) { return String(text || '').split(',').map(collapse).filter((s) => s.length > 0); }
        get mediaText() { return this._items.join(', '); }
        set mediaText(v) { this._items = MediaList._split(v); if (this._onChange) this._onChange(); }
        get length() { return this._items.length; }
        item(i) { return i >= 0 && i < this._items.length ? this._items[i] : null; }
        appendMedium(m) { m = collapse(String(m)); if (!this._items.includes(m)) { this._items.push(m); if (this._onChange) this._onChange(); } }
        deleteMedium(m) {
            const i = this._items.indexOf(collapse(String(m)));
            if (i < 0) throw domException("Failed to execute 'deleteMedium' on 'MediaList': Failed to delete '" + m + "'.", 'NotFoundError');
            this._items.splice(i, 1);
            if (this._onChange) this._onChange();
        }
        toString() { return this.mediaText; }
        [Symbol.iterator]() { return this._items.slice()[Symbol.iterator](); }
    }

    // --- Rules ---------------------------------------------------------------

    class CSSRuleList {
        constructor(rules) {
            this._rules = rules;
            return new Proxy(this, {
                get(target, key, receiver) {
                    if (typeof key === 'string' && /^\d+$/.test(key)) return target._rules[Number(key)];
                    return Reflect.get(target, key, receiver);
                },
                has(target, key) {
                    if (typeof key === 'string' && /^\d+$/.test(key)) return Number(key) < target._rules.length;
                    return Reflect.has(target, key);
                },
            });
        }
        get length() { return this._rules.length; }
        item(i) { return this._rules[i] || null; }
        [Symbol.iterator]() { return this._rules.slice()[Symbol.iterator](); }
    }

    class CSSRule {
        constructor(sheet, parentRule) {
            this._sheet = sheet;
            this._parentRule = parentRule || null;
        }
        get parentStyleSheet() { return this._sheet; }
        get parentRule() { return this._parentRule; }
        get type() { return 0; }
        get cssText() { return this._text || ''; }
        set cssText(v) { /* ignored, as a browser does */ }
        _changed() { if (this._sheet) this._sheet._commit(); }
    }
    const ruleTypes = { STYLE_RULE: 1, CHARSET_RULE: 2, IMPORT_RULE: 3, MEDIA_RULE: 4, FONT_FACE_RULE: 5, PAGE_RULE: 6,
                        KEYFRAMES_RULE: 7, KEYFRAME_RULE: 8, MARGIN_RULE: 9, NAMESPACE_RULE: 10, COUNTER_STYLE_RULE: 11,
                        SUPPORTS_RULE: 12, FONT_FEATURE_VALUES_RULE: 14 };
    for (const k in ruleTypes) {
        Object.defineProperty(CSSRule, k, { value: ruleTypes[k], enumerable: true });
        Object.defineProperty(CSSRule.prototype, k, { value: ruleTypes[k], enumerable: true });
    }

    class CSSStyleRule extends CSSRule {
        constructor(sheet, parentRule, selector, body) {
            super(sheet, parentRule);
            this._selector = collapse(selector);
            const parts = splitDeclarations(body);
            this._nested = parts.nested;
            this._style = new CSSStyleDeclaration(parts.decls, this, () => this._changed());
        }
        get type() { return 1; }
        get selectorText() { return this._selector; }
        set selectorText(v) {
            v = collapse(String(v));
            if (!v || /[{};]/.test(v)) return;
            this._selector = v;
            this._changed();
        }
        get style() { return this._style; }
        set style(v) { this._style.cssText = String(v); }
        get cssText() {
            const decl = this._style.cssText;
            const inner = [decl, this._nested].filter((s) => s.length > 0).join(' ');
            return this._selector + ' { ' + (inner ? inner + ' ' : '') + '}';
        }
    }

    class CSSGroupingRule extends CSSRule {
        constructor(sheet, parentRule, prelude, body) {
            super(sheet, parentRule);
            this._prelude = collapse(prelude);
            this._rules = parseRuleList(body, sheet, this);
            this._list = new CSSRuleList(this._rules);
        }
        get cssRules() { return this._list; }
        insertRule(text, index) { return insertInto(this._rules, this._sheet, this, text, index); }
        deleteRule(index) { deleteFrom(this._rules, index); this._changed(); }
        _innerText() { return this._rules.map((r) => '  ' + r.cssText).join('\n'); }
        get cssText() {
            const inner = this._innerText();
            return this._prelude + ' {' + (inner ? '\n' + inner + '\n' : ' ') + '}';
        }
    }

    class CSSConditionRule extends CSSGroupingRule {
        get conditionText() { return this._prelude.replace(/^@[-\w]+\s*/, ''); }
    }

    class CSSMediaRule extends CSSConditionRule {
        constructor(sheet, parentRule, prelude, body) {
            super(sheet, parentRule, prelude, body);
            this._media = new MediaList(prelude.replace(/^@media\s*/i, ''), () => {
                this._prelude = '@media ' + this._media.mediaText;
                this._changed();
            });
        }
        get type() { return 4; }
        get media() { return this._media; }
        get conditionText() { return this._media.mediaText; }
    }

    class CSSSupportsRule extends CSSConditionRule {
        get type() { return 12; }
    }

    class CSSKeyframeRule extends CSSRule {
        constructor(sheet, parentRule, keyText, body) {
            super(sheet, parentRule);
            this._key = collapse(keyText);
            this._style = new CSSStyleDeclaration(splitDeclarations(body).decls, this, () => this._changed());
        }
        get type() { return 8; }
        get keyText() { return this._key; }
        set keyText(v) { this._key = collapse(String(v)); this._changed(); }
        get style() { return this._style; }
        get cssText() { const d = this._style.cssText; return this._key + ' { ' + (d ? d + ' ' : '') + '}'; }
    }

    class CSSKeyframesRule extends CSSRule {
        constructor(sheet, parentRule, prelude, body) {
            super(sheet, parentRule);
            this._at = /^@[-\w]+/.exec(prelude)[0];
            this._name = collapse(prelude.slice(this._at.length)).replace(/^["']|["']$/g, '');
            this._rules = splitRules(body).filter((r) => r.body !== null)
                .map((r) => new CSSKeyframeRule(sheet, this, r.prelude, r.body));
            this._list = new CSSRuleList(this._rules);
        }
        get type() { return 7; }
        get name() { return this._name; }
        set name(v) { this._name = String(v); this._changed(); }
        get cssRules() { return this._list; }
        get length() { return this._rules.length; }
        findRule(key) {
            key = collapse(String(key));
            for (let i = this._rules.length - 1; i >= 0; i--) if (this._rules[i].keyText === key) return this._rules[i];
            return null;
        }
        appendRule(text) {
            const parts = splitRules(String(text));
            if (parts.length !== 1 || parts[0].body === null) throw domException("Failed to execute 'appendRule' on 'CSSKeyframesRule': Failed to parse the rule '" + text + "'.", 'SyntaxError');
            this._rules.push(new CSSKeyframeRule(this._sheet, this, parts[0].prelude, parts[0].body));
            this._changed();
        }
        deleteRule(key) {
            const r = this.findRule(key);
            if (!r) return;
            this._rules.splice(this._rules.indexOf(r), 1);
            this._changed();
        }
        get cssText() {
            const inner = this._rules.map((r) => '  ' + r.cssText).join('\n');
            return this._at + ' ' + this._name + ' {' + (inner ? '\n' + inner + '\n' : ' ') + '}';
        }
    }

    class CSSFontFaceRule extends CSSRule {
        constructor(sheet, parentRule, body) {
            super(sheet, parentRule);
            this._style = new CSSStyleDeclaration(splitDeclarations(body).decls, this, () => this._changed());
        }
        get type() { return 5; }
        get style() { return this._style; }
        get cssText() { const d = this._style.cssText; return '@font-face { ' + (d ? d + ' ' : '') + '}'; }
    }

    class CSSImportRule extends CSSRule {
        constructor(sheet, parentRule, prelude) {
            super(sheet, parentRule);
            this._text = collapse(prelude) + ';';
            const rest = prelude.replace(/^@import\s*/i, '');
            const m = /^url\(\s*(['"]?)(.*?)\1\s*\)|^(['"])(.*?)\3/.exec(rest);
            this._href = m ? (m[2] !== undefined ? m[2] : m[4]) : '';
            this._media = new MediaList(m ? rest.slice(m[0].length) : '');
        }
        get type() { return 3; }
        get href() { return this._href; }
        get media() { return this._media; }
        get styleSheet() { return null; }
    }

    // A rule other than these: its text, kept as written.
    class CSSOtherRule extends CSSRule {
        constructor(sheet, parentRule, text) {
            super(sheet, parentRule);
            this._text = text;
        }
    }

    function makeRule(stmt, sheet, parentRule) {
        const p = stmt.prelude;
        if (p[0] !== '@') {
            if (stmt.body === null) return null;
            return new CSSStyleRule(sheet, parentRule, p, stmt.body);
        }
        const at = (/^@([-\w]+)/.exec(p) || ['', ''])[1].toLowerCase();
        if (stmt.body === null) {
            if (at === 'import') return new CSSImportRule(sheet, parentRule, p);
            return new CSSOtherRule(sheet, parentRule, collapse(p) + ';');
        }
        switch (at) {
            case 'media': return new CSSMediaRule(sheet, parentRule, p, stmt.body);
            case 'supports': return new CSSSupportsRule(sheet, parentRule, p, stmt.body);
            case 'keyframes': case '-webkit-keyframes': return new CSSKeyframesRule(sheet, parentRule, p, stmt.body);
            case 'font-face': return new CSSFontFaceRule(sheet, parentRule, stmt.body);
            case 'layer': case 'container': case 'scope': case 'starting-style': case 'document':
                return new CSSGroupingRule(sheet, parentRule, p, stmt.body);
            default: return new CSSOtherRule(sheet, parentRule, collapse(p) + ' { ' + collapse(stripComments(stmt.body)) + ' }');
        }
    }

    function parseRuleList(text, sheet, parentRule) {
        const out = [];
        for (const stmt of splitRules(String(text || ''))) {
            const r = makeRule(stmt, sheet, parentRule);
            if (r) out.push(r);
        }
        return out;
    }

    function insertInto(rules, sheet, parentRule, text, index) {
        index = index === undefined ? 0 : Number(index) >>> 0;
        if (index > rules.length)
            throw domException("Failed to execute 'insertRule' on 'CSSStyleSheet': The index provided (" + index +
                               ') is larger than the maximum index (' + rules.length + ').', 'IndexSizeError');
        const stmts = splitRules(String(text));
        const rule = stmts.length === 1 ? makeRule(stmts[0], sheet, parentRule) : null;
        if (!rule || (rule instanceof CSSStyleRule && !rule.selectorText))
            throw domException("Failed to execute 'insertRule' on 'CSSStyleSheet': Failed to parse the rule '" + text + "'.", 'SyntaxError');
        rules.splice(index, 0, rule);
        if (sheet) sheet._commit();
        return index;
    }

    function deleteFrom(rules, index) {
        index = Number(index) >>> 0;
        if (index >= rules.length)
            throw domException("Failed to execute 'deleteRule' on 'CSSStyleSheet': The index provided (" + index +
                               ') is larger than the maximum index (' + Math.max(0, rules.length - 1) + ').', 'IndexSizeError');
        rules.splice(index, 1);
    }

    // --- Sheets --------------------------------------------------------------

    class StyleSheet {
        get type() { return 'text/css'; }
        get ownerNode() { return this._owner || null; }
        get parentStyleSheet() { return null; }
        get href() {
            const o = this._owner;
            if (!o || o.localName !== 'link') return null;
            const raw = o.getAttribute('href') || '';
            try { return new URL(raw, document.baseURI).href; } catch (e) { return raw; }
        }
        get title() { const o = this._owner; return o && o.getAttribute('title') !== null ? o.getAttribute('title') : null; }
        get media() {
            if (!this._media) {
                const o = this._owner;
                this._media = new MediaList(o ? o.getAttribute('media') || '' : '', null);
            }
            return this._media;
        }
        get disabled() { return this._owner ? hooks.sheetDisabled(this._owner) : !!this._disabled; }
        set disabled(v) {
            if (this._owner) hooks.setSheetDisabled(this._owner, !!v);
            else { this._disabled = !!v; this._pushAdopters(); }
        }
    }

    // The scopes adopting each constructed sheet (null for the document).
    const adopters = new Map();   // sheet -> Set of scopes
    const adoptedOf = new Map();  // scope (null | ShadowRoot) -> array of sheets

    class CSSStyleSheet extends StyleSheet {
        constructor(options) {
            super();
            this._owner = null;
            this._constructed = true;
            this._text = '';
            this._rules = [];
            this._list = new CSSRuleList(this._rules);
            this._disabled = !!(options && options.disabled);
            const m = options && options.media;
            this._media = new MediaList(typeof m === 'string' ? m : (m && m.mediaText) || '', null);
        }
        // An element's sheet: its rules come from the document.
        static _forElement(el) {
            const s = Object.create(CSSStyleSheet.prototype);
            s._owner = el;
            s._constructed = false;
            s._text = null;
            s._rules = [];
            s._list = new CSSRuleList(s._rules);
            return s;
        }
        _sync() {
            if (!this._owner) return;
            const text = hooks.sheetText(this._owner);
            if (text === this._text) return;
            this._text = text;
            this._rules.length = 0;
            for (const r of parseRuleList(text, this, null)) this._rules.push(r);
        }
        _serialize() { return this._rules.map((r) => r.cssText).join('\n'); }
        _commit() {
            const text = this._serialize();
            this._text = text;
            if (this._owner) hooks.setSheetText(this._owner, text);
            else this._pushAdopters();
        }
        _pushAdopters() {
            const scopes = adopters.get(this);
            if (scopes) for (const scope of scopes) pushAdopted(scope);
        }
        get ownerRule() { return null; }
        get cssRules() { this._sync(); return this._list; }
        get rules() { return this.cssRules; }
        insertRule(text, index) {
            this._sync();
            return insertInto(this._rules, this, null, text, index);
        }
        deleteRule(index) {
            this._sync();
            deleteFrom(this._rules, index);
            this._commit();
        }
        addRule(selector, style, index) {
            this._sync();
            if (index === undefined) index = this._rules.length;
            this.insertRule(String(selector) + ' { ' + String(style || '') + ' }', index);
            return -1;
        }
        removeRule(index) { this.deleteRule(index === undefined ? 0 : index); }
        replaceSync(text) {
            if (!this._constructed)
                throw domException("Failed to execute 'replaceSync' on 'CSSStyleSheet': Can't call replaceSync on non-constructed CSSStyleSheets.", 'NotAllowedError');
            this._rules.length = 0;
            // @import is not allowed in a constructed sheet: dropped.
            for (const r of parseRuleList(text, this, null)) if (!(r instanceof CSSImportRule)) this._rules.push(r);
            this._commit();
        }
        replace(text) {
            try {
                this.replaceSync(text);
                return Promise.resolve(this);
            } catch (e) {
                return Promise.reject(e);
            }
        }
    }

    function pushAdopted(scope) {
        const sheets = adoptedOf.get(scope) || [];
        const texts = [];
        for (const s of sheets) {
            if (s._disabled) continue;
            const media = s._media ? s._media.mediaText : '';
            const text = s._serialize();
            texts.push(media ? '@media ' + media + ' {\n' + text + '\n}' : text);
        }
        hooks.setAdopted(scope, texts);
    }

    function setAdopted(scope, value) {
        if (value === null || value === undefined || typeof value[Symbol.iterator] !== 'function')
            throw new TypeError("Failed to set the 'adoptedStyleSheets' property: The provided value cannot be converted to a sequence.");
        const list = Array.from(value);
        for (const s of list) {
            if (!(s instanceof CSSStyleSheet))
                throw new TypeError("Failed to set the 'adoptedStyleSheets' property: Failed to convert value to 'CSSStyleSheet'.");
            if (!s._constructed)
                throw domException("Failed to set the 'adoptedStyleSheets' property: Can't adopt non-constructed stylesheets.", 'NotAllowedError');
        }
        for (const s of adoptedOf.get(scope) || []) {
            const set = adopters.get(s);
            if (set) set.delete(scope);
        }
        for (const s of list) {
            let set = adopters.get(s);
            if (!set) adopters.set(s, set = new Set());
            set.add(scope);
        }
        const kept = adoptedOf.get(scope);
        if (kept) { kept.length = 0; for (const s of list) kept.push(s); }
        else adoptedOf.set(scope, list);
        pushAdopted(scope);
    }

    // An ObservableArray, as Chromium has it: push(), splice() and index
    // writes on the array adopt as assignment does.
    const adoptedViews = new Map();   // scope -> proxy over adoptedOf(scope)
    function getAdopted(scope) {
        let view = adoptedViews.get(scope);
        if (view) return view;
        if (!adoptedOf.get(scope)) adoptedOf.set(scope, []);
        const repush = () => setAdopted(scope, adoptedOf.get(scope).filter((x) => x !== undefined));
        view = new Proxy([], {
            get(_, key) {
                const list = adoptedOf.get(scope);
                const v = Reflect.get(list, key);
                return typeof v === 'function' ? v.bind(view) : v;
            },
            set(_, key, value) {
                const list = adoptedOf.get(scope);
                if (typeof key === 'string' && /^\d+$/.test(key) &&
                    (!(value instanceof CSSStyleSheet) || !value._constructed))
                    throw domException("Failed to set an indexed property on 'ObservableArray<CSSStyleSheet>': Can't adopt non-constructed stylesheets.", 'NotAllowedError');
                list[key] = value;
                repush();
                return true;
            },
            deleteProperty(_, key) { delete adoptedOf.get(scope)[key]; repush(); return true; },
            has(_, key) { return key in adoptedOf.get(scope); },
            ownKeys() { return Reflect.ownKeys(adoptedOf.get(scope)); },
            getOwnPropertyDescriptor(_, key) {
                const d = Reflect.getOwnPropertyDescriptor(adoptedOf.get(scope), key);
                if (d) d.configurable = true;
                return d;
            },
        });
        adoptedViews.set(scope, view);
        return view;
    }

    class StyleSheetList {
        constructor(sheets) {
            this._sheets = sheets;
            return new Proxy(this, {
                get(target, key, receiver) {
                    if (typeof key === 'string' && /^\d+$/.test(key)) return target._sheets[Number(key)];
                    return Reflect.get(target, key, receiver);
                },
            });
        }
        get length() { return this._sheets.length; }
        item(i) { return this._sheets[i] || null; }
        [Symbol.iterator]() { return this._sheets.slice()[Symbol.iterator](); }
    }

    const elementSheets = new WeakMap();
    function sheetOf(el) {
        if (!hooks.hasSheet(el)) return null;
        let s = elementSheets.get(el);
        if (!s) elementSheets.set(el, s = CSSStyleSheet._forElement(el));
        return s;
    }

    function sheetsUnder(root) {
        const out = [];
        for (const el of root.querySelectorAll('style, link')) {
            const s = sheetOf(el);
            if (s) out.push(s);
        }
        return new StyleSheetList(out);
    }

    function accessor(proto, name, get, set) {
        if (!proto) return;
        Object.defineProperty(proto, name, { get: get, set: set, configurable: true, enumerable: true });
    }

    const docProto = g.Document && g.Document.prototype;
    accessor(docProto, 'styleSheets', function () { return sheetsUnder(this); });
    accessor(docProto, 'adoptedStyleSheets', function () { return getAdopted(null); },
             function (v) { setAdopted(null, v); });
    const srProto = g.ShadowRoot && g.ShadowRoot.prototype;
    accessor(srProto, 'styleSheets', function () { return sheetsUnder(this); });
    accessor(srProto, 'adoptedStyleSheets', function () { return getAdopted(this); },
             function (v) { setAdopted(this, v); });
    for (const iface of ['HTMLStyleElement', 'HTMLLinkElement']) {
        const C = g[iface];
        accessor(C && C.prototype, 'sheet', function () { return sheetOf(this); });
    }

    g.CSSStyleSheet = CSSStyleSheet;
    g.StyleSheet = StyleSheet;
    g.StyleSheetList = StyleSheetList;
    g.CSSRuleList = CSSRuleList;
    g.CSSRule = CSSRule;
    g.CSSStyleRule = CSSStyleRule;
    g.CSSGroupingRule = CSSGroupingRule;
    g.CSSConditionRule = CSSConditionRule;
    g.CSSMediaRule = CSSMediaRule;
    g.CSSSupportsRule = CSSSupportsRule;
    g.CSSKeyframesRule = CSSKeyframesRule;
    g.CSSKeyframeRule = CSSKeyframeRule;
    g.CSSFontFaceRule = CSSFontFaceRule;
    g.CSSImportRule = CSSImportRule;
    g.CSSStyleDeclaration = CSSStyleDeclaration;
    // element.style and getComputedStyle()'s host declarations are born on it.
    hooks.declarationProto = CSSStyleDeclaration.prototype;
    g.MediaList = MediaList;
})();
