// Intl — a working subset of ECMA-402 for an engine that has none.
//
// QuickJS ships no Intl at all, so `new Intl.NumberFormat(...)` is a
// ReferenceError rather than a formatting difference. That is not a rounding
// error in a page's output, it is the page not running: the three.js editor
// dies constructing its viewport info panel, and any app that formats a number
// for display dies with it.
//
// So this is deliberately a *subset*, and says so. It covers the parts pages
// actually reach for — grouping and fraction digits, plural category
// selection, locale-aware comparison, date/time and list and relative-time
// formatting, and language/region display names — with real CLDR behaviour for
// the major locale families and a sane fallback elsewhere. It is not a
// substitute for ICU: no scripts beyond the separators below, no calendars but
// Gregorian, no `scopes`-deep option surface. A page that needs true CLDR
// should ship its own data; a page that needs "1,234 vertices" gets it here.
//
// Installed only when the engine has no native Intl, so a QuickJS built with
// ECMA-402 one day wins automatically.

(function () {
'use strict';

if (typeof globalThis.Intl !== 'undefined') return;

// ---------------------------------------------------------------------------
// Locale plumbing
// ---------------------------------------------------------------------------

// Normalize a locale argument to a single BCP-47-ish tag. The spec negotiates
// over a list against what the implementation supports; we support everything
// approximately, so the first entry wins.
function pickLocale(locales) {
    if (locales === undefined) return 'en-US';
    if (typeof locales === 'string') return canonicalize(locales);
    if (Array.isArray(locales) && locales.length) return canonicalize(String(locales[0]));
    if (locales && typeof locales === 'object') return canonicalize(String(locales));
    return 'en-US';
}

// lowercase language, Titlecase script, UPPERCASE region — the canonical
// casing, which is what resolvedOptions() is expected to report back.
function canonicalize(tag) {
    const parts = String(tag).replace(/_/g, '-').split('-');
    if (!parts[0]) return 'en-US';
    parts[0] = parts[0].toLowerCase();
    for (let i = 1; i < parts.length; i++) {
        const p = parts[i];
        if (p.length === 4) parts[i] = p[0].toUpperCase() + p.slice(1).toLowerCase();
        else if (p.length === 2 || p.length === 3) parts[i] = p.toUpperCase();
        else parts[i] = p.toLowerCase();
    }
    return parts.join('-');
}

function languageOf(tag) { return String(tag).split('-')[0].toLowerCase(); }
function regionOf(tag) {
    const parts = String(tag).split('-');
    for (let i = 1; i < parts.length; i++)
        if (parts[i].length === 2 && /^[A-Za-z]{2}$/.test(parts[i])) return parts[i].toUpperCase();
    return '';
}

// ---------------------------------------------------------------------------
// Number symbols
//
// Which characters separate groups and the fraction, by language. This is the
// difference that actually shows up on screen — "1,234.5" against "1.234,5" —
// and it is a short table because the world only uses a few conventions.
// ---------------------------------------------------------------------------

const NBSP = ' ';        // no-break space
const NNBSP = ' ';       // narrow no-break space, what fr-FR uses today

const SYMBOLS = {
    // group '.', decimal ','
    de: ['.', ','], es: ['.', ','], it: ['.', ','], pt: ['.', ','], nl: ['.', ','],
    id: ['.', ','], tr: ['.', ','], da: ['.', ','], ro: ['.', ','], el: ['.', ','],
    vi: ['.', ','], hr: ['.', ','], sl: ['.', ','],
    // group ' ' (no-break), decimal ','
    fr: [NNBSP, ','], ru: [NBSP, ','], pl: [NBSP, ','], cs: [NBSP, ','],
    sv: [NBSP, ','], fi: [NBSP, ','], nb: [NBSP, ','], no: [NBSP, ','],
    uk: [NBSP, ','], sk: [NBSP, ','], bg: [NBSP, ','], lv: [NBSP, ','],
    lt: [NBSP, ','], et: [NBSP, ','], hu: [NBSP, ','],
    // apostrophe grouping
    // (de-CH and it-CH; handled by region below, listed for completeness)
};

function symbolsFor(locale) {
    if (regionOf(locale) === 'CH') return ['’', '.'];
    const s = SYMBOLS[languageOf(locale)];
    return s ? s : [',', '.'];   // en and everything unlisted
}

// India groups the thousands then in pairs: 1,23,45,678. It is the one
// grouping shape that is not "every three digits".
function usesIndianGrouping(locale) {
    const lang = languageOf(locale), region = regionOf(locale);
    if (region === 'IN') return true;
    return ['hi', 'bn', 'gu', 'kn', 'ml', 'mr', 'pa', 'ta', 'te', 'ur'].indexOf(lang) >= 0 &&
           region === '';
}

function groupDigits(intDigits, sep, indian) {
    if (!sep) return intDigits;
    if (!indian) {
        let out = '';
        for (let i = 0; i < intDigits.length; i++) {
            if (i > 0 && (intDigits.length - i) % 3 === 0) out += sep;
            out += intDigits[i];
        }
        return out;
    }
    if (intDigits.length <= 3) return intDigits;
    const head = intDigits.slice(0, intDigits.length - 3);
    const tail = intDigits.slice(intDigits.length - 3);
    let out = '';
    for (let i = 0; i < head.length; i++) {
        if (i > 0 && (head.length - i) % 2 === 0) out += sep;
        out += head[i];
    }
    return out + sep + tail;
}

// ---------------------------------------------------------------------------
// Intl.NumberFormat
// ---------------------------------------------------------------------------

const CURRENCY_SYMBOL = {
    USD: '$', EUR: '€', GBP: '£', JPY: '¥', CNY: 'CN¥',
    KRW: '₩', INR: '₹', RUB: '₽', BRL: 'R$', CAD: 'CA$',
    AUD: 'A$', CHF: 'CHF', SEK: 'SEK', NOK: 'NOK', DKK: 'DKK', PLN: 'zł',
};
// Currencies whose minor unit is not 1/100.
const CURRENCY_DIGITS = { JPY: 0, KRW: 0, VND: 0, CLP: 0, ISK: 0, BHD: 3, KWD: 3, OMR: 3, TND: 3 };

function NumberFormat(locales, options) {
    if (!(this instanceof NumberFormat)) return new NumberFormat(locales, options);
    const opts = options || {};
    const locale = pickLocale(locales);
    const style = opts.style || 'decimal';
    const currency = opts.currency ? String(opts.currency).toUpperCase() : undefined;

    if (style === 'currency' && !currency)
        throw new TypeError('Currency code is required with currency style');

    const defaultMinFrac = style === 'currency'
        ? (CURRENCY_DIGITS[currency] !== undefined ? CURRENCY_DIGITS[currency] : 2)
        : 0;
    const defaultMaxFrac = style === 'currency' ? defaultMinFrac
                         : style === 'percent'  ? 0
                         : 3;

    const minFrac = opts.minimumFractionDigits !== undefined
        ? clampDigits(opts.minimumFractionDigits) : defaultMinFrac;
    let maxFrac = opts.maximumFractionDigits !== undefined
        ? clampDigits(opts.maximumFractionDigits) : Math.max(minFrac, defaultMaxFrac);
    if (maxFrac < minFrac) maxFrac = minFrac;

    this._locale = locale;
    this._style = style;
    this._currency = currency;
    this._currencyDisplay = opts.currencyDisplay || 'symbol';
    this._minInt = opts.minimumIntegerDigits !== undefined
        ? clampDigits(opts.minimumIntegerDigits) : 1;
    this._minFrac = minFrac;
    this._maxFrac = maxFrac;
    this._signDisplay = opts.signDisplay || 'auto';
    // `useGrouping` went tri-state in a later edition ('auto' | 'always' |
    // 'min2' | false); accept the strings, treat anything truthy as on.
    this._grouping = opts.useGrouping === undefined ? true
                   : opts.useGrouping === false || opts.useGrouping === 'false' ? false
                   : true;
    this._min2 = opts.useGrouping === 'min2';
    const sym = symbolsFor(locale);
    this._group = sym[0];
    this._decimal = sym[1];
    this._indian = usesIndianGrouping(locale);
}

function clampDigits(v) {
    const n = Math.floor(Number(v));
    if (!isFinite(n) || n < 0) return 0;
    return n > 21 ? 21 : n;
}

NumberFormat.prototype.format = function (value) {
    return this.formatToParts(value).map(p => p.value).join('');
};

NumberFormat.prototype.formatToParts = function (value) {
    let n = Number(value);
    const parts = [];

    if (typeof value === 'bigint') n = Number(value);
    if (Number.isNaN(n)) return [{ type: 'nan', value: 'NaN' }];

    const negative = n < 0 || Object.is(n, -0);
    let abs = Math.abs(n);
    if (this._style === 'percent') abs *= 100;

    const wantSign = this._signDisplay === 'always' ||
                     (this._signDisplay !== 'never' && negative);
    if (wantSign) parts.push({ type: negative ? 'minusSign' : 'plusSign',
                               value: negative ? '-' : '+' });

    if (this._style === 'currency' && this._currencyDisplay !== 'code' &&
        !isPrefixAfterNumber(this._locale)) {
        parts.push({ type: 'currency', value: currencyText(this) });
    } else if (this._style === 'currency' && this._currencyDisplay === 'code' &&
               !isPrefixAfterNumber(this._locale)) {
        parts.push({ type: 'currency', value: this._currency });
        parts.push({ type: 'literal', value: NBSP });
    }

    if (!isFinite(abs)) {
        parts.push({ type: 'infinity', value: '∞' });
    } else {
        // toFixed does the rounding, then trailing zeros come off down to the
        // minimum. Doing it the other way (round, then pad) loses digits on
        // values toFixed represents better than a manual scale-and-round.
        let fixed = abs.toFixed(this._maxFrac);
        let [intPart, fracPart = ''] = fixed.split('.');
        while (fracPart.length > this._minFrac && fracPart.endsWith('0'))
            fracPart = fracPart.slice(0, -1);
        while (intPart.length < this._minInt) intPart = '0' + intPart;

        const doGroup = this._grouping && !(this._min2 && intPart.length <= 4);
        const grouped = doGroup
            ? groupDigits(intPart, this._group, this._indian)
            : intPart;

        // Emit integer/group parts separately: formatToParts callers style them.
        let buf = '';
        for (let i = 0; i < grouped.length; i++) {
            const c = grouped[i];
            if (c === this._group || (this._group.length > 1 &&
                grouped.substr(i, this._group.length) === this._group)) {
                if (buf) { parts.push({ type: 'integer', value: buf }); buf = ''; }
                parts.push({ type: 'group', value: this._group });
                i += this._group.length - 1;
                continue;
            }
            buf += c;
        }
        if (buf) parts.push({ type: 'integer', value: buf });

        if (fracPart) {
            parts.push({ type: 'decimal', value: this._decimal });
            parts.push({ type: 'fraction', value: fracPart });
        }
    }

    if (this._style === 'percent') {
        if (languageOf(this._locale) === 'fr' || this._group === NBSP)
            parts.push({ type: 'literal', value: NBSP });
        parts.push({ type: 'percentSign', value: '%' });
    }
    if (this._style === 'currency' && isPrefixAfterNumber(this._locale)) {
        parts.push({ type: 'literal', value: NBSP });
        parts.push({ type: 'currency',
                    value: this._currencyDisplay === 'code' ? this._currency
                                                            : currencyText(this) });
    }
    if (this._style === 'unit' ) {
        parts.push({ type: 'literal', value: ' ' });
        parts.push({ type: 'unit', value: String(this._unit || '') });
    }
    return parts;
};

function currencyText(nf) {
    if (nf._currencyDisplay === 'name') return nf._currency;
    return CURRENCY_SYMBOL[nf._currency] || nf._currency;
}

// Most of Europe writes the currency after the amount; English and a few
// others write it before.
function isPrefixAfterNumber(locale) {
    const lang = languageOf(locale);
    return ['en', 'ja', 'ko', 'zh', 'he', 'ga'].indexOf(lang) < 0;
}

NumberFormat.prototype.resolvedOptions = function () {
    return {
        locale: this._locale,
        numberingSystem: 'latn',
        style: this._style,
        currency: this._currency,
        minimumIntegerDigits: this._minInt,
        minimumFractionDigits: this._minFrac,
        maximumFractionDigits: this._maxFrac,
        useGrouping: this._grouping,
        notation: 'standard',
        signDisplay: this._signDisplay,
    };
};

NumberFormat.supportedLocalesOf = function (locales) {
    if (locales === undefined) return [];
    return (typeof locales === 'string' ? [locales] : Array.from(locales)).map(canonicalize);
};

// ---------------------------------------------------------------------------
// Intl.PluralRules
//
// CLDR cardinal categories for the families that differ. Everything unlisted
// gets the English rule, which is the single most common shape.
// ---------------------------------------------------------------------------

// Languages with no plural distinction at all.
const NO_PLURAL = ['ja', 'zh', 'ko', 'th', 'vi', 'id', 'ms', 'lo', 'my', 'km', 'yo'];
// Languages where 0 and 1 are both "one".
const ZERO_IS_ONE = ['fr', 'hi', 'fa', 'am', 'bn', 'gu', 'kn', 'mr', 'zu', 'pt'];

function PluralRules(locales, options) {
    if (!(this instanceof PluralRules)) return new PluralRules(locales, options);
    this._locale = pickLocale(locales);
    this._type = (options && options.type) || 'cardinal';
}

PluralRules.prototype.select = function (value) {
    const n = Math.abs(Number(value));
    const lang = languageOf(this._locale);
    if (this._type === 'ordinal') return selectOrdinal(lang, n);

    if (NO_PLURAL.indexOf(lang) >= 0) return 'other';

    const i = Math.floor(n);
    const hasFraction = n !== i;

    if (ZERO_IS_ONE.indexOf(lang) >= 0) return (i === 0 || i === 1) ? 'one' : 'other';

    if (lang === 'ru' || lang === 'uk' || lang === 'be') {
        if (hasFraction) return 'other';
        const mod10 = i % 10, mod100 = i % 100;
        if (mod10 === 1 && mod100 !== 11) return 'one';
        if (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14)) return 'few';
        return 'many';
    }
    if (lang === 'pl') {
        if (hasFraction) return 'other';
        const mod10 = i % 10, mod100 = i % 100;
        if (i === 1) return 'one';
        if (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14)) return 'few';
        return 'many';
    }
    if (lang === 'cs' || lang === 'sk') {
        if (hasFraction) return 'many';
        if (i === 1) return 'one';
        if (i >= 2 && i <= 4) return 'few';
        return 'other';
    }
    if (lang === 'ar') {
        if (n === 0) return 'zero';
        if (n === 1) return 'one';
        if (n === 2) return 'two';
        const mod100 = i % 100;
        if (mod100 >= 3 && mod100 <= 10) return 'few';
        if (mod100 >= 11 && mod100 <= 99) return 'many';
        return 'other';
    }
    if (lang === 'lt') {
        if (hasFraction) return 'many';
        const mod10 = i % 10, mod100 = i % 100;
        if (mod10 === 1 && (mod100 < 11 || mod100 > 19)) return 'one';
        if (mod10 >= 2 && mod10 <= 9 && (mod100 < 11 || mod100 > 19)) return 'few';
        return 'other';
    }
    // English and friends: exactly 1, with no visible fraction.
    return (n === 1 && !hasFraction) ? 'one' : 'other';
};

