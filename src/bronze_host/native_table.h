// native_table.h — how a registration table names the C entry point it
// registers: BRO_NATIVE_FN(f) for a native, BRO_NATIVE_DTOR(f) for a class's
// handle destructor.
//
// The tables (natives/*/native_*_register.cpp and native_tables.cpp) are
// compiled twice. Into bro_bronze_host, where the macros are the entry
// points' addresses. And into bro-native-manifest with BRO_NATIVE_TABLE_ONLY,
// where they are a placeholder and null: the manifest records each native's
// path, kind and types, never a pointer (bronze embed's nativeManifestJson),
// so the tool links the tables and bronze's registry and nothing they
// register. It relinks when a table changes, not on every engine change, and
// a C++ rebuild does not wait on it. The manifest is still printed from the
// same registrations the runtime makes.
#pragma once

#if BRO_NATIVE_TABLE_ONLY
namespace bro::bronze_host {
inline void nativeTablePlaceholder() {}
}  // namespace bro::bronze_host
#define BRO_NATIVE_FN(f) reinterpret_cast<void*>(&::bro::bronze_host::nativeTablePlaceholder)
#define BRO_NATIVE_DTOR(f) nullptr
#else
#define BRO_NATIVE_FN(f) reinterpret_cast<void*>(&(f))
#define BRO_NATIVE_DTOR(f) (&(f))
#endif
