#pragma once

// What a page's write to each of the host's globals does: the WebIDL
// attribute kinds of the Window's (and a WorkerGlobalScope's) members, over
// bronze's host-global kinds (embed.h, GlobalKind / registerGlobalAccessor).
//
//   readonly attribute (document, navigator, history, customElements, ...):
//     registerHostGlobal(name, v, GlobalKind::Readonly). A sloppy write is a
//     no-op, a strict one a TypeError, and the bare name keeps answering.
//   [Replaceable] (self, parent, frames, screen, performance, ...) and every
//     operation and interface object: registerHostGlobal(name, v) — the
//     page's write makes the name its own data property.
//   [Replaceable] attribute whose value is live (innerWidth, scrollY,
//     devicePixelRatio, ...): defineReplaceableAttribute, a getter on the
//     global object whose setter replaces it with the written value.
//   read-write attribute (name, status, the on* handlers) and location's
//     [PutForwards=href]: registerHostGlobalAccessor.
//
// The kinds are recorded per thread (each realm of a thread is installed
// alike), so a reload can put the app realm's globals back with the kinds
// they were installed with (host_global_sync.cpp).

#include "bronze_host/host_builder.h"

#include <string>

namespace bro::bronze_host {

void registerHostGlobal(const char* name, Value value, ev::GlobalKind kind = ev::GlobalKind::Replaceable);

// The kind `name` was last registered with on this thread through
// registerHostGlobal (Replaceable when it never was), and whether it is an
// accessor global (registerHostGlobalAccessor).
ev::GlobalKind hostGlobalKind(const std::string& name);
bool isHostGlobalAccessor(const std::string& name);

// An accessor global: reads and writes, bare or through `window.x`, call
// `getter` / `setter` with the global object as `this`. A null setter makes
// it read-only; `configurable` false makes it [LegacyUnforgeable] (location).
void registerHostGlobalAccessor(const char* name, ev::NativeFn getter, ev::NativeFn setter,
                                bool configurable = true);

// A [Replaceable] attribute on the global object `global` answered by
// `getter`; a write defines the written value as the global's own data
// property in its place (WebIDL 3.7.6.3, [Replaceable]'s setter).
void defineReplaceableAttribute(ObjectBuilder& global, const char* name, ev::NativeFn getter);

}  // namespace bro::bronze_host