function selectOrdinal(lang, n) {
    if (lang !== 'en') return 'other';
    const mod10 = n % 10, mod100 = n % 100;
    if (mod10 === 1 && mod100 !== 11) return 'one';
    if (mod10 === 2 && mod100 !== 12) return 'two';
    if (mod10 === 3 && mod100 !== 13) return 'few';
    return 'other';
}

PluralRules.prototype.resolvedOptions = function () {
    return { locale: this._locale, type: this._type, pluralCategories: ['one', 'other'] };
};
PluralRules.supportedLocalesOf = NumberFormat.supportedLocalesOf;

// ---------------------------------------------------------------------------
// Intl.Collator
// ---------------------------------------------------------------------------

function Collator(locales, options) {
    if (!(this instanceof Collator)) return new Collator(locales, options);
    const opts = options || {};
    this._locale = pickLocale(locales);
    this._numeric = !!opts.numeric;
    this._sensitivity = opts.sensitivity || 'variant';
    const self = this;
    // Bound, because `collator.compare` is routinely passed straight to sort().
    this.compare = function (a, b) { return compareWith(self, String(a), String(b)); };
}

// `base` and `case` treat "é" and "e" as the same letter. Decomposing to NFD
// and dropping the combining marks is the cheap way to get there and covers
// the Latin accents a page actually sorts. Guarded, because normalize() is
// only there when the engine was built with the Unicode tables.
const CAN_NORMALIZE = typeof ''.normalize === 'function';
function foldAccents(s) {
    if (!CAN_NORMALIZE) return s;
    return s.normalize('NFD').replace(/[\u0300-\u036f]/g, '');
}

