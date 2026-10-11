#include "platform/desktop_notifications.h"
#include "platform/desktop_platform.h"
#include "util/main_loop_wake.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern "C" char** environ;
#endif

namespace bro::platform::desktop {

namespace {

std::mutex s_mutex;
std::vector<NotificationRecord> s_records;
std::atomic<uint32_t> s_nextId{1};

std::mutex s_activationMutex;
std::vector<NotificationActivation> s_activations;
// What each notification of this run was posted with, by id: what a
// D-Bus ActionInvoked (which carries only the id) and a headless click
// report back.
std::map<uint32_t, std::string> s_payloads;

void record(uint32_t id, const std::string& title, const std::string& body, const NotificationOptions& options,
            const char* via) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_records.push_back({id, title, body, options, via});
    s_payloads[id] = options.payload;
}

// This run, as a click's arguments name it: a click on a notification an
// earlier run posted carries another run's token.
const std::string& runToken() {
    static const std::string token = [] {
#ifdef _WIN32
        const unsigned long pid = GetCurrentProcessId();
#else
        const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
        const auto t = std::chrono::steady_clock::now().time_since_epoch().count();
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%lx-%llx", pid, static_cast<unsigned long long>(t));
        return std::string(buf);
    }();
    return token;
}

std::string percentEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string percentDecode(const std::string& s) {
    auto hexVal = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        const int hi = s[i] == '%' && i + 2 < s.size() ? hexVal(s[i + 1]) : -1;
        const int lo = hi >= 0 ? hexVal(s[i + 2]) : -1;
        if (lo >= 0) {
            out += static_cast<char>(hi * 16 + lo);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string s_exePath, s_appDir;

}  // namespace

void queueNotificationActivation(NotificationActivation a) {
    {
        std::lock_guard<std::mutex> lock(s_activationMutex);
        s_activations.push_back(std::move(a));
    }
    // A windowed loop asleep for want of work takes it now.
    util::wakeMainLoop();
}

std::string encodeNotificationArgs(uint32_t id, const std::string& action, const std::string& payload) {
    return "bro1;" + runToken() + ";" + std::to_string(id) + ";" + percentEncode(action) + ";" + payload;
}

bool decodeNotificationArgs(const std::string& args, NotificationActivation& out) {
    // bro1;<run>;<id>;<action>;<payload...>
    if (args.compare(0, 5, "bro1;") != 0) return false;
    size_t p = 5;
    auto field = [&](std::string& f) {
        const size_t e = args.find(';', p);
        if (e == std::string::npos) return false;
        f = args.substr(p, e - p);
        p = e + 1;
        return true;
    };
    std::string run, id, action;
    if (!field(run) || !field(id) || !field(action)) return false;
    out.earlierRun = run != runToken();
    out.id = out.earlierRun ? 0 : static_cast<uint32_t>(std::strtoul(id.c_str(), nullptr, 10));
    out.action = percentDecode(action);
    out.payload = args.substr(p);
    out.close = false;
    return true;
}

void noteLaunchNotification(const std::string& args) {
    NotificationActivation a;
    if (!decodeNotificationArgs(args, a)) return;
    // Posted by the run before this one, whatever the token says (a
    // launch's run is never the one that posted).
    a.earlierRun = true;
    a.id = 0;
    queueNotificationActivation(std::move(a));
}

bool simulateNotificationActivation(uint32_t id, const std::string& action, bool close) {
    NotificationActivation a;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto it = s_payloads.find(id);
        if (it == s_payloads.end()) return false;
        a.payload = it->second;
    }
    a.id = id;
    a.action = close ? std::string() : action;
    a.close = close;
    queueNotificationActivation(std::move(a));
    return true;
}

namespace {

#ifdef _WIN32
constexpr UINT kNotificationTrayId = 0xBE;
HWND g_trayOwner = nullptr;

bool showBalloon(const Window* window, const std::string& title, const std::string& body,
                 const NotificationOptions& options) {
    HWND hwnd = hwndOf(window);
    if (!hwnd) return false;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kNotificationTrayId;
    if (!g_trayOwner) {
        nid.uFlags = NIF_ICON | NIF_TIP;
        nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
        if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
        copyWide(nid.szTip, ARRAYSIZE(nid.szTip), utf8ToWide(options.appName.empty() ? "bro" : options.appName));
        if (Shell_NotifyIconW(NIM_ADD, &nid)) g_trayOwner = hwnd;
    }
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME | (options.silent ? NIIF_NOSOUND : 0);
    copyWide(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), utf8ToWide(title.empty() ? "bro" : title));
    copyWide(nid.szInfo, ARRAYSIZE(nid.szInfo), utf8ToWide(body.empty() ? " " : body));
    return Shell_NotifyIconW(NIM_MODIFY, &nid) != FALSE;
}
#else

// Runs `argv` without a shell (nothing in a title can be read as a command)
// and reaps it off this thread.
bool spawnQuiet(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return false;
    std::thread([pid] {
        int status = 0;
        waitpid(pid, &status, 0);
    }).detach();
    return true;
}

#if defined(__APPLE__)
// An AppleScript string literal.
std::string appleScriptString(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}
#else

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
#define DBUS_TYPE_VARIANT ((int)'v')
#define DBUS_TYPE_DICT_ENTRY ((int)'e')
#define DBUS_TYPE_BOOLEAN ((int)'b')

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
typedef DBusConnection* (*Fn_dbus_bus_get_private)(DBusBusType, DBusError*);
typedef void (*Fn_dbus_connection_set_exit_on_disconnect)(DBusConnection*, int);
typedef void (*Fn_dbus_bus_add_match)(DBusConnection*, const char*, DBusError*);
typedef int (*Fn_dbus_connection_read_write)(DBusConnection*, int);
typedef DBusMessage* (*Fn_dbus_connection_pop_message)(DBusConnection*);
typedef int (*Fn_dbus_message_is_signal)(DBusMessage*, const char*, const char*);
typedef int (*Fn_dbus_message_iter_next)(DBusMessageIter*);
typedef int (*Fn_dbus_message_iter_get_arg_type)(DBusMessageIter*);

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
    Fn_dbus_bus_get_private bus_get_private = nullptr;
    Fn_dbus_connection_set_exit_on_disconnect set_exit_on_disconnect = nullptr;
    Fn_dbus_bus_add_match bus_add_match = nullptr;
    Fn_dbus_connection_read_write read_write = nullptr;
    Fn_dbus_connection_pop_message pop_message = nullptr;
    Fn_dbus_message_is_signal message_is_signal = nullptr;
    Fn_dbus_message_iter_next message_iter_next = nullptr;
    Fn_dbus_message_iter_get_arg_type message_iter_get_arg_type = nullptr;

