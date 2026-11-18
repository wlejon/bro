// Intl.Locale (ECMA-402 §14), over the Intl namespace host_intl.cpp installs:
// a Unicode BCP 47 locale identifier parsed and canonicalized (language
// lower case, script title case, region upper case, the -u- keywords
// sorted), its options laid over it (language, script, region, calendar,
// collation, hourCycle, caseFirst, numeric, numberingSystem), and the
// accessors, toString, maximize and minimize (likely subtags for
// the common languages; any other language maximizes to itself). A tag that
// is not well formed is a RangeError, as in Chromium. Shoelace's
// LocalizeController makes one per render.
(function () {
    'use strict';

    const g = globalThis;
    const Intl = g.Intl;
    if (!Intl || Intl.Locale) return;

    const ALPHA = /^[a-z]+$/i;
    const ALNUM = /^[a-z0-9]+$/i;
    const DIGIT = /^[0-9]+$/;
    const KEYS = { calendar: 'ca', collation: 'co', hourCycle: 'hc', caseFirst: 'kf', numeric: 'kn', numberingSystem: 'nu' };
    const HOUR_CYCLES = ['h11', 'h12', 'h23', 'h24'];
    const CASE_FIRST = ['upper', 'lower', 'false'];
    // language -> [script, region] (CLDR likely subtags, the common ones).
    const LIKELY = {
        en: ['Latn', 'US'], fr: ['Latn', 'FR'], de: ['Latn', 'DE'], es: ['Latn', 'ES'], it: ['Latn', 'IT'],
        pt: ['Latn', 'BR'], nl: ['Latn', 'NL'], sv: ['Latn', 'SE'], da: ['Latn', 'DK'], nb: ['Latn', 'NO'],
        fi: ['Latn', 'FI'], pl: ['Latn', 'PL'], cs: ['Latn', 'CZ'], tr: ['Latn', 'TR'], hu: ['Latn', 'HU'],
        ro: ['Latn', 'RO'], id: ['Latn', 'ID'], vi: ['Latn', 'VN'], ru: ['Cyrl', 'RU'], uk: ['Cyrl', 'UA'],
        bg: ['Cyrl', 'BG'], el: ['Grek', 'GR'], he: ['Hebr', 'IL'], ar: ['Arab', 'EG'], fa: ['Arab', 'IR'],
        hi: ['Deva', 'IN'], th: ['Thai', 'TH'], ja: ['Jpan', 'JP'], ko: ['Kore', 'KR'], zh: ['Hans', 'CN'],
    };

    const state = new WeakMap();

    function bad(tag) {
        return new RangeError('Incorrect locale information provided' + (tag === undefined ? '' : ': ' + tag));
    }

    // unicode_locale_id -> {language, script, region, variants, keywords, other}
    function parse(tag) {
        if (typeof tag !== 'string' || tag.length === 0) throw bad(tag);
        const parts = tag.split('-');
        let i = 0;
        const lang = parts[i++];
        if (!lang || !ALPHA.test(lang) || !(lang.length >= 2 && lang.length <= 3 || lang.length >= 5 && lang.length <= 8)) throw bad(tag);
        const r = { language: lang.toLowerCase(), script: undefined, region: undefined, variants: [], keywords: {}, other: [] };
        if (i < parts.length && parts[i].length === 4 && ALPHA.test(parts[i])) {
            const s = parts[i++].toLowerCase();
            r.script = s[0].toUpperCase() + s.slice(1);
        }
        if (i < parts.length && (parts[i].length === 2 && ALPHA.test(parts[i]) || parts[i].length === 3 && DIGIT.test(parts[i])))
            r.region = parts[i++].toUpperCase();
        while (i < parts.length && ALNUM.test(parts[i]) &&
               (parts[i].length >= 5 && parts[i].length <= 8 || parts[i].length === 4 && /^[0-9]/.test(parts[i]))) {
            const v = parts[i++].toLowerCase();
            if (r.variants.includes(v)) throw bad(tag);
            r.variants.push(v);
        }
        const seen = new Set();
        while (i < parts.length) {
            const single = parts[i++].toLowerCase();
            if (single.length !== 1 || !ALNUM.test(single) || seen.has(single)) throw bad(tag);
            seen.add(single);
            if (single === 'x') {
                const rest = parts.slice(i);
                if (!rest.length || rest.some((p) => !ALNUM.test(p) || p.length < 1 || p.length > 8)) throw bad(tag);
                r.other.push('x-' + rest.join('-').toLowerCase());
                i = parts.length;
                break;
            }
            const sub = [];
            while (i < parts.length && parts[i].length > 1) {
                if (!ALNUM.test(parts[i]) || parts[i].length > 8) throw bad(tag);
                sub.push(parts[i++].toLowerCase());
            }
            if (!sub.length) throw bad(tag);
            if (single !== 'u') {
                r.other.push(single + '-' + sub.join('-'));
                continue;
            }
            // -u-: attributes, then key (2 chars) [type...]
            let k = 0;
            const attrs = [];
            while (k < sub.length && sub[k].length >= 3) attrs.push(sub[k++]);
            if (attrs.length) r.other.push('u-' + attrs.join('-'));
            while (k < sub.length) {
                const key = sub[k++];
                if (key.length !== 2 || !/^[a-z0-9][a-z]$/.test(key)) throw bad(tag);
                const types = [];
                while (k < sub.length && sub[k].length >= 3) types.push(sub[k++]);
                if (!(key in r.keywords)) r.keywords[key] = types.length ? types.join('-') : 'true';
            }
        }
        return r;
    }

    function baseName(r) {
        return [r.language, r.script, r.region].concat(r.variants).filter((x) => x !== undefined).join('-');
    }

    function serialize(r) {
        let s = baseName(r);
        const keys = Object.keys(r.keywords).sort();
        const ext = r.other.filter((e) => !e.startsWith('x-') && e < 'u');
        const after = r.other.filter((e) => !e.startsWith('x-') && e > 'u' && !e.startsWith('u-'));
        const uattrs = r.other.filter((e) => e.startsWith('u-'));
        for (const e of ext) s += '-' + e;
        if (keys.length || uattrs.length) {
            s += '-u';
            for (const a of uattrs) s += '-' + a.slice(2);
            for (const k of keys) s += '-' + k + (r.keywords[k] === 'true' ? '' : '-' + r.keywords[k]);
        }
        for (const e of after) s += '-' + e;
        for (const e of r.other.filter((e) => e.startsWith('x-'))) s += '-' + e;
        return s;
    }

    function stringOption(options, name, allowed) {
        const v = options[name];
        if (v === undefined) return undefined;
        const s = String(v);
        if (allowed && !allowed.includes(s)) throw new RangeError('Value ' + s + ' out of range for Intl.Locale options property ' + name);
        return s;
    }

    function rOf(self) {
        const r = state.get(self);
        if (!r) throw new TypeError('Method Intl.Locale.prototype called on an incompatible receiver');
        return r;
    }

    function fromState(r) {
        const loc = Object.create(Locale.prototype);
        state.set(loc, r);
        return loc;
    }

    function copy(r) {
        return { language: r.language, script: r.script, region: r.region, variants: r.variants.slice(),
                 keywords: Object.assign({}, r.keywords), other: r.other.slice() };
    }

    class Locale {
        constructor(tag, options) {
            if (typeof tag !== 'string' && (typeof tag !== 'object' || tag === null))
                throw new TypeError('First argument to Intl.Locale constructor can\'t be empty or missing');
            const r = state.has(tag) ? copy(state.get(tag)) : parse(String(tag));
            const o = options === undefined ? {} : Object(options);
            const lang = stringOption(o, 'language');
            if (lang !== undefined) {
                if (!ALPHA.test(lang) || !(lang.length >= 2 && lang.length <= 3 || lang.length >= 5 && lang.length <= 8)) throw bad(lang);
                r.language = lang.toLowerCase();
            }
            const script = stringOption(o, 'script');
            if (script !== undefined) {
                if (script.length !== 4 || !ALPHA.test(script)) throw bad(script);
                r.script = script[0].toUpperCase() + script.slice(1).toLowerCase();
            }
            const region = stringOption(o, 'region');
            if (region !== undefined) {
                if (!(region.length === 2 && ALPHA.test(region) || region.length === 3 && DIGIT.test(region))) throw bad(region);
                r.region = region.toUpperCase();
            }
            const values = {
                calendar: stringOption(o, 'calendar'), collation: stringOption(o, 'collation'),
                hourCycle: stringOption(o, 'hourCycle', HOUR_CYCLES), caseFirst: stringOption(o, 'caseFirst', CASE_FIRST),
                numeric: o.numeric === undefined ? undefined : String(Boolean(o.numeric)),
                numberingSystem: stringOption(o, 'numberingSystem'),
            };
            for (const name of Object.keys(KEYS)) {
                const v = values[name];
                if (v === undefined) continue;
                if (!/^[a-z0-9]{3,8}(-[a-z0-9]{3,8})*$/i.test(v) && !(name === 'numeric' || name === 'caseFirst')) throw bad(v);
                r.keywords[KEYS[name]] = v.toLowerCase();
            }
            state.set(this, r);
        }

        get baseName() { return baseName(rOf(this)); }
        get language() { return rOf(this).language; }
        get script() { return rOf(this).script; }
        get region() { return rOf(this).region; }
        get calendar() { return rOf(this).keywords.ca; }
        get collation() { return rOf(this).keywords.co; }
        get hourCycle() { return rOf(this).keywords.hc; }
        get caseFirst() { const v = rOf(this).keywords.kf; return v === 'true' ? '' : v; }
        get numeric() { const v = rOf(this).keywords.kn; return v === 'true' || v === ''; }
        get numberingSystem() { return rOf(this).keywords.nu; }

        toString() { return serialize(rOf(this)); }

        maximize() {
            const r = copy(rOf(this));
            const likely = LIKELY[r.language];
            if (likely) {
                if (r.script === undefined) r.script = likely[0];
                if (r.region === undefined) r.region = likely[1];
            }
            return fromState(r);
        }

        minimize() {
            const r = copy(rOf(this));
            const likely = LIKELY[r.language];
            if (likely) {
                if (r.script === likely[0]) r.script = undefined;
                if (r.region === likely[1] && (r.script === undefined)) r.region = undefined;
            }
            return fromState(r);
        }
    }

    Object.defineProperty(Locale.prototype, Symbol.toStringTag, { value: 'Intl.Locale', configurable: true });
    Object.defineProperty(Intl, 'Locale', { value: Locale, writable: true, configurable: true });
})();
