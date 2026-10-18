// native_net_register.cpp — registers every native of native_net_decl.h with
// bronze (embed::registerNative) under __bro_native.net. Hand-maintained: each
// registration's signature matches its prototype there. Call registerNatives_net from registerBroNatives
// (src/bronze_host/host_natives.h) on the thread that runs the program.
//
// Class members are Function-kind natives taking the handle first (the wrapper's
// receiver is dynamic, so a Method-kind registration would be unreachable from it);
// each class has one Constructor-kind registration, which mints the class, and its
// prototype is published as __bro_native.net.<Class>Proto for js/net.js to chain.

#include "native_net_decl.h"
#include "bronze_host/native_table.h"
#include "bronze_host/host_natives.h"

#if BRO_WITH_NET

#include "embed/embed.h"

#include <initializer_list>
#include <string>

namespace bro::bronze_host {

namespace {

namespace ev = bronze::embed;

ev::NativeSignature sig(const char* ret, std::initializer_list<const char*> params, ev::NativeKind kind) {
    ev::NativeSignature s;
    s.returnType = ret;
    for (const char* p : params) s.paramTypes.emplace_back(p);
    s.kind = kind;
    return s;
}

bool fn(const char* path, void* f, const char* ret, std::initializer_list<const char*> params, std::string* error) {
    return ev::registerNative(path, f, sig(ret, params, ev::NativeKind::Function), error);
}

}  // namespace

bool registerNatives_net(std::string* error) {
    const bool ok =
        fn("__bro_native.net.host", BRO_NATIVE_FN(bro_net_host), "void", {"i32", "dynamic"}, error) &&
        fn("__bro_native.net.unhost", BRO_NATIVE_FN(bro_net_unhost), "void", {}, error) &&
        fn("__bro_native.net.connect", BRO_NATIVE_FN(bro_net_connect), "i32", {"str", "i32", "dynamic"}, error) &&
        fn("__bro_native.net.disconnect", BRO_NATIVE_FN(bro_net_disconnect), "void", {"i32", "i32"}, error) &&
        fn("__bro_native.net.disconnectAll", BRO_NATIVE_FN(bro_net_disconnectAll), "void", {}, error) &&
        fn("__bro_native.net.send", BRO_NATIVE_FN(bro_net_send), "void", {"i32", "u8[]", "i32"}, error) &&
        fn("__bro_native.net.broadcast", BRO_NATIVE_FN(bro_net_broadcast), "void", {"u8[]", "i32"}, error) &&
        fn("__bro_native.net.peers", BRO_NATIVE_FN(bro_net_peers), "f64[]", {}, error) &&
        fn("__bro_native.net.getPeerAddress", BRO_NATIVE_FN(bro_net_getPeerAddress), "str", {"i32"}, error) &&
        fn("__bro_native.net.stats", BRO_NATIVE_FN(bro_net_stats), "str", {}, error) &&
        fn("__bro_native.net.getPeerStats", BRO_NATIVE_FN(bro_net_getPeerStats), "str", {"i32"}, error) &&
        fn("__bro_native.net.setPeerSimulatedLoss", BRO_NATIVE_FN(bro_net_setPeerSimulatedLoss), "void", {"i32", "f64", "f64", "f64"}, error);
    if (!ok) return false;
    return true;
}

bool registerNetNatives(std::string* error) {
    if (!registerNatives_net(error)) return false;
    if (!natives::fn("__bro_native.net._setDispatcher",
                       BRO_NATIVE_FN(bro_net_setDispatcher),
                       "void", {"dynamic"}, error)) return false;
    if (!natives::fn("__bro_native.net.isHosting",
                       BRO_NATIVE_FN(bro_net_isHosting),
                       "bool", {}, error)) return false;
    if (!natives::fn("__bro_native.net.init",
                       BRO_NATIVE_FN(bro_net_init),
                       "bool", {}, error)) return false;
    if (!natives::fn("__bro_native.net.sendClone",
                       BRO_NATIVE_FN(bro_net_sendCloneRaw),
                       "bool", {"i32", "dynamic", "dynamic"}, error)) return false;
    if (!natives::fn("__bro_native.net.broadcastClone",
                       BRO_NATIVE_FN(bro_net_broadcastCloneRaw),
                       "void", {"dynamic", "dynamic"}, error)) return false;
    if (!natives::fn("__bro_native.net.sendRawOpts",
                       BRO_NATIVE_FN(bro_net_sendRawOpts),
                       "bool", {"i32", "u8[]", "dynamic"}, error)) return false;
    if (!natives::fn("__bro_native.net.broadcastRawOpts",
                       BRO_NATIVE_FN(bro_net_broadcastRawOpts),
                       "void", {"u8[]", "dynamic"}, error)) return false;
    if (!natives::fn("__bro_native.net._sendUnframed",
                       BRO_NATIVE_FN(bro_net_sendUnframed),
                       "bool", {"i32", "u8[]", "dynamic"}, error)) return false;
    return true;
}

}  // namespace bro::bronze_host

#else  // !(BRO_WITH_NET)

#include <string>

namespace bro::bronze_host {

// Compiled out: nothing is registered and net.js is not to be installed.
bool registerNatives_net(std::string*) { return true; }

bool registerNetNatives(std::string*) { return true; }

}  // namespace bro::bronze_host

#endif  // BRO_WITH_NET