function compareWith(c, a, b) {
    let x = a, y = b;
    // base:   case- and accent-insensitive
    // accent: accents count, case does not
    // case:   case counts, accents do not
    if (c._sensitivity === 'base' || c._sensitivity === 'accent') {
        x = x.toLowerCase(); y = y.toLowerCase();
    }
    if (c._sensitivity === 'base' || c._sensitivity === 'case') {
        x = foldAccents(x); y = foldAccents(y);
    }
    if (c._numeric) {
        // Split each string into digit and non-digit runs and compare run by
        // run, so "item10" sorts after "item9".
        const rx = x.match(/\d+|\D+/g) || [];
        const ry = y.match(/\d+|\D+/g) || [];
        for (let i = 0; i < Math.min(rx.length, ry.length); i++) {
            const dx = /^\d/.test(rx[i]), dy = /^\d/.test(ry[i]);
            if (dx && dy) {
                const nx = Number(rx[i]), ny = Number(ry[i]);
                if (nx !== ny) return nx < ny ? -1 : 1;
            } else if (rx[i] !== ry[i]) {
                return rx[i] < ry[i] ? -1 : 1;
            }
        }
        return rx.length === ry.length ? 0 : (rx.length < ry.length ? -1 : 1);
    }
    if (x === y) return 0;
    return x < y ? -1 : 1;
}

