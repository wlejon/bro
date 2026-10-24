#pragma once

#include <cstdint>
#include <string>

namespace bro::dom { class Document; }

namespace bro::bronze_host {

void initRealmScopeBaseline();
// The main thread's promise jobs run in the realm that subscribed them.
void installJobRealmHooks();
void enterRealmScope(uint64_t scopeId);
void exitRealmScope();
uint64_t currentRealmScope();
bool isChildRealm();
void clearRealmScope(uint64_t scopeId);
void resetAllRealmScopes();

// Synchronizes registered host globals (`ev::registerGlobal`) with any
// replaceable properties a script has overwritten on `globalThis`, so bare
// global reads in subsequent scripts, `-e`, or `bro-ctl eval` see the new
// binding.
void syncHostGlobalsFromGlobalThis();
bool isBaselineHostGlobalName(const std::string& name);

uint64_t scopeIdForDocument(dom::Document* doc);

} // namespace bro::bronze_host
