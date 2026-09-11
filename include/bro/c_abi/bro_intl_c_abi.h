// =============================================================================
// bro_intl_c_abi.h — Pure C-ABI declarations for bro.intl
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_INTL_C_ABI_H
#define BRO_INTL_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.intl.NumberFormat ---
void* bro_NumberFormat_create(void* locales, void* options);
void  bro_NumberFormat_destroy(void* self);
const char* bro_NumberFormat_format(void* self, double value);
void* bro_NumberFormat_formatToParts(void* self, double value);
void* bro_NumberFormat_resolvedOptions(void* self);
void* bro_NumberFormat_supportedLocalesOf(void* locales, void* options);

// --- Interface bro.intl.DateTimeFormat ---
void* bro_DateTimeFormat_create(void* locales, void* options);
void  bro_DateTimeFormat_destroy(void* self);
const char* bro_DateTimeFormat_format(void* self, void* date);
void* bro_DateTimeFormat_formatToParts(void* self, void* date);
void* bro_DateTimeFormat_resolvedOptions(void* self);
void* bro_DateTimeFormat_supportedLocalesOf(void* locales, void* options);

// --- Interface bro.intl.PluralRules ---
void* bro_PluralRules_create(void* locales, void* options);
void  bro_PluralRules_destroy(void* self);
const char* bro_PluralRules_select(void* self, double value);
void* bro_PluralRules_resolvedOptions(void* self);
void* bro_PluralRules_supportedLocalesOf(void* locales, void* options);

// --- Interface bro.intl.Collator ---
void* bro_Collator_create(void* locales, void* options);
void  bro_Collator_destroy(void* self);
int32_t bro_Collator_compare(void* self, const char* string1, const char* string2);
void* bro_Collator_resolvedOptions(void* self);
void* bro_Collator_supportedLocalesOf(void* locales, void* options);

// --- Interface bro.intl.ListFormat ---
void* bro_ListFormat_create(void* locales, void* options);
void  bro_ListFormat_destroy(void* self);
const char* bro_ListFormat_format(void* self, void* list);
void* bro_ListFormat_formatToParts(void* self, void* list);
void* bro_ListFormat_resolvedOptions(void* self);
void* bro_ListFormat_supportedLocalesOf(void* locales, void* options);

// --- Interface bro.intl.RelativeTimeFormat ---
void* bro_RelativeTimeFormat_create(void* locales, void* options);
void  bro_RelativeTimeFormat_destroy(void* self);
const char* bro_RelativeTimeFormat_format(void* self, double value, const char* unit);
void* bro_RelativeTimeFormat_formatToParts(void* self, double value, const char* unit);
void* bro_RelativeTimeFormat_resolvedOptions(void* self);
void* bro_RelativeTimeFormat_supportedLocalesOf(void* locales, void* options);

// --- Interface bro.intl.DisplayNames ---
void* bro_DisplayNames_create(void* locales, void* options);
void  bro_DisplayNames_destroy(void* self);
const char* bro_DisplayNames_of(void* self, const char* code);
void* bro_DisplayNames_resolvedOptions(void* self);
void* bro_DisplayNames_supportedLocalesOf(void* locales, void* options);

// --- Namespace bro.Intl ---
void* bro_Intl_getCanonicalLocales(void* locales);

#ifdef __cplusplus
}
#endif

#endif // BRO_INTL_C_ABI_H