Collator.prototype.resolvedOptions = function () {
    return { locale: this._locale, usage: 'sort', sensitivity: this._sensitivity,
             numeric: this._numeric, collation: 'default' };
};
Collator.supportedLocalesOf = NumberFormat.supportedLocalesOf;

// ---------------------------------------------------------------------------
// Intl.DateTimeFormat
// ---------------------------------------------------------------------------

const MONTHS_LONG = ['January','February','March','April','May','June','July',
                     'August','September','October','November','December'];
const DAYS_LONG = ['Sunday','Monday','Tuesday','Wednesday','Thursday','Friday','Saturday'];

function DateTimeFormat(locales, options) {
    if (!(this instanceof DateTimeFormat)) return new DateTimeFormat(locales, options);
    this._locale = pickLocale(locales);
    this._opts = options || {};
    // No date and no time component means "date only", per the spec's defaults.
    const o = this._opts;
    this._anyDate = o.year || o.month || o.day || o.weekday || o.dateStyle;
    this._anyTime = o.hour || o.minute || o.second || o.timeStyle;
    if (!this._anyDate && !this._anyTime) this._anyDate = true;
    // 12-hour clock outside the places that write 24.
    const lang = languageOf(this._locale);
    this._hour12 = o.hour12 !== undefined ? !!o.hour12
                 : ['en', 'ar', 'hi', 'ko', 'ja'].indexOf(lang) >= 0;
    const self = this;
    this.format = function (d) { return formatDateTime(self, d); };
}

