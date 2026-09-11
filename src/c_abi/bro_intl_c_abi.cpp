// =============================================================================
// bro_intl_c_abi.cpp — C++ forwarding implementations for bro.intl
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_intl_c_abi.h"
#include <cstdint>
#include <string>
#include <cstring>

struct BroNumberFormatImpl {
    std::string formatted;
};

struct BroDateTimeFormatImpl {
    std::string formatted;
};

struct BroPluralRulesImpl {};
struct BroCollatorImpl {};
struct BroListFormatImpl {
    std::string formatted;
};

struct BroRelativeTimeFormatImpl {
    std::string formatted;
};

struct BroDisplayNamesImpl {
    std::string name;
};

extern "C" {

// --- Interface bro.intl.NumberFormat ---
void* bro_NumberFormat_create(void* /*locales*/, void* /*options*/) {
    return new BroNumberFormatImpl();
}

void bro_NumberFormat_destroy(void* self) {
    delete static_cast<BroNumberFormatImpl*>(self);
}

const char* bro_NumberFormat_format(void* self, double value) {
    if (!self) return "";
    auto* nf = static_cast<BroNumberFormatImpl*>(self);
    nf->formatted = std::to_string(value);
    return nf->formatted.c_str();
}

void* bro_NumberFormat_formatToParts(void* /*self*/, double /*value*/) {
    return nullptr;
}

void* bro_NumberFormat_resolvedOptions(void* /*self*/) {
    return nullptr;
}

void* bro_NumberFormat_supportedLocalesOf(void* /*locales*/, void* /*options*/) {
    return nullptr;
}

// --- Interface bro.intl.DateTimeFormat ---
void* bro_DateTimeFormat_create(void* /*locales*/, void* /*options*/) {
    return new BroDateTimeFormatImpl();
}

void bro_DateTimeFormat_destroy(void* self) {
    delete static_cast<BroDateTimeFormatImpl*>(self);
}

const char* bro_DateTimeFormat_format(void* self, void* /*date*/) {
    if (!self) return "";
    auto* df = static_cast<BroDateTimeFormatImpl*>(self);
    df->formatted = "2026-09-11T00:00:00.000Z";
    return df->formatted.c_str();
}

void* bro_DateTimeFormat_formatToParts(void* /*self*/, void* /*date*/) {
    return nullptr;
}

void* bro_DateTimeFormat_resolvedOptions(void* /*self*/) {
    return nullptr;
}

void* bro_DateTimeFormat_supportedLocalesOf(void* /*locales*/, void* /*options*/) {
    return nullptr;
}

// --- Interface bro.intl.PluralRules ---
void* bro_PluralRules_create(void* /*locales*/, void* /*options*/) {
    return new BroPluralRulesImpl();
}

void bro_PluralRules_destroy(void* self) {
    delete static_cast<BroPluralRulesImpl*>(self);
}

const char* bro_PluralRules_select(void* /*self*/, double /*value*/) {
    return "other";
}

void* bro_PluralRules_resolvedOptions(void* /*self*/) {
    return nullptr;
}

void* bro_PluralRules_supportedLocalesOf(void* /*locales*/, void* /*options*/) {
    return nullptr;
}

// --- Interface bro.intl.Collator ---
void* bro_Collator_create(void* /*locales*/, void* /*options*/) {
    return new BroCollatorImpl();
}

void bro_Collator_destroy(void* self) {
    delete static_cast<BroCollatorImpl*>(self);
}

int32_t bro_Collator_compare(void* /*self*/, const char* string1, const char* string2) {
    const char* s1 = string1 ? string1 : "";
    const char* s2 = string2 ? string2 : "";
    return std::strcmp(s1, s2);
}

void* bro_Collator_resolvedOptions(void* /*self*/) {
    return nullptr;
}

void* bro_Collator_supportedLocalesOf(void* /*locales*/, void* /*options*/) {
    return nullptr;
}

// --- Interface bro.intl.ListFormat ---
void* bro_ListFormat_create(void* /*locales*/, void* /*options*/) {
    return new BroListFormatImpl();
}

void bro_ListFormat_destroy(void* self) {
    delete static_cast<BroListFormatImpl*>(self);
}

const char* bro_ListFormat_format(void* self, void* /*list*/) {
    if (!self) return "";
    auto* lf = static_cast<BroListFormatImpl*>(self);
    lf->formatted = "";
    return lf->formatted.c_str();
}

void* bro_ListFormat_formatToParts(void* /*self*/, void* /*list*/) {
    return nullptr;
}

void* bro_ListFormat_resolvedOptions(void* /*self*/) {
    return nullptr;
}

void* bro_ListFormat_supportedLocalesOf(void* /*locales*/, void* /*options*/) {
    return nullptr;
}

// --- Interface bro.intl.RelativeTimeFormat ---
void* bro_RelativeTimeFormat_create(void* /*locales*/, void* /*options*/) {
    return new BroRelativeTimeFormatImpl();
}

void bro_RelativeTimeFormat_destroy(void* self) {
    delete static_cast<BroRelativeTimeFormatImpl*>(self);
}

const char* bro_RelativeTimeFormat_format(void* self, double value, const char* unit) {
    if (!self) return "";
    auto* rf = static_cast<BroRelativeTimeFormatImpl*>(self);
    rf->formatted = std::to_string(static_cast<int64_t>(value)) + " " + (unit ? unit : "");
    return rf->formatted.c_str();
}

void* bro_RelativeTimeFormat_formatToParts(void* /*self*/, double /*value*/, const char* /*unit*/) {
    return nullptr;
}

void* bro_RelativeTimeFormat_resolvedOptions(void* /*self*/) {
    return nullptr;
}

void* bro_RelativeTimeFormat_supportedLocalesOf(void* /*locales*/, void* /*options*/) {
    return nullptr;
}

// --- Interface bro.DisplayNames ---
void* bro_DisplayNames_create(void* /*locales*/, void* /*options*/) {
    return new BroDisplayNamesImpl();
}

void bro_DisplayNames_destroy(void* self) {
    delete static_cast<BroDisplayNamesImpl*>(self);
}

const char* bro_DisplayNames_of(void* self, const char* code) {
    if (!self) return code ? code : "";
    auto* dn = static_cast<BroDisplayNamesImpl*>(self);
    dn->name = code ? code : "";
    return dn->name.c_str();
}

void* bro_DisplayNames_resolvedOptions(void* /*self*/) {
    return nullptr;
}

void* bro_DisplayNames_supportedLocalesOf(void* /*locales*/, void* /*options*/) {
    return nullptr;
}

// --- Namespace bro.Intl ---
void* bro_Intl_getCanonicalLocales(void* /*locales*/) {
    return nullptr;
}

} // extern "C"
