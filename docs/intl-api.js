/**
 * =============================================================================
 * Intl (ECMA-402) — Internationalization and Formatting Suite
 * =============================================================================
 *
 * Implements standard ECMA-402 internationalization constructors and helpers:
 * NumberFormat, DateTimeFormat, PluralRules, Collator, ListFormat,
 * RelativeTimeFormat, DisplayNames, Locale, and getCanonicalLocales.
 *
 * @example
 *   // Number formatting with locale separators
 *   const nf = new Intl.NumberFormat('en-US', { style: 'currency', currency: 'USD' });
 *   console.log(nf.format(123456.78)); // '$123,456.78'
 *
 * @example
 *   // Date formatting
 *   const df = new Intl.DateTimeFormat('en-US', { dateStyle: 'full' });
 *   console.log(df.format(new Date()));
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * Formats numbers, currencies, and percentages according to locale conventions.
 */
class NumberFormat {

  /**
   * @param {*} [locales]
   * @param {Object} [options]
   */
  constructor(locales, options) {}

  /**
   *  Formats a number to a localized string.
   *
   * @param {number} value
   * @returns {string}
   */
  format(value) {}

  /**
   *  Returns sequence of tokenized format parts.
   *
   * @param {number} value
   * @returns {Array<Object>}
   */
  formatToParts(value) {}

  /**
   *  Returns resolved formatting options.
   * @returns {Object}
   */
  resolvedOptions() {}

}

/**
 * Formats dates and times according to locale conventions.
 */
class DateTimeFormat {

  /**
   * @param {*} [locales]
   * @param {Object} [options]
   */
  constructor(locales, options) {}

  /**
   *  Formats a Date or timestamp to a localized string.
   *
   * @param {*} [date]
   * @returns {string}
   */
  format(date) {}

  /**
   *  Returns sequence of tokenized date/time format parts.
   *
   * @param {*} [date]
   * @returns {Array<Object>}
   */
  formatToParts(date) {}

  /**
   *  Returns resolved formatting options.
   * @returns {Object}
   */
  resolvedOptions() {}

}

/**
 * Determines plural category rules for numbers.
 */
class PluralRules {

  /**
   * @param {*} [locales]
   * @param {Object} [options]
   */
  constructor(locales, options) {}

  /**
   *  Selects plural category ('zero' | 'one' | 'two' | 'few' | 'many' | 'other').
   *
   * @param {number} value
   * @returns {string}
   */
  select(value) {}

  /**
   *  Returns resolved formatting options.
   * @returns {Object}
   */
  resolvedOptions() {}

}

/**
 * Compares strings according to locale-specific collation rules.
 */
class Collator {

  /**
   * @param {*} [locales]
   * @param {Object} [options]
   */
  constructor(locales, options) {}

  /**
   *  Compares two strings returning negative, zero, or positive integer.
   *
   * @param {string} string1
   * @param {string} string2
   * @returns {number}
   */
  compare(string1, string2) {}

  /**
   *  Returns resolved formatting options.
   * @returns {Object}
   */
  resolvedOptions() {}

}

/**
 * Formats lists of items using localized conjunctions and disjunctions.
 */
class ListFormat {

  /**
   * @param {*} [locales]
   * @param {Object} [options]
   */
  constructor(locales, options) {}

  /**
   *  Formats a list of strings into a localized sentence fragment.
   *
   * @param {Array<string>} list
   * @returns {string}
   */
  format(list) {}

  /**
   *  Returns sequence of tokenized list format parts.
   *
   * @param {Array<string>} list
   * @returns {Array<Object>}
   */
  formatToParts(list) {}

  /**
   *  Returns resolved formatting options.
   * @returns {Object}
   */
  resolvedOptions() {}

}

/**
 * Formats relative time units ('3 days ago', 'in 2 hours').
 */
class RelativeTimeFormat {

  /**
   * @param {*} [locales]
   * @param {Object} [options]
   */
  constructor(locales, options) {}

  /**
   *  Formats a value and time unit into relative time string.
   *
   * @param {number} value
   * @param {string} unit
   * @returns {string}
   */
  format(value, unit) {}

  /**
   *  Returns sequence of tokenized relative time format parts.
   *
   * @param {number} value
   * @param {string} unit
   * @returns {Array<Object>}
   */
  formatToParts(value, unit) {}

  /**
   *  Returns resolved formatting options.
   * @returns {Object}
   */
  resolvedOptions() {}

}

/**
 * Returns localized display names for languages, regions, and currencies.
 */
class DisplayNames {

  /**
   * @param {*} [locales]
   * @param {Object} [options]
   */
  constructor(locales, options) {}

  /**
   *  Returns localized display name for given language/region code.
   *
   * @param {string} code
   * @returns {string|null}
   */
  of(code) {}

  /**
   *  Returns resolved formatting options.
   * @returns {Object}
   */
  resolvedOptions() {}

}

/**
 * A Unicode BCP 47 locale identifier, parsed and canonicalized as V8 does
 * (language lower case, script title case, region upper case, the -u-
 * keywords sorted; `_` is not a separator). A malformed tag or option value is
 * a RangeError. maximize() / minimize() add or drop the likely script and
 * region for the common languages (en, fr, de, es, it, pt, ja, zh, ko, ru,
 * ar, hi and about twenty more); another language is returned as it is.
 *
 * @example
 *   const l = new Intl.Locale('en', { region: 'GB', hourCycle: 'h23' });
 *   l.toString();                                   // 'en-GB-u-hc-h23'
 *   new Intl.Locale('zh').maximize().baseName;      // 'zh-Hans-CN'
 */
class Locale {
  /**
   * @param {string|Locale} tag
   * @param {{language?: string, script?: string, region?: string, calendar?: string, collation?: string,
   *          hourCycle?: 'h11'|'h12'|'h23'|'h24', caseFirst?: 'upper'|'lower'|'false', numeric?: boolean,
   *          numberingSystem?: string}} [options]
   */
  constructor(tag, options) {}
  /** @readonly @type {string} */ baseName;
  /** @readonly @type {string} */ language;
  /** @readonly @type {string|undefined} */ script;
  /** @readonly @type {string|undefined} */ region;
  /** @readonly @type {string|undefined} */ calendar;
  /** @readonly @type {string|undefined} */ collation;
  /** @readonly @type {string|undefined} */ hourCycle;
  /** @readonly @type {string|undefined} */ caseFirst;
  /** @readonly @type {boolean} */ numeric;
  /** @readonly @type {string|undefined} */ numberingSystem;
  /** @returns {string} */ toString() {}
  /** @returns {Locale} */ maximize() {}
  /** @returns {Locale} */ minimize() {}
}

// ── The Intl global ──────────────────────────────────────────────────────────
//
// The classes above are members of the global `Intl` (Intl.NumberFormat, ...),
// not of `bro`. The main realm has it; Worker realms do not yet.

/**
 *  Normalizes BCP-47 language tag strings to canonical form.
 *
 * @param {*} [locales]
 * @returns {Array<string>}
 */
Intl.getCanonicalLocales = function(locales) {};