function two(n) { return n < 10 ? '0' + n : String(n); }

function formatDateTime(f, value) {
    const d = value === undefined ? new Date()
            : (value instanceof Date ? value : new Date(Number(value)));
    if (isNaN(d.getTime())) return 'Invalid Date';
    const o = f._opts;
    const lang = languageOf(f._locale);
    // Order: US writes month first, most everywhere else writes day first.
    const monthFirst = lang === 'en' && regionOf(f._locale) !== 'GB';
    const isoOrder = ['ja', 'zh', 'ko', 'hu', 'lt'].indexOf(lang) >= 0;

    const out = [];
    if (f._anyDate) {
        if (o.weekday) out.push(DAYS_LONG[d.getDay()] + ',');
        const y = String(d.getFullYear());
        const mNum = d.getMonth() + 1;
        const day = d.getDate();
        let datePart;
        if (o.month === 'long' || o.dateStyle === 'long' || o.dateStyle === 'full') {
            datePart = monthFirst ? MONTHS_LONG[d.getMonth()] + ' ' + day + ', ' + y
                                  : day + ' ' + MONTHS_LONG[d.getMonth()] + ' ' + y;
        } else if (isoOrder) {
            datePart = y + '/' + two(mNum) + '/' + two(day);
        } else if (monthFirst) {
            datePart = mNum + '/' + day + '/' + y;
        } else {
            datePart = day + '/' + mNum + '/' + y;
        }
        out.push(datePart);
    }
    if (f._anyTime) {
        let h = d.getHours();
        let suffix = '';
        if (f._hour12) {
            suffix = h < 12 ? ' AM' : ' PM';
            h = h % 12; if (h === 0) h = 12;
        }
        let t = (f._hour12 ? String(h) : two(h)) + ':' + two(d.getMinutes());
        if (o.second || o.timeStyle === 'medium' || o.timeStyle === 'long' ||
            o.timeStyle === 'full')
            t += ':' + two(d.getSeconds());
        out.push(t + suffix);
    }
    return out.join(' ');
}

