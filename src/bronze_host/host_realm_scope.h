#pragma once

#include <cstdint>

namespace bro::dom { class Document; }

namespace bro::bronze_host {

void initRealmScopeBaseline();
void enterRealmScope(uint64_t scopeId);
void exitRealmScope();
uint64_t currentRealmScope();
bool isChildRealm();
void clearRealmScope(uint64_t scopeId);
void resetAllRealmScopes();

uint64_t scopeIdForDocument(dom::Document* doc);

} // namespace bro::bronze_host