    bool load() {
        if (handle) return true;
        handle = dlopen("libdbus-1.so.3", RTLD_LAZY);
        if (!handle) handle = dlopen("libdbus-1.so", RTLD_LAZY);
        if (!handle) return false;

        #define LOAD_SYM(member, sym) member = (Fn_##sym)dlsym(handle, #sym); if (!member) { handle = nullptr; return false; }
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
        LOAD_SYM(bus_get_private, dbus_bus_get_private);
        LOAD_SYM(set_exit_on_disconnect, dbus_connection_set_exit_on_disconnect);
        LOAD_SYM(bus_add_match, dbus_bus_add_match);
        LOAD_SYM(read_write, dbus_connection_read_write);
        LOAD_SYM(pop_message, dbus_connection_pop_message);
        LOAD_SYM(message_is_signal, dbus_message_is_signal);
        LOAD_SYM(message_iter_next, dbus_message_iter_next);
        LOAD_SYM(message_iter_get_arg_type, dbus_message_iter_get_arg_type);
        #undef LOAD_SYM
        return true;
    }
};

DBusApi g_dbus;
bool g_dbusTried = false;

// One string-valued hint: {key: <string>}.
void appendStringHint(DBusMessageIter* hints, const char* key, const char* value) {
    DBusMessageIter entry, variant;
    g_dbus.message_iter_open_container(hints, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    g_dbus.message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    g_dbus.message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
    g_dbus.message_iter_append_basic(&variant, DBUS_TYPE_STRING, &value);
    g_dbus.message_iter_close_container(&entry, &variant);
    g_dbus.message_iter_close_container(hints, &entry);
}

void appendBoolHint(DBusMessageIter* hints, const char* key, bool value) {
    DBusMessageIter entry, variant;
    const int v = value ? 1 : 0;
    g_dbus.message_iter_open_container(hints, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    g_dbus.message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    g_dbus.message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &variant);
    g_dbus.message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &v);
    g_dbus.message_iter_close_container(&entry, &variant);
    g_dbus.message_iter_close_container(hints, &entry);
}