DateTimeFormat.prototype.formatToParts = function (d) {
    return [{ type: 'literal', value: this.format(d) }];
};
DateTimeFormat.prototype.resolvedOptions = function () {
    return { locale: this._locale, calendar: 'gregory', numberingSystem: 'latn',
             timeZone: 'UTC', hour12: this._hour12 };
};
DateTimeFormat.supportedLocalesOf = NumberFormat.supportedLocalesOf;

// ---------------------------------------------------------------------------
// Intl.ListFormat / Intl.RelativeTimeFormat
// ---------------------------------------------------------------------------

function ListFormat(locales, options) {
    if (!(this instanceof ListFormat)) return new ListFormat(locales, options);
    this._locale = pickLocale(locales);
    const o = options || {};
    this._type = o.type || 'conjunction';
    this._style = o.style || 'long';
}
ListFormat.prototype.format = function (list) {
    const items = Array.from(list || []).map(String);
    if (items.length === 0) return '';
    if (items.length === 1) return items[0];
    const word = this._type === 'disjunction' ? 'or' : 'and';
    if (this._type === 'unit') return items.join(', ');
    if (items.length === 2) return items[0] + ' ' + word + ' ' + items[1];
    return items.slice(0, -1).join(', ') + ', ' + word + ' ' + items[items.length - 1];
};
ListFormat.prototype.formatToParts = function (list) {
    return [{ type: 'literal', value: this.format(list) }];
};
ListFormat.prototype.resolvedOptions = function () {
    return { locale: this._locale, type: this._type, style: this._style };
};
ListFormat.supportedLocalesOf = NumberFormat.supportedLocalesOf;

