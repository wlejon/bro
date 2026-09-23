// ECMA-402 (Intl) runtime support for Bronze host.
#include "bronze_host/host_intl.h"
#include "bronze_host/bronze_host.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/gl_internal.h"
#include "engine/engine.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bro::bronze_host {

namespace {

// ---------------------------------------------------------------------------
// Locale canonicalization helpers
// ---------------------------------------------------------------------------

std::string canonicalize(std::string_view tag) {
    if (tag.empty()) return "en-US";
    std::string s;
    s.reserve(tag.size());
    for (char c : tag) s.push_back(c == '_' ? '-' : c);

    std::vector<std::string> parts;
    size_t start = 0;
    while (start < s.size()) {
        size_t end = s.find('-', start);
        if (end == std::string::npos) end = s.size();
        parts.push_back(s.substr(start, end - start));
        start = end + 1;
    }
    if (parts.empty() || parts[0].empty()) return "en-US";
    for (char& c : parts[0]) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (size_t i = 1; i < parts.size(); ++i) {
        if (parts[i].size() == 4) {
            parts[i][0] = static_cast<char>(std::toupper(static_cast<unsigned char>(parts[i][0])));
            for (size_t j = 1; j < parts[i].size(); ++j)
                parts[i][j] = static_cast<char>(std::tolower(static_cast<unsigned char>(parts[i][j])));
        } else if (parts[i].size() == 2 || parts[i].size() == 3) {
            for (char& c : parts[i]) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        } else {
            for (char& c : parts[i]) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    std::string res;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) res += "-";
        res += parts[i];
    }
    return res;
}

std::string languageOf(const std::string& tag) {
    size_t dash = tag.find('-');
    std::string l = (dash == std::string::npos) ? tag : tag.substr(0, dash);
    for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return l;
}

std::string regionOf(const std::string& tag) {
    size_t dash = tag.find('-');
    while (dash != std::string::npos) {
        size_t next = tag.find('-', dash + 1);
        size_t len = (next == std::string::npos) ? tag.size() - (dash + 1) : next - (dash + 1);
        if (len == 2) {
            std::string r = tag.substr(dash + 1, 2);
            for (char& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return r;
        }
        dash = next;
    }
    return "";
}

std::string pickLocale(std::span<const Value> a, size_t idx = 0) {
    if (a.size() <= idx || ev::isUndefined(a[idx]) || ev::isNull(a[idx])) return "en-US";
    const Value& v = a[idx];  // the rooted slot, current across the reads
    if (ev::isString(v)) return canonicalize(ev::toUtf8(v));
    if (ev::isObject(v)) {
        Value lenVal = ev::getProperty(v, "length");
        if (ev::isNumber(lenVal)) {
            uint32_t len = static_cast<uint32_t>(ev::toDouble(lenVal));
            if (len > 0) {
                Value first = ev::getElement(v, 0);
                if (ev::isString(first)) return canonicalize(ev::toUtf8(first));
            }
        }
        return canonicalize(ev::toUtf8(v));
    }
    return "en-US";
}

// ---------------------------------------------------------------------------
// NumberFormat implementation
// ---------------------------------------------------------------------------

struct NumberFormatOptions {
    std::string locale = "en-US";
    std::string style = "decimal";    // "decimal", "percent", "currency"
    std::string currency = "USD";
    bool useGrouping = true;
    int minFractionDigits = -1;
    int maxFractionDigits = -1;
};

NumberFormatOptions parseNumberFormatOptions(std::span<const Value> a) {
    NumberFormatOptions opt;
    opt.locale = pickLocale(a, 0);
    if (a.size() > 1 && ev::isObject(a[1])) {
        const Value& o = a[1];  // the rooted slot, current across the reads
        Value v = ev::getProperty(o, "style");
        if (ev::isString(v)) opt.style = ev::toUtf8(v);
        v = ev::getProperty(o, "currency");
        if (ev::isString(v)) opt.currency = ev::toUtf8(v);
        v = ev::getProperty(o, "useGrouping");
        if (ev::isBool(v)) opt.useGrouping = ev::toBool(v);
        v = ev::getProperty(o, "minimumFractionDigits");
        if (ev::isNumber(v)) opt.minFractionDigits = static_cast<int>(ev::toDouble(v));
        v = ev::getProperty(o, "maximumFractionDigits");
        if (ev::isNumber(v)) opt.maxFractionDigits = static_cast<int>(ev::toDouble(v));
    }
    if (opt.minFractionDigits < 0) {
        if (opt.style == "currency") opt.minFractionDigits = (opt.currency == "JPY") ? 0 : 2;
        else if (opt.style == "percent") opt.minFractionDigits = 0;
        else opt.minFractionDigits = 0;
    }
    if (opt.maxFractionDigits < 0) {
        if (opt.style == "currency") opt.maxFractionDigits = (opt.currency == "JPY") ? 0 : 2;
        else if (opt.style == "percent") opt.maxFractionDigits = 0;
        else opt.maxFractionDigits = std::max(opt.minFractionDigits, 3);
    }
    return opt;
}

std::string groupStandard(const std::string& digits, const std::string& sep) {
    if (digits.size() <= 3) return digits;
    std::string res;
    size_t rem = digits.size() % 3;
    if (rem > 0) {
        res += digits.substr(0, rem);
        if (rem < digits.size()) res += sep;
    }
    for (size_t i = rem; i < digits.size(); i += 3) {
        if (i > rem) res += sep;
        res += digits.substr(i, 3);
    }
    return res;
}

std::string groupIndian(const std::string& digits, const std::string& sep) {
    if (digits.size() <= 3) return digits;
    std::string last3 = digits.substr(digits.size() - 3);
    std::string rest = digits.substr(0, digits.size() - 3);
    std::vector<std::string> pairs;
    for (long long i = static_cast<long long>(rest.size()); i > 0; i -= 2) {
        long long st = std::max(0LL, i - 2);
        pairs.insert(pairs.begin(), rest.substr(static_cast<size_t>(st), static_cast<size_t>(i - st)));
    }
    std::string res;
    for (size_t i = 0; i < pairs.size(); ++i) {
        if (i > 0) res += sep;
        res += pairs[i];
    }
    return res + sep + last3;
}

struct FormattedNumber {
    bool isNegative = false;
    std::string currencyPrefix;
    std::vector<std::pair<std::string, std::string>> parts;  // type, value
    std::string fullString;
};

FormattedNumber formatNumberDetails(double num, const NumberFormatOptions& opt) {
    FormattedNumber fn;
    if (std::isnan(num)) {
        fn.fullString = "NaN";
        fn.parts.push_back({"nan", "NaN"});
        return fn;
    }
    fn.isNegative = std::signbit(num) && num != 0.0;
    double absNum = std::abs(num);
    if (opt.style == "percent") absNum *= 100.0;

    double factor = std::pow(10.0, opt.maxFractionDigits);
    double rounded = std::round(absNum * factor) / factor;

    std::ostringstream ss;
    ss << std::fixed << std::setprecision(opt.maxFractionDigits) << rounded;
    std::string numStr = ss.str();

    size_t dot = numStr.find('.');
    std::string intPart = (dot == std::string::npos) ? numStr : numStr.substr(0, dot);
    std::string fracPart = (dot == std::string::npos) ? "" : numStr.substr(dot + 1);

    while (static_cast<int>(fracPart.size()) > opt.minFractionDigits && !fracPart.empty() && fracPart.back() == '0') {
        fracPart.pop_back();
    }
    while (static_cast<int>(fracPart.size()) < opt.minFractionDigits) {
        fracPart.push_back('0');
    }

    std::string lang = languageOf(opt.locale);
    std::string reg = regionOf(opt.locale);
    std::string groupSep = (lang == "de") ? "." : ",";
    std::string decSep = (lang == "de") ? "," : ".";
    bool isIndian = (reg == "IN" || opt.locale == "en-IN");

    std::string formattedInt;
    if (opt.useGrouping) {
        formattedInt = isIndian ? groupIndian(intPart, groupSep) : groupStandard(intPart, groupSep);
    } else {
        formattedInt = intPart;
    }

    if (opt.style == "currency") {
        if (opt.currency == "USD") fn.currencyPrefix = "$";
        else if (opt.currency == "JPY") fn.currencyPrefix = "\xC2\xA5";  // ¥
        else fn.currencyPrefix = opt.currency + " ";
    }

    std::string s;
    if (fn.isNegative) {
        s += "-";
        fn.parts.push_back({"minusSign", "-"});
    }
    if (!fn.currencyPrefix.empty()) {
        s += fn.currencyPrefix;
        fn.parts.push_back({"currency", fn.currencyPrefix});
    }

    // Split formattedInt into integer and group parts:
    size_t pStart = 0;
    while (pStart < formattedInt.size()) {
        size_t gPos = formattedInt.find(groupSep, pStart);
        if (gPos == std::string::npos) {
            std::string sub = formattedInt.substr(pStart);
            s += sub;
            fn.parts.push_back({"integer", sub});
            break;
        } else {
            std::string sub = formattedInt.substr(pStart, gPos - pStart);
            s += sub;
            fn.parts.push_back({"integer", sub});
            s += groupSep;
            fn.parts.push_back({"group", groupSep});
            pStart = gPos + groupSep.size();
        }
    }

    if (!fracPart.empty()) {
        s += decSep;
        fn.parts.push_back({"decimal", decSep});
        s += fracPart;
        fn.parts.push_back({"fraction", fracPart});
    }

    if (opt.style == "percent") {
        s += "%";
        fn.parts.push_back({"percentSign", "%"});
    }
    fn.fullString = s;
    return fn;
}

Value makeNumberFormatInstance(const NumberFormatOptions& opt) {
    ObjectBuilder b;
    b.def("format", 1, [opt](Value, std::span<const Value> a) {
        double n = a.empty() ? 0.0 : ev::toDouble(a[0]);
        return ev::fromUtf8(formatNumberDetails(n, opt).fullString);
    });
    b.def("formatToParts", 1, [opt](Value, std::span<const Value> a) {
        double n = a.empty() ? 0.0 : ev::toDouble(a[0]);
        FormattedNumber fn = formatNumberDetails(n, opt);
        std::function<Value(size_t)> cb = [&fn](size_t i) -> Value {
            ObjectBuilder p;
            p.set("type", ev::fromUtf8(fn.parts[i].first));
            p.set("value", ev::fromUtf8(fn.parts[i].second));
            return p.get();
        };
        return hostArrayOf(fn.parts.size(), cb);
    });
    b.def("resolvedOptions", 0, [opt](Value, std::span<const Value>) {
        ObjectBuilder r;
        r.set("locale", ev::fromUtf8(opt.locale));
        r.set("style", ev::fromUtf8(opt.style));
        if (opt.style == "currency") r.set("currency", ev::fromUtf8(opt.currency));
        r.set("minimumFractionDigits", ev::fromDouble(opt.minFractionDigits));
        r.set("maximumFractionDigits", ev::fromDouble(opt.maxFractionDigits));
        r.set("useGrouping", ev::fromBool(opt.useGrouping));
        return r.get();
    });
    return b.get();
}

// ---------------------------------------------------------------------------
// PluralRules implementation
// ---------------------------------------------------------------------------

struct PluralRulesOptions {
    std::string locale = "en-US";
    std::string type = "cardinal";  // "cardinal", "ordinal"
};

PluralRulesOptions parsePluralRulesOptions(std::span<const Value> a) {
    PluralRulesOptions opt;
    opt.locale = pickLocale(a, 0);
    if (a.size() > 1 && ev::isObject(a[1])) {
        Value v = ev::getProperty(a[1], "type");
        if (ev::isString(v)) opt.type = ev::toUtf8(v);
    }
    return opt;
}

const char* selectPlural(double n, const PluralRulesOptions& opt) {
    std::string lang = languageOf(opt.locale);
    long long val = static_cast<long long>(std::abs(n));
    if (opt.type == "ordinal") {
        long long mod10 = val % 10;
        long long mod100 = val % 100;
        if (mod10 == 1 && mod100 != 11) return "one";
        if (mod10 == 2 && mod100 != 12) return "two";
        if (mod10 == 3 && mod100 != 13) return "few";
        return "other";
    }
    if (lang == "ja" || lang == "zh" || lang == "ko") return "other";
    if (lang == "fr") return (val == 0 || val == 1) ? "one" : "other";
    if (lang == "ru" || lang == "uk") {
        long long mod10 = val % 10;
        long long mod100 = val % 100;
        if (mod10 == 1 && mod100 != 11) return "one";
        if (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14)) return "few";
        if (mod10 == 0 || (mod10 >= 5 && mod10 <= 9) || (mod100 >= 11 && mod100 <= 14)) return "many";
        return "other";
    }
    return (val == 1) ? "one" : "other";
}

Value makePluralRulesInstance(const PluralRulesOptions& opt) {
    ObjectBuilder b;
    b.def("select", 1, [opt](Value, std::span<const Value> a) {
        double n = a.empty() ? 0.0 : ev::toDouble(a[0]);
        return ev::fromUtf8(selectPlural(n, opt));
    });
    b.def("resolvedOptions", 0, [opt](Value, std::span<const Value>) {
        ObjectBuilder r;
        r.set("locale", ev::fromUtf8(opt.locale));
        r.set("type", ev::fromUtf8(opt.type));
        return r.get();
    });
    return b.get();
}

// ---------------------------------------------------------------------------
// Collator implementation
// ---------------------------------------------------------------------------

struct CollatorOptions {
    std::string locale = "en-US";
    bool numeric = false;
    std::string sensitivity = "variant";  // "base", "accent", "case", "variant"
};

CollatorOptions parseCollatorOptions(std::span<const Value> a) {
    CollatorOptions opt;
    opt.locale = pickLocale(a, 0);
    if (a.size() > 1 && ev::isObject(a[1])) {
        Value v = ev::getProperty(a[1], "numeric");
        if (ev::isBool(v)) opt.numeric = ev::toBool(v);
        v = ev::getProperty(a[1], "sensitivity");
        if (ev::isString(v)) opt.sensitivity = ev::toUtf8(v);
    }
    return opt;
}

std::string stripAccents(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0xC3 && i + 1 < s.size()) {
            unsigned char c2 = static_cast<unsigned char>(s[i + 1]);
            // Latin-1 Supplement accents:
            if ((c2 >= 0x80 && c2 <= 0x86) || (c2 >= 0xA0 && c2 <= 0xA6)) { out.push_back('a'); i++; continue; }
            if ((c2 >= 0x88 && c2 <= 0x8B) || (c2 >= 0xA8 && c2 <= 0xAB)) { out.push_back('e'); i++; continue; }
            if ((c2 >= 0x8C && c2 <= 0x8F) || (c2 >= 0xAC && c2 <= 0xAF)) { out.push_back('i'); i++; continue; }
            if ((c2 >= 0x92 && c2 <= 0x96) || (c2 >= 0xB2 && c2 <= 0xB6)) { out.push_back('o'); i++; continue; }
            if ((c2 >= 0x99 && c2 <= 0x9C) || (c2 >= 0xB9 && c2 <= 0xBC)) { out.push_back('u'); i++; continue; }
            if (c2 == 0x91 || c2 == 0xB1) { out.push_back('n'); i++; continue; }
            if (c2 == 0x87 || c2 == 0xA7) { out.push_back('c'); i++; continue; }
        }
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

int naturalCompare(const std::string& a, const std::string& b) {
    size_t ia = 0, ib = 0;
    while (ia < a.size() && ib < b.size()) {
        if (std::isdigit(static_cast<unsigned char>(a[ia])) && std::isdigit(static_cast<unsigned char>(b[ib]))) {
            size_t ea = ia, eb = ib;
            while (ea < a.size() && std::isdigit(static_cast<unsigned char>(a[ea]))) ea++;
            while (eb < b.size() && std::isdigit(static_cast<unsigned char>(b[eb]))) eb++;
            long long na = std::stoll(a.substr(ia, ea - ia));
            long long nb = std::stoll(b.substr(ib, eb - ib));
            if (na != nb) return na < nb ? -1 : 1;
            ia = ea;
            ib = eb;
        } else {
            if (a[ia] != b[ib]) return a[ia] < b[ib] ? -1 : 1;
            ia++;
            ib++;
        }
    }
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    return 0;
}

int collatorCompare(const std::string& s1, const std::string& s2, const CollatorOptions& opt) {
    std::string a = s1, b = s2;
    if (opt.sensitivity == "base") {
        a = stripAccents(a);
        b = stripAccents(b);
    }
    if (opt.numeric) {
        return naturalCompare(a, b);
    }
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

Value makeCollatorInstance(const CollatorOptions& opt) {
    ObjectBuilder obj;
    Value compFn = ev::makeFunction([opt](Value, std::span<const Value> a) {
        std::string s1 = a.size() > 0 ? ev::toUtf8(a[0]) : "";
        std::string s2 = a.size() > 1 ? ev::toUtf8(a[1]) : "";
        return ev::fromDouble(collatorCompare(s1, s2, opt));
    }, 2);
    obj.set("compare", compFn);
    obj.def("resolvedOptions", 0, [opt](Value, std::span<const Value>) {
        ObjectBuilder r;
        r.set("locale", ev::fromUtf8(opt.locale));
        r.set("numeric", ev::fromBool(opt.numeric));
        r.set("sensitivity", ev::fromUtf8(opt.sensitivity));
        return r.get();
    });
    return obj.get();
}

// ---------------------------------------------------------------------------
// DateTimeFormat implementation
// ---------------------------------------------------------------------------

struct DateTimeOptions {
    std::string locale = "en-US";
    std::string monthStyle;  // "long", "numeric", etc.
    std::string dateStyle;
    std::string timeStyle;
    bool hasYear = false;
    bool hasMonth = false;
    bool hasDay = false;
    bool hasHour = false;
    bool hasMinute = false;
    bool hasSecond = false;
};

DateTimeOptions parseDateTimeOptions(std::span<const Value> a) {
    DateTimeOptions opt;
    opt.locale = pickLocale(a, 0);
    if (a.size() > 1 && ev::isObject(a[1])) {
        const Value& o = a[1];  // the rooted slot, current across the reads
        Value v = ev::getProperty(o, "month");
        if (ev::isString(v)) { opt.monthStyle = ev::toUtf8(v); opt.hasMonth = true; }
        v = ev::getProperty(o, "year");
        if (!ev::isUndefined(v)) opt.hasYear = true;
        v = ev::getProperty(o, "day");
        if (!ev::isUndefined(v)) opt.hasDay = true;
        v = ev::getProperty(o, "hour");
        if (!ev::isUndefined(v)) opt.hasHour = true;
        v = ev::getProperty(o, "minute");
        if (!ev::isUndefined(v)) opt.hasMinute = true;
        v = ev::getProperty(o, "second");
        if (!ev::isUndefined(v)) opt.hasSecond = true;
    }
    return opt;
}

std::string formatDateTimeDetails(Value dateIn, const DateTimeOptions& opt) {
    int year = 1970, month = 0, day = 1, hours = 0, minutes = 0, seconds = 0;
    if (ev::isObject(dateIn)) {
        // Rooted: each getter read and call below allocates.
        const Rooted dateVal(dateIn);
        auto callGetter = [&dateVal](const char* name) -> int {
            Value fn = ev::getProperty(dateVal, name);
            if (ev::isFunction(fn)) {
                auto res = ev::call(fn, dateVal.get(), {});
                if (!res.thrown) return static_cast<int>(ev::toDouble(res.value));
            }
            return 0;
        };
        year = callGetter("getFullYear");
        month = callGetter("getMonth");
        day = callGetter("getDate");
        hours = callGetter("getHours");
        minutes = callGetter("getMinutes");
        seconds = callGetter("getSeconds");
    }

    if (opt.monthStyle == "long") {
        static const char* kMonths[] = {
            "January", "February", "March", "April", "May", "June",
            "July", "August", "September", "October", "November", "December"
        };
        const char* mName = (month >= 0 && month < 12) ? kMonths[month] : "";
        return std::string(mName) + " " + std::to_string(day) + ", " + std::to_string(year);
    }

    std::string lang = languageOf(opt.locale);
    std::string reg = regionOf(opt.locale);

    // If only time was requested (hour/minute/second without year/month/day):
    if (opt.hasHour && !opt.hasYear && !opt.hasMonth) {
        char buf[64];
        if (lang == "de" || reg == "DE") {
            snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hours, minutes, seconds);
        } else {
            int h12 = hours % 12;
            if (h12 == 0) h12 = 12;
            snprintf(buf, sizeof(buf), "%d:%02d:%02d %s", h12, minutes, seconds, hours >= 12 ? "PM" : "AM");
        }
        return buf;
    }

    // Date formatting:
    if (reg == "GB" || opt.locale == "en-GB") {
        return std::to_string(day) + "/" + std::to_string(month + 1) + "/" + std::to_string(year);
    }
    if (lang == "de" || reg == "DE") {
        return std::to_string(day) + "." + std::to_string(month + 1) + "." + std::to_string(year);
    }
    return std::to_string(month + 1) + "/" + std::to_string(day) + "/" + std::to_string(year);
}

// One `{type, value}` part — the shape every Intl formatToParts answers with.
// A formatter that does not track where its pieces came from reports the whole
// string as a single `literal`, which is what the spec permits and what the
// old stack returned for DateTimeFormat, ListFormat and RelativeTimeFormat:
// a caller that maps over the parts and joins the values gets `format()` back,
// which is the invariant it actually relies on. Without the method at all,
// that same caller throws.
Value singleLiteralPart(const std::string& text) {
    std::function<Value(size_t)> cb = [&text](size_t) -> Value {
        ObjectBuilder p;
        p.set("type", ev::fromUtf8("literal"));
        p.set("value", ev::fromUtf8(text));
        return p.get();
    };
    return hostArrayOf(1, cb);
}

// `formatToParts` for a formatter whose `format` is already on the instance:
// call it with the same arguments and report the result as one literal part.
// Written against the receiver rather than against a captured closure so the
// two stay in step — there is no second copy of the formatting rules here.
void defFormatToPartsViaFormat(ObjectBuilder& b, uint32_t arity) {
    b.def("formatToParts", arity, [](Value self, std::span<const Value> a) -> Value {
        // Everything that has to survive the `format` call is rooted first:
        // `a` is a span of plain Values and `getProperty` may collect.
        ev::Persistent me(self);
        std::vector<ev::Persistent> held;
        held.reserve(a.size());
        for (const Value& v : a) held.emplace_back(v);
        ev::Persistent fn(ev::getProperty(me.get(), "format"));
        if (!ev::isFunction(fn.get())) return singleLiteralPart(std::string());
        std::vector<Value> argv;
        argv.reserve(held.size());
        for (const ev::Persistent& p : held) argv.push_back(p.get());
        ev::CallResult r = ev::call(fn.get(), me.get(), argv);
        if (r.thrown) return r.value;
        return singleLiteralPart(ev::toUtf8(r.value));
    });
}

Value makeDateTimeFormatInstance(const DateTimeOptions& opt) {
    ObjectBuilder b;
    b.def("format", 1, [opt](Value, std::span<const Value> a) {
        Value d = a.empty() ? ev::undefined() : a[0];
        return ev::fromUtf8(formatDateTimeDetails(d, opt));
    });
    b.def("formatToParts", 1, [opt](Value, std::span<const Value> a) {
        Value d = a.empty() ? ev::undefined() : a[0];
        return singleLiteralPart(formatDateTimeDetails(d, opt));
    });
    b.def("resolvedOptions", 0, [opt](Value, std::span<const Value>) {
        ObjectBuilder r;
        r.set("locale", ev::fromUtf8(opt.locale));
        return r.get();
    });
    return b.get();
}

// ---------------------------------------------------------------------------
// ListFormat, RelativeTimeFormat, DisplayNames implementations
// ---------------------------------------------------------------------------

Value makeListFormatInstance(std::span<const Value> a) {
    std::string loc = pickLocale(a, 0);
    std::string type = "conjunction";
    if (a.size() > 1 && ev::isObject(a[1])) {
        Value v = ev::getProperty(a[1], "type");
        if (ev::isString(v)) type = ev::toUtf8(v);
    }
    ObjectBuilder b;
    b.def("format", 1, [loc, type](Value, std::span<const Value> args) {
        if (args.empty() || !ev::isObject(args[0])) return ev::fromUtf8("");
        const Value& arr = args[0];  // the rooted slot, current across the reads
        Value lenVal = ev::getProperty(arr, "length");
        if (!ev::isNumber(lenVal)) return ev::fromUtf8("");
        uint32_t len = static_cast<uint32_t>(ev::toDouble(lenVal));
        if (len == 0) return ev::fromUtf8("");
        if (len == 1) return ev::getProperty(arr, "0");
        std::string word = (type == "disjunction") ? "or" : "and";
        if (len == 2) {
            std::string s0 = ev::toUtf8(ev::getProperty(arr, "0"));
            std::string s1 = ev::toUtf8(ev::getProperty(arr, "1"));
            return ev::fromUtf8(s0 + " " + word + " " + s1);
        }
        std::string res;
        for (uint32_t i = 0; i < len; ++i) {
            std::string item = ev::toUtf8(ev::getElement(arr, i));
            if (i == 0) res = item;
            else if (i + 1 == len) res += ", " + word + " " + item;
            else res += ", " + item;
        }
        return ev::fromUtf8(res);
    });
    defFormatToPartsViaFormat(b, 1);
    b.def("resolvedOptions", 0, [loc, type](Value, std::span<const Value>) {
        ObjectBuilder r;
        r.set("locale", ev::fromUtf8(loc));
        r.set("type", ev::fromUtf8(type));
        return r.get();
    });
    return b.get();
}

Value makeRelativeTimeFormatInstance(std::span<const Value> a) {
    std::string loc = pickLocale(a, 0);
    std::string numeric = "always";
    if (a.size() > 1 && ev::isObject(a[1])) {
        Value v = ev::getProperty(a[1], "numeric");
        if (ev::isString(v)) numeric = ev::toUtf8(v);
    }
    ObjectBuilder b;
    b.def("format", 2, [loc, numeric](Value, std::span<const Value> args) {
        double val = args.size() > 0 ? ev::toDouble(args[0]) : 0.0;
        std::string unit = args.size() > 1 ? ev::toUtf8(args[1]) : "day";
        if (!unit.empty() && unit.back() == 's') unit.pop_back();
        if (numeric == "auto" && unit == "day") {
            if (val == -1.0) return ev::fromUtf8("yesterday");
            if (val == 0.0) return ev::fromUtf8("today");
            if (val == 1.0) return ev::fromUtf8("tomorrow");
        }
        long long count = static_cast<long long>(std::abs(val));
        std::string pluralUnit = (count == 1) ? unit : unit + "s";
        if (val < 0) {
            return ev::fromUtf8(std::to_string(count) + " " + pluralUnit + " ago");
        }
        return ev::fromUtf8("in " + std::to_string(count) + " " + pluralUnit);
    });
    defFormatToPartsViaFormat(b, 2);
    b.def("resolvedOptions", 0, [loc, numeric](Value, std::span<const Value>) {
        ObjectBuilder r;
        r.set("locale", ev::fromUtf8(loc));
        r.set("numeric", ev::fromUtf8(numeric));
        return r.get();
    });
    return b.get();
}

static const std::unordered_map<std::string, std::string> kRegionNames = {
    {"US", "United States"}, {"GB", "United Kingdom"}, {"FR", "France"}, {"DE", "Germany"},
    {"ES", "Spain"}, {"IT", "Italy"}, {"PT", "Portugal"}, {"NL", "Netherlands"},
    {"SE", "Sweden"}, {"NO", "Norway"}, {"DK", "Denmark"}, {"FI", "Finland"},
    {"PL", "Poland"}, {"CZ", "Czechia"}, {"RU", "Russia"}, {"UA", "Ukraine"},
    {"TR", "Turkey"}, {"IL", "Israel"}, {"SA", "Saudi Arabia"}, {"IR", "Iran"},
    {"IN", "India"}, {"CN", "China"}, {"JP", "Japan"}, {"KR", "South Korea"},
    {"BR", "Brazil"}, {"CA", "Canada"}, {"AU", "Australia"}, {"NZ", "New Zealand"},
    {"MX", "Mexico"}, {"CH", "Switzerland"}, {"AT", "Austria"}, {"BE", "Belgium"},
    {"IE", "Ireland"}, {"ZA", "South Africa"}
};

static const std::unordered_map<std::string, std::string> kEndonymLanguages = {
    {"en", "English"}, {"fr", "français"}, {"es", "español"}, {"de", "Deutsch"},
    {"it", "italiano"}, {"pt", "português"}, {"nl", "Nederlands"}, {"ru", "русский"},
    {"ja", "日本語"}, {"ko", "한국어"}, {"zh", "中文"}, {"ar", "العربية"},
    {"hi", "हिन्दी"}, {"fa", "فارسی"}
};

static const std::unordered_map<std::string, std::string> kEnglishLanguages = {
    {"en", "English"}, {"fr", "French"}, {"es", "Spanish"}, {"de", "German"},
    {"it", "Italian"}, {"pt", "Portuguese"}, {"nl", "Dutch"}, {"ru", "Russian"},
    {"ja", "Japanese"}, {"ko", "Korean"}, {"zh", "Chinese"}, {"ar", "Arabic"},
    {"hi", "Hindi"}, {"fa", "Persian"}
};

Value makeDisplayNamesInstance(Value thisValue, std::span<const Value> a) {
    if (a.size() < 2 || !ev::isObject(a[1])) {
        return ev::throwTypeError("type option must be provided");
    }
    Value typeVal = ev::getProperty(a[1], "type");
    if (ev::isUndefined(typeVal)) {
        return ev::throwTypeError("type option must be provided");
    }
    std::string type = ev::toUtf8(typeVal);
    std::string loc = pickLocale(a, 0);

    ObjectBuilder b;
    b.def("of", 1, [loc, type](Value, std::span<const Value> args) {
        if (args.empty()) return ev::fromUtf8("");
        std::string code = ev::toUtf8(args[0]);
        if (type == "region") {
            std::string upCode = code;
            for (char& c : upCode) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            auto it = kRegionNames.find(upCode);
            if (it != kRegionNames.end()) return ev::fromUtf8(it->second);
            return ev::fromUtf8(code);
        }
        if (type == "language") {
            std::string lang = languageOf(code);
            std::string targetLang = languageOf(loc);
            if (lang == targetLang) {
                auto it = kEndonymLanguages.find(lang);
                if (it != kEndonymLanguages.end()) return ev::fromUtf8(it->second);
            }
            auto it = kEnglishLanguages.find(lang);
            if (it != kEnglishLanguages.end()) return ev::fromUtf8(it->second);
            return ev::fromUtf8(code);
        }
        return ev::fromUtf8(code);
    });
    b.def("resolvedOptions", 0, [loc, type](Value, std::span<const Value>) {
        ObjectBuilder r;
        r.set("locale", ev::fromUtf8(loc));
        r.set("type", ev::fromUtf8(type));
        return r.get();
    });
    return b.get();
}

}  // namespace