struct FakeDBusError {
    const char* name;
    const char* message;
    unsigned int dummy1 : 1;
    unsigned int dummy2 : 1;
    unsigned int dummy3 : 1;
    unsigned int dummy4 : 1;
    unsigned int dummy5 : 1;
    void* padding1;
};

// bro's own connection to the session bus, kept for the life of the process:
// the server sends a notification's ActionInvoked and NotificationClosed to
// the connection that posted it, and this one is read only here (a shared
// connection's messages belong to whoever else uses it).
std::mutex g_dbusMutex;
DBusConnection* g_dbusConn = nullptr;
// The D-Bus ids this run posted, and their payloads (the signals carry the
// id; other apps' notifications are ignored).
std::map<uint32_t, std::string> g_dbusPosted;

DBusConnection* dbusConnection() {
    if (!g_dbusTried) {
        g_dbusTried = true;
        g_dbus.load();
    }
    if (!g_dbus.handle) return nullptr;
    if (g_dbusConn) return g_dbusConn;
    FakeDBusError err;
    g_dbus.error_init((DBusError*)&err);
    g_dbusConn = g_dbus.bus_get_private(DBUS_BUS_SESSION, (DBusError*)&err);
    g_dbus.error_free((DBusError*)&err);
    if (!g_dbusConn) return nullptr;
    g_dbus.set_exit_on_disconnect(g_dbusConn, 0);
    for (const char* member : {"ActionInvoked", "NotificationClosed"}) {
        const std::string rule = std::string("type='signal',interface='org.freedesktop.Notifications',member='") +
                                 member + "'";
        g_dbus.error_init((DBusError*)&err);
        g_dbus.bus_add_match(g_dbusConn, rule.c_str(), (DBusError*)&err);
        g_dbus.error_free((DBusError*)&err);
    }
    return g_dbusConn;
}

// ActionInvoked(u id, s action_key) and NotificationClosed(u id, u reason)
// for this run's notifications, as activations. Reason 2 is "dismissed by
// the user"; expiry (1) and CloseNotification (3) are not a user's close.
void pollDbusSignals() {
    std::lock_guard<std::mutex> lock(g_dbusMutex);
    if (!g_dbusConn || g_dbusPosted.empty()) return;
    g_dbus.read_write(g_dbusConn, 0);
    while (DBusMessage* msg = g_dbus.pop_message(g_dbusConn)) {
        const bool invoked = g_dbus.message_is_signal(msg, "org.freedesktop.Notifications", "ActionInvoked");
        const bool closed = !invoked &&
                            g_dbus.message_is_signal(msg, "org.freedesktop.Notifications", "NotificationClosed");
        DBusMessageIter it;
        if ((invoked || closed) && g_dbus.message_iter_init(msg, &it) &&
            g_dbus.message_iter_get_arg_type(&it) == DBUS_TYPE_UINT32) {
            uint32_t id = 0;
            g_dbus.message_iter_get_basic(&it, &id);
            auto posted = g_dbusPosted.find(id);
            if (posted != g_dbusPosted.end() && g_dbus.message_iter_next(&it)) {
                NotificationActivation a;
                a.id = id;
                a.payload = posted->second;
                if (invoked && g_dbus.message_iter_get_arg_type(&it) == DBUS_TYPE_STRING) {
                    const char* key = nullptr;
                    g_dbus.message_iter_get_basic(&it, &key);
                    const std::string k = key ? key : "";
                    a.action = k == "default" ? std::string() : k;
                    queueNotificationActivation(std::move(a));
                } else if (closed && g_dbus.message_iter_get_arg_type(&it) == DBUS_TYPE_UINT32) {
                    uint32_t reason = 0;
                    g_dbus.message_iter_get_basic(&it, &reason);
                    if (reason == 2) {
                        a.close = true;
                        queueNotificationActivation(std::move(a));
                    }
                    g_dbusPosted.erase(posted);
                }
            }
        }
        g_dbus.message_unref(msg);
    }
}