function RelativeTimeFormat(locales, options) {
    if (!(this instanceof RelativeTimeFormat)) return new RelativeTimeFormat(locales, options);
    this._locale = pickLocale(locales);
    const o = options || {};
    this._numeric = o.numeric || 'always';
}
RelativeTimeFormat.prototype.format = function (value, unit) {
    const n = Number(value);
    let u = String(unit);
    if (u.endsWith('s')) u = u.slice(0, -1);
    if (this._numeric === 'auto') {
        if (n === 0) return u === 'day' ? 'today' : 'this ' + u;
        if (n === 1 && u === 'day') return 'tomorrow';
        if (n === -1 && u === 'day') return 'yesterday';
    }
    const abs = Math.abs(n);
    const plural = abs === 1 ? u : u + 's';
    return n < 0 ? abs + ' ' + plural + ' ago' : 'in ' + abs + ' ' + plural;
};
RelativeTimeFormat.prototype.formatToParts = function (v, u) {
    return [{ type: 'literal', value: this.format(v, u) }];
};
RelativeTimeFormat.prototype.resolvedOptions = function () {
    return { locale: this._locale, numeric: this._numeric, style: 'long' };
};
RelativeTimeFormat.supportedLocalesOf = NumberFormat.supportedLocalesOf;

// ---------------------------------------------------------------------------
// Intl.DisplayNames
//
// Two tables: what a language calls itself (the endonym), and its English
// name. A language picker — the overwhelmingly common use — asks for a
// locale's name *in that locale*, so the endonym is exactly what it wants;
// anything else falls back to the English name, then to the code itself,
// which is the spec's `fallback: 'code'` behaviour.
// ---------------------------------------------------------------------------

const ENDONYM = {
    en: 'English', fr: 'français', es: 'español', de: 'Deutsch', it: 'italiano',
    pt: 'português', nl: 'Nederlands', sv: 'svenska', da: 'dansk', nb: 'norsk bokmål',
    fi: 'suomi', pl: 'polski', cs: 'čeština', sk: 'slovenčina', hu: 'magyar',
    ro: 'română', el: 'Ελληνικά', ru: 'русский', uk: 'українська', bg: 'български',
    sr: 'српски', hr: 'hrvatski', tr: 'Türkçe', he: 'עברית', ar: 'العربية',
    fa: 'فارسی', ur: 'اردو', hi: 'हिन्दी', bn: 'বাংলা', ta: 'தமிழ்', te: 'తెలుగు',
    th: 'ไทย', vi: 'Tiếng Việt', id: 'Indonesia', ms: 'Melayu',
    ja: '日本語', ko: '한국어', zh: '中文',
};

const ENGLISH_NAME = {
    en: 'English', fr: 'French', es: 'Spanish', de: 'German', it: 'Italian',
    pt: 'Portuguese', nl: 'Dutch', sv: 'Swedish', da: 'Danish', nb: 'Norwegian Bokmål',
    fi: 'Finnish', pl: 'Polish', cs: 'Czech', sk: 'Slovak', hu: 'Hungarian',
    ro: 'Romanian', el: 'Greek', ru: 'Russian', uk: 'Ukrainian', bg: 'Bulgarian',
    sr: 'Serbian', hr: 'Croatian', tr: 'Turkish', he: 'Hebrew', ar: 'Arabic',
    fa: 'Persian', ur: 'Urdu', hi: 'Hindi', bn: 'Bangla', ta: 'Tamil', te: 'Telugu',
    th: 'Thai', vi: 'Vietnamese', id: 'Indonesian', ms: 'Malay',
    ja: 'Japanese', ko: 'Korean', zh: 'Chinese',
};

