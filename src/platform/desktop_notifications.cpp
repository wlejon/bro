#include "platform/desktop_notifications.h"
#include "platform/desktop_platform.h"

#include <atomic>
#include <cstdlib>
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

void record(uint32_t id, const std::string& title, const std::string& body, const NotificationOptions& options,
            const char* via) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_records.push_back({id, title, body, options, via});
}

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

    // actions: string array (as)
    DBusMessageIter actionsIter;
    g_dbus.message_iter_open_container(&args, DBUS_TYPE_ARRAY, "s", &actionsIter);
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

} // namespace bro::platform::desktop