void installIntlGlobals() {
    ObjectBuilder intl;

    Value numberFormatCtor = ev::makeFunction(
        [](Value, std::span<const Value> a) {
            return makeNumberFormatInstance(parseNumberFormatOptions(a));
        }, 1, "NumberFormat");
    intl.set("NumberFormat", numberFormatCtor);

    Value pluralRulesCtor = ev::makeFunction(
        [](Value, std::span<const Value> a) {
            return makePluralRulesInstance(parsePluralRulesOptions(a));
        }, 1, "PluralRules");
    intl.set("PluralRules", pluralRulesCtor);

    Value collatorCtor = ev::makeFunction(
        [](Value, std::span<const Value> a) {
            return makeCollatorInstance(parseCollatorOptions(a));
        }, 1, "Collator");
    intl.set("Collator", collatorCtor);

    Value dateTimeFormatCtor = ev::makeFunction(
        [](Value, std::span<const Value> a) {
            return makeDateTimeFormatInstance(parseDateTimeOptions(a));
        }, 1, "DateTimeFormat");
    intl.set("DateTimeFormat", dateTimeFormatCtor);

    Value listFormatCtor = ev::makeFunction(
        [](Value, std::span<const Value> a) {
            return makeListFormatInstance(a);
        }, 1, "ListFormat");
    intl.set("ListFormat", listFormatCtor);

    Value relativeTimeFormatCtor = ev::makeFunction(
        [](Value, std::span<const Value> a) {
            return makeRelativeTimeFormatInstance(a);
        }, 1, "RelativeTimeFormat");
    intl.set("RelativeTimeFormat", relativeTimeFormatCtor);

    Value displayNamesCtor = ev::makeFunction(
        [](Value thisValue, std::span<const Value> a) {
            return makeDisplayNamesInstance(thisValue, a);
        }, 2, "DisplayNames");
    intl.set("DisplayNames", displayNamesCtor);

    intl.def("getCanonicalLocales", 1, [](Value, std::span<const Value> a) {
        if (a.empty() || ev::isUndefined(a[0]) || ev::isNull(a[0])) {
            std::function<Value(size_t)> emptyCb = [](size_t) { return ev::undefined(); };
            return hostArrayOf(0, emptyCb);
        }
        std::vector<std::string> results;
        if (ev::isString(a[0])) {
            results.push_back(canonicalize(ev::toUtf8(a[0])));
        } else if (ev::isObject(a[0])) {
            Value lenVal = ev::getProperty(a[0], "length");
            if (ev::isNumber(lenVal)) {
                uint32_t len = static_cast<uint32_t>(ev::toDouble(lenVal));
                for (uint32_t i = 0; i < len; ++i) {
                    Value item = ev::getElement(a[0], i);
                    if (ev::isString(item)) results.push_back(canonicalize(ev::toUtf8(item)));
                }
            }
        }
        std::function<Value(size_t)> cb = [&results](size_t i) {
            return ev::fromUtf8(results[i]);
        };
        return hostArrayOf(results.size(), cb);
    });

    // Rooted from here on: every step below allocates.
    ev::Persistent intlVal(intl.get());

    // Set Symbol.toStringTag = 'Intl'
    ev::GlobalValue objG = ev::globalValue("Object");
    ev::GlobalValue symG = ev::globalValue("Symbol");
    if (objG.found && symG.found) {
        ev::Persistent objCtor(objG.value);
        ev::Persistent symCtor(symG.value);
        ev::Persistent defProp(ev::getProperty(objCtor.get(), "defineProperty"));
        ev::Persistent tagSym(ev::getProperty(symCtor.get(), "toStringTag"));
        if (ev::isFunction(defProp.get()) && !ev::isUndefined(tagSym.get())) {
            ObjectBuilder desc;
            desc.set("value", ev::fromUtf8("Intl"));
            desc.set("configurable", ev::fromBool(true));
            Value args[3] = { intlVal.get(), tagSym.get(), desc.get() };
            ev::call(defProp.get(), objCtor.get(), args);
        }
    }

    ev::registerGlobal("Intl", intlVal.get());
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (gt.found && ev::isObject(gt.value)) {
        ev::Persistent global(gt.value);
        ev::setProperty(global.get(), "Intl", intlVal.get());
    }

    // <Ctor>.prototype[name] = fn, with the prototype and the new function
    // rooted across makeFunction's allocation.
    auto setProtoMethod = [](const char* ctorName, const char* name, uint32_t arity,
                             ev::NativeFn fn) {
        ev::GlobalValue ctorG = ev::globalValue(ctorName);
        if (!ctorG.found || !ev::isObject(ctorG.value)) return;
        ev::Persistent ctor(ctorG.value);
        ev::Persistent proto(ev::getProperty(ctor.get(), "prototype"));
        if (!ev::isObject(proto.get())) return;
        ev::Persistent f(ev::makeFunction(std::move(fn), arity, name));
        ev::setProperty(proto.get(), name, f.get());
    };

    // Number.prototype.toLocaleString
    setProtoMethod("Number", "toLocaleString", 1,
        [](Value thisValue, std::span<const Value> a) {
            double n = ev::isNumber(thisValue) ? ev::toDouble(thisValue) : 0.0;
            NumberFormatOptions opt = parseNumberFormatOptions(a);
            return ev::fromUtf8(formatNumberDetails(n, opt).fullString);
        });

    // Date.prototype.toLocaleDateString & Date.prototype.toLocaleTimeString
    setProtoMethod("Date", "toLocaleDateString", 1,
        [](Value thisValue, std::span<const Value> a) {
            const Rooted date(thisValue);  // the option reads allocate
            DateTimeOptions opt = parseDateTimeOptions(a);
            opt.hasYear = true;
            opt.hasMonth = true;
            opt.hasDay = true;
            return ev::fromUtf8(formatDateTimeDetails(date, opt));
        });
    setProtoMethod("Date", "toLocaleTimeString", 1,
        [](Value thisValue, std::span<const Value> a) {
            const Rooted date(thisValue);  // the option reads allocate
            DateTimeOptions opt = parseDateTimeOptions(a);
            opt.hasHour = true;
            opt.hasMinute = true;
            opt.hasSecond = true;
            return ev::fromUtf8(formatDateTimeDetails(date, opt));
        });

    // String.prototype.localeCompare
    setProtoMethod("String", "localeCompare", 1,
        [](Value thisValue, std::span<const Value> a) {
            std::string s1 = ev::isString(thisValue) ? ev::toUtf8(thisValue) : "";
            std::string s2 = (a.size() > 0 && ev::isString(a[0])) ? ev::toUtf8(a[0]) : "";
            std::span<const Value> collArgs = a.empty() ? a : a.subspan(1);
            CollatorOptions opt = parseCollatorOptions(collArgs);
            return ev::fromDouble(collatorCompare(s1, s2, opt));
        });
}

}  // namespace bro::bronze_host