const REGION_NAME = {
    US: 'United States', GB: 'United Kingdom', FR: 'France', DE: 'Germany',
    ES: 'Spain', IT: 'Italy', PT: 'Portugal', NL: 'Netherlands', SE: 'Sweden',
    NO: 'Norway', DK: 'Denmark', FI: 'Finland', PL: 'Poland', CZ: 'Czechia',
    RU: 'Russia', UA: 'Ukraine', TR: 'Turkey', IL: 'Israel', SA: 'Saudi Arabia',
    IR: 'Iran', IN: 'India', CN: 'China', JP: 'Japan', KR: 'South Korea',
    BR: 'Brazil', CA: 'Canada', AU: 'Australia', NZ: 'New Zealand', MX: 'Mexico',
    CH: 'Switzerland', AT: 'Austria', BE: 'Belgium', IE: 'Ireland', ZA: 'South Africa',
};

function DisplayNames(locales, options) {
    if (!(this instanceof DisplayNames))
        throw new TypeError("Constructor Intl.DisplayNames requires 'new'");
    const o = options || {};
    if (!o.type) throw new TypeError('type option must be provided');
    this._locale = pickLocale(locales);
    this._type = o.type;
    this._fallback = o.fallback || 'code';
}

DisplayNames.prototype.of = function (code) {
    const c = String(code);
    if (this._type === 'region') {
        const name = REGION_NAME[c.toUpperCase()];
        return name || (this._fallback === 'none' ? undefined : c);
    }
    if (this._type === 'language') {
        const lang = languageOf(c);
        // Asking a locale for its own name wants the endonym — that is what a
        // language picker renders, and it is the one case a table this size
        // can always answer correctly.
        const name = (languageOf(this._locale) === lang)
            ? (ENDONYM[lang] || ENGLISH_NAME[lang])
            : (ENGLISH_NAME[lang] || ENDONYM[lang]);
        if (!name) return this._fallback === 'none' ? undefined : c;
        const region = regionOf(c);
        return region && REGION_NAME[region] ? name + ' (' + REGION_NAME[region] + ')'
                                             : name;
    }
    // currency / script / calendar / dateTimeField: the code is the honest
    // answer when there is no table behind it.
    return this._fallback === 'none' ? undefined : c;
};

DisplayNames.prototype.resolvedOptions = function () {
    return { locale: this._locale, type: this._type, style: 'long',
             fallback: this._fallback };
};
DisplayNames.supportedLocalesOf = NumberFormat.supportedLocalesOf;

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

const Intl = {
    NumberFormat: NumberFormat,
    PluralRules: PluralRules,
    Collator: Collator,
    DateTimeFormat: DateTimeFormat,
    ListFormat: ListFormat,
    RelativeTimeFormat: RelativeTimeFormat,
    DisplayNames: DisplayNames,
    getCanonicalLocales: function (locales) {
        if (locales === undefined) return [];
        const list = typeof locales === 'string' ? [locales] : Array.from(locales);
        return list.map(canonicalize);
    },
};
Object.defineProperty(Intl, Symbol.toStringTag, { value: 'Intl', configurable: true });
Object.defineProperty(globalThis, 'Intl',
                      { value: Intl, writable: true, configurable: true });

// The toLocaleString family is where a page most often meets Intl without
// naming it. QuickJS's are aliases for toString, so "1234567".toLocaleString()
// comes back ungrouped — the one difference a user actually sees.
Number.prototype.toLocaleString = function (locales, options) {
    return new NumberFormat(locales, options).format(this.valueOf());
};
Date.prototype.toLocaleString = function (locales, options) {
    return new DateTimeFormat(locales, options ||
        { year: 'numeric', month: 'numeric', day: 'numeric',
          hour: 'numeric', minute: 'numeric', second: 'numeric' }).format(this);
};
Date.prototype.toLocaleDateString = function (locales, options) {
    return new DateTimeFormat(locales, options ||
        { year: 'numeric', month: 'numeric', day: 'numeric' }).format(this);
};
Date.prototype.toLocaleTimeString = function (locales, options) {
    return new DateTimeFormat(locales, options ||
        { hour: 'numeric', minute: 'numeric', second: 'numeric' }).format(this);
};
String.prototype.localeCompare = function (that, locales, options) {
    return new Collator(locales, options).compare(String(this), String(that));
};

})();
