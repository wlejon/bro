// The desktop's light/dark preference from the XDG desktop portal
// (org.freedesktop.portal.Settings, org.freedesktop.appearance color-scheme),
// which is where SDL's Wayland backend reads it too. libdbus is loaded at run
// time, as desktop_notifications.cpp does, so it is no build dependency.
//
// The first question is answered on the asking thread (with a short D-Bus
// timeout, so a session without a portal costs ~nothing); after that a
// background thread re-reads it every two seconds and flags a change.
#include "platform/wayland/wayland_backend.h"

#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <type_traits>

namespace bro::platform::wl {

namespace {

struct DBusConnection;
struct DBusMessage;
struct DBusMessageIter {
    void* dummy[16];
};
struct DBusErrorStorage {
    const char* name;
    const char* message;
    unsigned int dummy : 5;
    void* padding;
};

constexpr int kBusSession = 0;
constexpr int kTypeString = 's';
constexpr int kTypeUint32 = 'u';
constexpr int kTypeVariant = 'v';

struct DBus {
    void* handle = nullptr;
    DBusConnection* (*bus_get)(int, void*) = nullptr;
    DBusMessage* (*new_method_call)(const char*, const char*, const char*, const char*) = nullptr;
    int (*append_args)(DBusMessage*, int, ...) = nullptr;
    DBusMessage* (*send_block)(DBusConnection*, DBusMessage*, int, void*) = nullptr;
    int (*iter_init)(DBusMessage*, DBusMessageIter*) = nullptr;
    int (*iter_get_arg_type)(DBusMessageIter*) = nullptr;
    void (*iter_recurse)(DBusMessageIter*, DBusMessageIter*) = nullptr;
    void (*iter_get_basic)(DBusMessageIter*, void*) = nullptr;
    void (*message_unref)(DBusMessage*) = nullptr;
    void (*error_init)(void*) = nullptr;
    void (*error_free)(void*) = nullptr;
    DBusConnection* conn = nullptr;

    bool load() {
        handle = dlopen("libdbus-1.so.3", RTLD_LAZY);
        if (!handle) handle = dlopen("libdbus-1.so", RTLD_LAZY);
        if (!handle) return false;
        auto sym = [this](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(handle, name));
            return fn != nullptr;
        };
        if (!(sym(bus_get, "dbus_bus_get") && sym(new_method_call, "dbus_message_new_method_call") &&
              sym(append_args, "dbus_message_append_args") &&
              sym(send_block, "dbus_connection_send_with_reply_and_block") &&
              sym(iter_init, "dbus_message_iter_init") && sym(iter_get_arg_type, "dbus_message_iter_get_arg_type") &&
              sym(iter_recurse, "dbus_message_iter_recurse") && sym(iter_get_basic, "dbus_message_iter_get_basic") &&
              sym(message_unref, "dbus_message_unref") && sym(error_init, "dbus_error_init") &&
              sym(error_free, "dbus_error_free")))
            return false;
        DBusErrorStorage err{};
        error_init(&err);
        conn = bus_get(kBusSession, &err);
        error_free(&err);
        return conn != nullptr;
    }

    // ReadOne (portal v2) answers v(u); the older Read answers v(v(u)).
    int read(const char* method) {
        DBusMessage* msg = new_method_call("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                           "org.freedesktop.portal.Settings", method);
        if (!msg) return -1;
        const char* ns = "org.freedesktop.appearance";
        const char* key = "color-scheme";
        append_args(msg, kTypeString, &ns, kTypeString, &key, 0 /* DBUS_TYPE_INVALID */);
        DBusErrorStorage err{};
        error_init(&err);
        DBusMessage* reply = send_block(conn, msg, 250, &err);
        message_unref(msg);
        error_free(&err);
        if (!reply) return -1;
        int result = -1;
        DBusMessageIter it;
        if (iter_init(reply, &it)) {
            DBusMessageIter cur = it;
            for (int depth = 0; depth < 4 && iter_get_arg_type(&cur) == kTypeVariant; ++depth) {
                DBusMessageIter inner;
                iter_recurse(&cur, &inner);
                cur = inner;
            }
            if (iter_get_arg_type(&cur) == kTypeUint32) {
                uint32_t v = 0;
                iter_get_basic(&cur, &v);
                result = static_cast<int>(v);
            }
        }
        message_unref(reply);
        return result;
    }

    int colorScheme() {
        int v = read("ReadOne");
        if (v < 0) v = read("Read");
        return v < 0 ? 0 : v;
    }
};

struct Watcher {
    std::mutex mu;
    std::condition_variable cv;
    DBus dbus;
    bool loaded = false;
    std::atomic<int> scheme{0};
    std::atomic<bool> changed{false};
    std::thread thread;
    bool stop = false;

    Watcher() {
        loaded = dbus.load();
        if (!loaded) return;
        scheme = dbus.colorScheme();
        thread = std::thread([this] {
            std::unique_lock<std::mutex> lock(mu);
            while (!cv.wait_for(lock, std::chrono::seconds(2), [this] { return stop; })) {
                lock.unlock();
                const int v = dbus.colorScheme();
                lock.lock();
                if (v != scheme.load()) {
                    scheme = v;
                    changed = true;
                }
            }
        });
    }
    ~Watcher() {
        if (!thread.joinable()) return;
        {
            std::lock_guard<std::mutex> lock(mu);
            stop = true;
        }
        cv.notify_all();
        thread.join();
    }
};

Watcher& watcher() {
    static Watcher w;
    return w;
}

}  // namespace

int portalColorScheme() { return watcher().scheme.load(); }

bool portalColorSchemeChanged() { return watcher().changed.exchange(false); }

}  // namespace bro::platform::wl
