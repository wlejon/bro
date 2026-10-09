#include "platform/desktop_notifications.h"
#include "platform/desktop_platform.h"

#include <atomic>
#include <cstdlib>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace bro::platform::desktop {

namespace {

std::mutex s_mutex;
std::vector<NotificationRecord> s_records;
std::atomic<uint32_t> s_nextId{1};

#ifdef _WIN32
constexpr UINT kNotificationTrayId = 0xBE;
HWND g_trayOwner = nullptr;
#elif !defined(__APPLE__)

// Dynamic D-Bus definitions for org.freedesktop.Notifications
typedef struct DBusConnection DBusConnection;
typedef struct DBusMessage DBusMessage;
typedef struct DBusError DBusError;

struct DBusMessageIter {
    void* dummy[16];
};

typedef enum {
    DBUS_BUS_SESSION = 0,
    DBUS_BUS_SYSTEM = 1
} DBusBusType;

#define DBUS_TYPE_STRING ((int)'s')
#define DBUS_TYPE_UINT32 ((int)'u')
#define DBUS_TYPE_INT32  ((int)'i')
#define DBUS_TYPE_ARRAY  ((int)'a')

typedef DBusConnection* (*Fn_dbus_bus_get)(DBusBusType, DBusError*);
typedef DBusMessage* (*Fn_dbus_message_new_method_call)(const char*, const char*, const char*, const char*);
typedef void (*Fn_dbus_message_iter_init_append)(DBusMessage*, DBusMessageIter*);
typedef int (*Fn_dbus_message_iter_append_basic)(DBusMessageIter*, int, const void*);
typedef int (*Fn_dbus_message_iter_open_container)(DBusMessageIter*, int, const char*, DBusMessageIter*);
typedef int (*Fn_dbus_message_iter_close_container)(DBusMessageIter*, DBusMessageIter*);
typedef DBusMessage* (*Fn_dbus_connection_send_with_reply_and_block)(DBusConnection*, DBusMessage*, int, DBusError*);
typedef int (*Fn_dbus_message_iter_init)(DBusMessage*, DBusMessageIter*);
typedef void (*Fn_dbus_message_iter_get_basic)(DBusMessageIter*, void*);
typedef void (*Fn_dbus_message_unref)(DBusMessage*);
typedef void (*Fn_dbus_error_init)(DBusError*);
typedef void (*Fn_dbus_error_free)(DBusError*);

struct DBusApi {
    void* handle = nullptr;
    Fn_dbus_bus_get bus_get = nullptr;
    Fn_dbus_message_new_method_call message_new_method_call = nullptr;
    Fn_dbus_message_iter_init_append message_iter_init_append = nullptr;
    Fn_dbus_message_iter_append_basic message_iter_append_basic = nullptr;
    Fn_dbus_message_iter_open_container message_iter_open_container = nullptr;
    Fn_dbus_message_iter_close_container message_iter_close_container = nullptr;
    Fn_dbus_connection_send_with_reply_and_block send_with_reply_and_block = nullptr;
    Fn_dbus_message_iter_init message_iter_init = nullptr;
    Fn_dbus_message_iter_get_basic message_iter_get_basic = nullptr;
    Fn_dbus_message_unref message_unref = nullptr;
    Fn_dbus_error_init error_init = nullptr;
    Fn_dbus_error_free error_free = nullptr;

