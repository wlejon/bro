#pragma once

// The interpreter bridge: compiled code's `new Function(source)`, answered by
// the QuickJS realm the same Engine is already running.
//
// bronze refuses dynamic code by name — compiling a string at run time is the
// one thing an ahead-of-time compiler cannot do — and for a standalone bronze
// program that refusal is the whole story. bro is not that situation: it runs
// an interpreter beside the compiled code, on the same thread, against the
// same DOM. So bronze offers a seam (embed.h, setDynamicFunctionHook) and this
// file is what bro puts in it.
//
// WHAT THIS COSTS THE BOUNDARY RULE. src/bronze_host/README.md states it
// flatly: "Engine objects are shared. Event data is copied. Heap values never
// cross." That rule was written for the EVENT path, where a copy is not merely
// safe but correct — an event's fields are a snapshot and a listener has no
// business holding the dispatch's own object. It cannot hold here. A script
// the page compiled is handed the compiled program's renderer, its scene and
// its camera, and it calls methods on them; a copy of a Scene is not a Scene.
// So this file does the other thing, and does it in one place so the rest of
// the layer keeps the simpler rule: values are WRAPPED, never copied, and each
// wrapper forwards to the object it stands for.
//
// The two collectors do not trace each other, so the crossing table roots the
// FOREIGN half of every crossing and holds its OWN side only weakly — a
// QuickJS finalizer on the outbound wrapper class, a bronze WeakRef on the
// inbound proxy. When a wrapper dies, sweepInterpBridge (called once per frame
// from hostFrame) releases the row and the foreign object with it, so the
// table's size tracks what is LIVE across the boundary, not what has ever
// crossed. resetInterpBridge() still empties it when the app realm goes away.
//
// Typed arrays do not wrap at all: both realms hold real views over one
// external byte store (embed.h's externalizeArrayBuffer), so writes on either
// side are simply visible on the other.

#include "embed/embed.h"

namespace bro::engine { class Engine; }

namespace bro::bronze_host {

namespace ev = bronze::embed;
using Value = bronze::Value;

// Install the hook. Called once from installWebHostGlobals, after the Engine's
// QuickJS realm exists — a compiled `new Function` before that point still
// gets bronze's own refusal, which is the truthful answer when there is no
// interpreter to delegate to.
void installInterpBridge(engine::Engine& engine);

// The value the interpreted realm has under `name`, as something compiled code
// can use — or undefined when the page never defined it.
//
// This is what a vendor library IS to a compiled app. The page loads
// CodeMirror, signals, esprima and the rest with ordinary <script> tags, into
// the QuickJS realm; the compiled program reads them as bare globals. Before
// the bridge, the only way to answer was to REIMPLEMENT them in C++, and
// src/bronze_host/host_vendor_globals.cpp did — a CodeMirror whose getValue
// answered a string held in a shared_ptr and whose every other method was a
// no-op. It looked like an editor and edited nothing. Bridging the real object
// is not a better fake; it is the library.
Value bridgeJsGlobal(const char* name);

// Reclaim rows whose own-side wrapper has died: run once per frame, from
// hostFrame, after the microtask/finalizer drain — a plain host stack where
// releasing either half is unconditionally safe.
void sweepInterpBridge();

// Drop every crossing. The wrappers handed out before this keep working
// against the objects they already hold; what goes is the table's claim on
// them, which is what lets both collectors reclaim the graph.
void resetInterpBridge();

}  // namespace bro::bronze_host
