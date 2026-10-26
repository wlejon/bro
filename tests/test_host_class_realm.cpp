// bro's HostClass keeps a constructor and prototype per realm
// (src/bronze_host/host_class.cpp): two realms on one thread that install the
// same class get two prototypes, a change one makes to its own is invisible to
// the other, an instance is born on the prototype of the realm it is made in,
// and destroying a realm drops its class while the other's stays. Before, one
// pair served the whole process, so a second realm's install (or a Worker's)
// replaced the first's.
#include "bronze_host/host_builder.h"
#include "bronze_host/host_class.h"

#include "embed/embed.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>

namespace ev = bronze::embed;
using bro::bronze_host::HostClass;
using bro::bronze_host::ObjectBuilder;
using Value = bronze::Value;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

namespace {

struct Payload {
    int value;
};

HostClass g_widget;

void decorate(ObjectBuilder& proto) {
    proto.def("value", 0, [](Value self, std::span<const Value>) {
        auto* p = static_cast<Payload*>(ev::handleData(self));
        return ev::fromDouble(p ? p->value : -1);
    });
}

}  // namespace

int main() {
    g_widget.install("RealmWidget", 0, nullptr, decorate);
    ev::Persistent protoA(g_widget.prototype());
    ev::Persistent ctorA(g_widget.constructor());
    CHECK(ev::isObject(protoA.get()));
    ev::Persistent instA(g_widget.createInstance(std::make_unique<Payload>(Payload{1})));

    ev::Realm* realmB = ev::createRealm();
    CHECK(realmB != nullptr);
    ev::Persistent protoB;
    ev::Persistent instB;
    {
        ev::RealmScope scope(realmB);
        // This realm never installed it: no class here, and a made value is a
        // bare handle rather than an instance of the other realm's class.
        CHECK(ev::isUndefined(g_widget.prototype()));
        g_widget.install("RealmWidget", 0, nullptr, decorate);
        protoB.set(g_widget.prototype());
        CHECK(protoB.get() != protoA.get());
        CHECK(g_widget.constructor() != ctorA.get());
        instB.set(g_widget.createInstance(std::make_unique<Payload>(Payload{2})));
        ev::setProperty(protoB.get(), "patched", ev::fromDouble(7.0));
        CHECK(ev::toDouble(ev::getProperty(instB.get(), "patched")) == 7.0);
    }

    // The first realm's class is untouched by the second's install and patch.
    CHECK(g_widget.prototype() == protoA.get());
    CHECK(g_widget.constructor() == ctorA.get());
    CHECK(ev::isUndefined(ev::getProperty(instA.get(), "patched")));
    ev::Persistent instA2(g_widget.createInstance(std::make_unique<Payload>(Payload{3})));
    CHECK(ev::isUndefined(ev::getProperty(instA2.get(), "patched")));

    // Destroying the second realm leaves the first's class working.
    ev::destroyRealm(realmB);
    CHECK(g_widget.prototype() == protoA.get());
    ev::Persistent method(ev::getProperty(instA2.get(), "value"));
    ev::CallResult r = ev::call(method.get(), instA2.get(), {});
    CHECK(!r.thrown);
    CHECK(ev::toDouble(r.value) == 3.0);

    std::printf("bro_host_class_realm_test: OK\n");
    return 0;
}