    bool load() {
        if (handle) return true;
        handle = dlopen("libdbus-1.so.3", RTLD_LAZY);
        if (!handle) handle = dlopen("libdbus-1.so", RTLD_LAZY);
        if (!handle) return false;

        #define LOAD_SYM(member, sym) member = (Fn_##sym)dlsym(handle, #sym); if (!member) return false
        LOAD_SYM(bus_get, dbus_bus_get);
        LOAD_SYM(message_new_method_call, dbus_message_new_method_call);
        LOAD_SYM(message_iter_init_append, dbus_message_iter_init_append);
        LOAD_SYM(message_iter_append_basic, dbus_message_iter_append_basic);
        LOAD_SYM(message_iter_open_container, dbus_message_iter_open_container);
        LOAD_SYM(message_iter_close_container, dbus_message_iter_close_container);
        LOAD_SYM(send_with_reply_and_block, dbus_connection_send_with_reply_and_block);
        LOAD_SYM(message_iter_init, dbus_message_iter_init);
        LOAD_SYM(message_iter_get_basic, dbus_message_iter_get_basic);
        LOAD_SYM(message_unref, dbus_message_unref);
        LOAD_SYM(error_init, dbus_error_init);
        LOAD_SYM(error_free, dbus_error_free);
        #undef LOAD_SYM
        return true;
    }
};

DBusApi g_dbus;
bool g_dbusTried = false;

uint32_t sendDbusNotification(const std::string& title, const std::string& body, const NotificationOptions& options) {
    if (!g_dbusTried) {
        g_dbusTried = true;
        g_dbus.load();
    }
    if (!g_dbus.handle) return 0;

    struct {
        const char* name;
        const char* message;
        unsigned int dummy1 : 1;
        unsigned int dummy2 : 1;
        unsigned int dummy3 : 1;
        unsigned int dummy4 : 1;
        unsigned int dummy5 : 1;
        void* padding1;
    } err;
    g_dbus.error_init((DBusError*)&err);

    DBusConnection* conn = g_dbus.bus_get(DBUS_BUS_SESSION, (DBusError*)&err);
    if (!conn) {
        g_dbus.error_free((DBusError*)&err);
        return 0;
    }

    DBusMessage* msg = g_dbus.message_new_method_call(
        "org.freedesktop.Notifications",
        "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications",
        "Notify"
    );
    if (!msg) return 0;

    DBusMessageIter args;
    g_dbus.message_iter_init_append(msg, &args);

    const char* appName = "bro";
    uint32_t replacesId = options.replacesId;
    const char* appIcon = options.icon.c_str();
    const char* summary = title.c_str();
    const char* bodyText = body.c_str();
    int32_t timeout = options.timeoutMs;

    g_dbus.message_iter_append_basic(&args, DBUS_TYPE_STRING, &appName);
    g_dbus.message_iter_append_basic(&args, DBUS_TYPE_UINT32, &replacesId);
    g_dbus.message_iter_append_basic(&args, DBUS_TYPE_STRING, &appIcon);
    g_dbus.message_iter_append_basic(&args, DBUS_TYPE_STRING, &summary);
    g_dbus.message_iter_append_basic(&args, DBUS_TYPE_STRING, &bodyText);

    // actions: string array (as)
    DBusMessageIter actionsIter;
    g_dbus.message_iter_open_container(&args, DBUS_TYPE_ARRAY, "s", &actionsIter);
    g_dbus.message_iter_close_container(&args, &actionsIter);

    // hints: dictionary (a{sv})
    DBusMessageIter hintsIter;
    g_dbus.message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &hintsIter);
    g_dbus.message_iter_close_container(&args, &hintsIter);

    g_dbus.message_iter_append_basic(&args, DBUS_TYPE_INT32, &timeout);

    uint32_t notificationId = 0;
    DBusMessage* reply = g_dbus.send_with_reply_and_block(conn, msg, 1000, (DBusError*)&err);
    if (reply) {
        DBusMessageIter replyIter;
        if (g_dbus.message_iter_init(reply, &replyIter)) {
            g_dbus.message_iter_get_basic(&replyIter, &notificationId);
        }
        g_dbus.message_unref(reply);
    }
    g_dbus.message_unref(msg);
    g_dbus.error_free((DBusError*)&err);

    return notificationId;
}

#endif

} // namespace

uint32_t showNotification(
    const Window* window,
    const std::string& title,
    const std::string& body,
    const NotificationOptions& options
) {
    const uint32_t generatedId = s_nextId.fetch_add(1, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_records.push_back({generatedId, title, body, options});
    }

    if (isHeadless()) {
        return generatedId;
    }

#ifdef _WIN32
    HWND hwnd = hwndOf(window);
    if (!hwnd) return generatedId;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kNotificationTrayId;
    if (!g_trayOwner) {
        nid.uFlags = NIF_ICON | NIF_TIP;
        nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
        if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
        copyWide(nid.szTip, ARRAYSIZE(nid.szTip), L"bro");
        if (Shell_NotifyIconW(NIM_ADD, &nid)) {
            g_trayOwner = hwnd;
        }
    }
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    copyWide(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), utf8ToWide(title.empty() ? "bro" : title));
    copyWide(nid.szInfo, ARRAYSIZE(nid.szInfo), utf8ToWide(body.empty() ? " " : body));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    return generatedId;
#elif defined(__APPLE__)
    (void)window;
    std::string cmd = "osascript -e 'display notification \"" + body + "\" with title \"" + title + "\"' >/dev/null 2>&1 &";
    std::system(cmd.c_str());
    return generatedId;
#else
    (void)window;
    uint32_t dbusId = sendDbusNotification(title, body, options);
    if (dbusId > 0) return dbusId;

    // Fallback to notify-send
    std::string cmd = "notify-send \"" + title + "\" \"" + body + "\" >/dev/null 2>&1 &";
    std::system(cmd.c_str());
    return generatedId;
#endif
}

std::vector<NotificationRecord> getRecordedNotifications() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_records;
}

void clearRecordedNotifications() {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_records.clear();
}

} // namespace bro::platform::desktop
