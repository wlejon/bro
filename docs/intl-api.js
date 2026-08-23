/**
 * =============================================================================
 * Intl (ECMA-402) — Internationalization and Formatting Suite
 * =============================================================================
 *
 * Implements standard ECMA-402 internationalization constructors and helpers:
 * NumberFormat, DateTimeFormat, PluralRules, Collator, ListFormat,
 * RelativeTimeFormat, DisplayNames, and getCanonicalLocales.
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
   *  Returns subset of provided locales supported by implementation.
   *
   * @param {*} locales
   * @param {Object} [options]
   * @returns {Array<string>}
   */
  static supportedLocalesOf(locales, options) {}

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
   *  Returns subset of provided locales supported by implementation.
   *
   * @param {*} locales
   * @param {Object} [options]
   * @returns {Array<string>}
   */
  static supportedLocalesOf(locales, options) {}

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
   *  Returns subset of provided locales supported by implementation.
   *
   * @param {*} locales
   * @param {Object} [options]
   * @returns {Array<string>}
   */
  static supportedLocalesOf(locales, options) {}

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
   *  Returns subset of provided locales supported by implementation.
   *
   * @param {*} locales
   * @param {Object} [options]
   * @returns {Array<string>}
   */
  static supportedLocalesOf(locales, options) {}

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
   *  Returns subset of provided locales supported by implementation.
   *
   * @param {*} locales
   * @param {Object} [options]
   * @returns {Array<string>}
   */
  static supportedLocalesOf(locales, options) {}

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
   *  Returns subset of provided locales supported by implementation.
   *
   * @param {*} locales
   * @param {Object} [options]
   * @returns {Array<string>}
   */
  static supportedLocalesOf(locales, options) {}

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
   *  Returns subset of provided locales supported by implementation.
   *
   * @param {*} locales
   * @param {Object} [options]
   * @returns {Array<string>}
   */
  static supportedLocalesOf(locales, options) {}

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

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * Standard ECMA-402 internationalization namespace.
 */
/**
 *  Normalizes BCP-47 language tag strings to canonical form.
 *
 * @param {*} [locales]
 * @returns {Array<string>}
 */
bro.Intl.getCanonicalLocales = function(locales) {};