uint32_t sendDbusNotification(const std::string& title, const std::string& body, const NotificationOptions& options) {
    std::lock_guard<std::mutex> lock(g_dbusMutex);
    DBusConnection* conn = dbusConnection();
    if (!conn) return 0;
    FakeDBusError err;
    g_dbus.error_init((DBusError*)&err);

    DBusMessage* msg = g_dbus.message_new_method_call(
        "org.freedesktop.Notifications",
        "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications",
        "Notify"
    );
    if (!msg) return 0;

    DBusMessageIter args;
    g_dbus.message_iter_init_append(msg, &args);

    // The app's own name, so the server groups and labels it as the app
    // rather than as "bro".
    const std::string appNameText = options.appName.empty() ? std::string("bro") : options.appName;
    const char* appName = appNameText.c_str();
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

    // actions: string array (as), key then label: "default" is a click on
    // the notification itself, then the page's buttons.
    DBusMessageIter actionsIter;
    g_dbus.message_iter_open_container(&args, DBUS_TYPE_ARRAY, "s", &actionsIter);
    {
        std::vector<std::string> pairs{"default", "Open"};
        for (const auto& a : options.actions) {
            if (a.id.empty() || a.id == "default") continue;
            pairs.push_back(a.id);
            pairs.push_back(a.title);
        }
        for (const auto& s : pairs) {
            const char* c = s.c_str();
            g_dbus.message_iter_append_basic(&actionsIter, DBUS_TYPE_STRING, &c);
        }
    }
    g_dbus.message_iter_close_container(&args, &actionsIter);

    // hints: dictionary (a{sv}). desktop-entry is the app's id: the desktop
    // entry `bro --install` writes is <id>.desktop, which is how a server
    // (helm's included) finds the app's icon and settings.
    DBusMessageIter hintsIter;
    g_dbus.message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &hintsIter);
    if (!options.appId.empty()) appendStringHint(&hintsIter, "desktop-entry", options.appId.c_str());
    if (options.silent) appendBoolHint(&hintsIter, "suppress-sound", true);
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

    if (notificationId) g_dbusPosted[notificationId] = options.payload;
    return notificationId;
}

#endif  // !__APPLE__
#endif  // !_WIN32

} // namespace

