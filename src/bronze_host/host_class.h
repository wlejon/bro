#pragma once

// HostClass, the ctor/prototype/handle shape every wrapper family is built
// from (host_class.cpp), and the tags that tell one family's handle payload
// from another's.

#include "embed/embed.h"

#include <cstdint>
#include <functional>
#include <memory>

namespace bronze::embed {
Value setPrototype(Value obj, Value proto);
}  // namespace bronze::embed

namespace bro::bronze_host {

namespace ev = bronze::embed;
using Value = bronze::Value;

// host_builder.h owns it; a file that only names it needs nothing more.
struct ObjectBuilder;

// One host class: a registered constructor, a prototype minted from it and
// decorated ONCE, and instances born on that prototype. See host_class.cpp for
// why each step is what it is, and host_image.cpp for a converted family read
// end to end.
//
// The win over a bare handle is two things at once: one copy of each method
// per CLASS instead of one per instance, and `x instanceof Name` answering
// true instead of false.
//
// Declare one at file scope per class — the members are pointers, so it is
// constant-initialised and has no static constructor to order.
class HostClass {
public:
    // Mint, decorate, and register `name`. `body` runs for `new Name(...)`;
    // pass nullptr for a class the program may name but not construct, which
    // is what makeBrandConstructor used to be — except that this one brands.
    // Call once, from the family's install function.
    void install(const char* name, uint32_t arity, ev::NativeFn body,
                 const std::function<void(ObjectBuilder&)>& decorate);

    void init(const char* name, const std::function<void(ObjectBuilder&)>& decorate) {
        install(name, 0, nullptr, decorate);
    }

    template <typename T>
    Value createInstance(std::unique_ptr<T> cell, ev::Finalize when = ev::Finalize::InSweep) const {
        if (!cell) return ev::undefined();
        T* raw = cell.release();
        return make(raw, [](void* p) { delete static_cast<T*>(p); }, when);
    }

    // Register a second name for the same constructor (Image and
    // HTMLImageElement).
    void alias(const char* name) const;

    // Bind this HostClass to an existing constructor and its prototype by name.
    void bind(const char* name);

    // `class This extends Base`: chain this prototype onto the base's, so an
    // instance inherits both surfaces and answers `instanceof` for both (an
    // HTMLDivElement IS an HTMLElement). Call AFTER both installs.
    // Prototypes are plain objects, not handle cells, so re-parenting one
    // costs nothing an instance pays for.
    void inherit(const HostClass& base) const;

    // An instance born on this class's prototype, or a bare cell if install()
    // has not run.
    Value make(void* data, ev::HandleDestructor dtor,
               ev::Finalize when = ev::Finalize::InSweep) const;

    // A property on the CONSTRUCTOR, where a class `static` member lands
    // (Node.TEXT_NODE). `name`, `length` and `prototype` are
    // refused by bronze and must not be passed.
    void setStatic(const char* name, Value v) const;

    Value prototype() const;
    Value constructor() const;

private:
    // Heap-allocated and never freed, on purpose: a static destructor would
    // run these after the engine has torn the runtime down. host_class.cpp
    // has the full reasoning.
    ev::Persistent* proto_ = nullptr;
    ev::Persistent* ctor_ = nullptr;
};

// ---------------------------------------------------------------------------
// Handle tags
// ---------------------------------------------------------------------------

// Every host object with a C++ payload is an embed handle, and every unwrap in
// this layer reaches it through the same embed::handleData — which answers a
// void* with no type on it. So each payload struct starts with a uint32_t tag
// in the same position as WebGLCell::kind (webgl_internal.h), carrying a value no
// WebGL kind uses: an Image handed to idOf(v, WebGLCell::Texture) reads kHostImageTag,
// fails the kind compare and answers 0, and a WebGLTexture handed to
// hostImageOf reads a kind of 1..8 and answers nullptr. The alternative — each
// unwrap trusting that it is only ever passed its own cells — is the shape of
// bug that reads a Shape* as a Value.
inline constexpr uint32_t kHostElementTag = 0x454C454Du;  // 'ELEM'
inline constexpr uint32_t kHostImageTag = 0x494D4147u;    // 'IMAG'
inline constexpr uint32_t kHostVideoEncoderTag = 0x56454E43u;  // 'VENC'
inline constexpr uint32_t kHostGifEncoderTag   = 0x47454E43u;  // 'GENC'

}  // namespace bro::bronze_host