uint32_t showNotification(
    const Window* window,
    const std::string& title,
    const std::string& body,
    const NotificationOptions& options
) {
    const uint32_t generatedId = s_nextId.fetch_add(1, std::memory_order_relaxed);

    if (isHeadless()) {
        record(generatedId, title, body, options, "headless");
        return generatedId;
    }

#ifdef _WIN32
    // A toast needs an AppUserModelID: the app's id. An anonymous app (no
    // id) gets the tray balloon.
    if (!options.appId.empty() &&
        showWindowsToast(options.appId, options.appName, title, body, options, options.replacesId ? options.replacesId : generatedId)) {
        record(generatedId, title, body, options, "toast");
        return options.replacesId ? options.replacesId : generatedId;
    }
    const bool shown = showBalloon(window, title, body, options);
    record(generatedId, title, body, options, shown ? "balloon" : "");
    return generatedId;
#elif defined(__APPLE__)
    (void)window;
    if (showMacUserNotification(title, body, options, options.replacesId ? options.replacesId : generatedId)) {
        record(generatedId, title, body, options, "usernotifications");
        return options.replacesId ? options.replacesId : generatedId;
    }
    std::string script = "display notification " + appleScriptString(body) + " with title " +
                         appleScriptString(title.empty() ? (options.appName.empty() ? "bro" : options.appName) : title);
    if (!options.silent) script += " sound name \"default\"";
    const bool shown = spawnQuiet({"osascript", "-e", script});
    record(generatedId, title, body, options, shown ? "osascript" : "");
    return generatedId;
#else
    (void)window;
    uint32_t dbusId = sendDbusNotification(title, body, options);
    if (dbusId > 0) {
        record(dbusId, title, body, options, "dbus");
        return dbusId;
    }
    std::vector<std::string> args{"notify-send"};
    if (!options.appName.empty()) args.push_back("--app-name=" + options.appName);
    if (!options.icon.empty()) args.push_back("--icon=" + options.icon);
    args.push_back("--");
    args.push_back(title);
    args.push_back(body);
    const bool shown = spawnQuiet(args);
    record(generatedId, title, body, options, shown ? "notify-send" : "");
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

std::vector<NotificationActivation> takeNotificationActivations() {
#if !defined(_WIN32) && !defined(__APPLE__)
    pollDbusSignals();
#endif
    std::lock_guard<std::mutex> lock(s_activationMutex);
    std::vector<NotificationActivation> out;
    out.swap(s_activations);
    return out;
}

namespace {
std::atomic<NotificationLaunchHandler> s_launchHandler{nullptr};
bool s_bareLaunch = false;

bool sameAppDir(const std::string& a, const std::string& b) {
    if (a == b) return true;
    auto canonical = [](const std::string& s, std::error_code& ec) {
        return std::filesystem::weakly_canonical(std::filesystem::path(std::u8string(s.begin(), s.end())), ec);
    };
    std::error_code ec;
    const auto ca = canonical(a, ec);
    if (ec) return false;
    const auto cb = canonical(b, ec);
    return !ec && ca == cb;
}
}  // namespace

void setNotificationLaunchHandler(NotificationLaunchHandler handler) { s_launchHandler.store(handler); }
bool notificationBareLaunch() { return s_bareLaunch; }

NotificationRoute routeNotificationResponse(const std::string& postedAppDir, const std::string& args,
                                            const std::string& action, bool close) {
    NotificationActivation a;
    if (!decodeNotificationArgs(args, a)) return NotificationRoute::Dropped;
    if (postedAppDir.empty() || s_appDir.empty() || sameAppDir(postedAppDir, s_appDir)) {
        a.close = close;
        a.action = close ? std::string() : action;
        queueNotificationActivation(std::move(a));
        return NotificationRoute::Queued;
    }
    // Another app's: a click starts it, carrying the click as a launch does
    // (Windows' toast activator starts the posting app the same way); the
    // user closing another app's notification concerns nobody running.
    if (close) return NotificationRoute::Dropped;
    const NotificationLaunchHandler launch = s_launchHandler.load();
    if (!launch || !launch(postedAppDir, encodeNotificationArgs(0, action, a.payload)))
        return NotificationRoute::Dropped;
    return NotificationRoute::Launched;
}

void initNotificationActivation(const std::string& appId, const std::string& exePath, const std::string& appDir,
                                bool comLaunch, bool bareLaunch) {
    s_exePath = exePath;
    s_appDir = appDir;
    s_bareLaunch = bareLaunch;
    if (isHeadless()) return;
#ifdef _WIN32
    if (!appId.empty()) initWindowsToastActivation(appId, comLaunch, /*onlyIfRegistered=*/!comLaunch);
#elif defined(__APPLE__)
    (void)appId;
    (void)comLaunch;
    initMacNotificationDelegate();
#else
    (void)appId;
    (void)comLaunch;
#endif
}

const std::string& notificationLaunchExe() { return s_exePath; }
const std::string& notificationLaunchAppDir() { return s_appDir; }

} // namespace bro::platform::desktop
