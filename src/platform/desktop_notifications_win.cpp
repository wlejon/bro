// Windows toasts for bro.window.notify / `Notification` (desktop_notifications.h).
//
// A toast is shown under an AppUserModelID: the app's id, which is also what
// SetCurrentProcessExplicitAppUserModelID gives its windows (sdl_window.cpp),
// so the toast and the taskbar button are one app. An unpackaged process can
// only post under an AUMID Windows knows; registering one for the current
// user is a key under HKCU\Software\Classes\AppUserModelId\<id> naming its
// display name and icon (what the Windows App SDK writes for unpackaged
// apps). bro writes it the first time the app notifies.
//
// The WinRT calls run on a thread of their own in the multithreaded
// apartment, so the caller's COM apartment (SDL's, OLE drag and drop's) is
// never changed or relied on.

#include "platform/desktop_notifications.h"
#include "platform/desktop_platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <roapi.h>
#include <windows.data.xml.dom.h>
#include <windows.ui.notifications.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <wrl/implements.h>
#include <wrl/module.h>
#include <wrl/wrappers/corewrappers.h>
#include <NotificationActivationCallback.h>

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#pragma comment(lib, "runtimeobject.lib")

namespace bro::platform::desktop {

namespace {

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;
namespace notif = ABI::Windows::UI::Notifications;
namespace xml = ABI::Windows::Data::Xml::Dom;

std::string xmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:
                // XML 1.0 has no place for the other C0 controls.
                if (static_cast<unsigned char>(c) < 0x20 && c != '\n' && c != '\t' && c != '\r') continue;
                out += c;
        }
    }
    return out;
}

// A toast shows PNG, JPEG and GIF images from a file:/// URI.
std::string toastImageUri(const std::string& path) {
    if (path.empty()) return {};
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p(utf8ToWide(path));
    if (!fs::is_regular_file(p, ec)) return {};
    std::wstring ext = p.extension().wstring();
    for (auto& ch : ext) ch = static_cast<wchar_t>(towlower(ch));
    if (ext != L".png" && ext != L".jpg" && ext != L".jpeg" && ext != L".gif") return {};
    std::wstring abs = fs::absolute(p, ec).wstring();
    std::string uri = "file:///" + wideToUtf8(abs);
    for (auto& ch : uri)
        if (ch == '\\') ch = '/';
    return uri;
}

// HKCU\Software\Classes\AppUserModelId\<aumid>: DisplayName, IconUri. Once a
// process per id; an existing DisplayName is left as the user (or an
// installer) set it.
void setRegString(HKEY h, const wchar_t* name, const std::wstring& value) {
    RegSetValueExW(h, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

// The toast activator: a COM class the AUMID names as its CustomActivator,
// served by the running app (registerActivatorClass), and otherwise by the
// command under HKCU\Software\Classes\CLSID\{clsid}\LocalServer32, which
// starts the app: `"<bro>" --notification-activated "<app dir>"` (COM adds
// -Embedding). Windows then calls Activate with the clicked toast's (or
// button's) arguments.
bool registerActivator(const std::string& aumid, HKEY aumidKey) {
    const std::string& exe = notificationLaunchExe();
    const std::string& appDir = notificationLaunchAppDir();
    if (exe.empty() || appDir.empty()) return false;
    const std::wstring clsid = utf8ToWide(windowsToastActivatorClsid(aumid));
    const std::wstring key = L"Software\\Classes\\CLSID\\" + clsid + L"\\LocalServer32";
    HKEY h = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr) !=
        ERROR_SUCCESS)
        return false;
    setRegString(h, nullptr,
                 L"\"" + utf8ToWide(exe) + L"\" --notification-activated \"" + utf8ToWide(appDir) + L"\"");
    RegCloseKey(h);
    setRegString(aumidKey, L"CustomActivator", clsid);
    return true;
}

bool registerAumid(const std::string& aumid, const std::string& appName, const std::string& icon) {
    static std::mutex mu;
    static std::set<std::string> done;
    std::lock_guard<std::mutex> lock(mu);
    if (done.count(aumid)) return true;
    const std::wstring key = L"Software\\Classes\\AppUserModelId\\" + utf8ToWide(aumid);
    HKEY h = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &h,
                        nullptr) != ERROR_SUCCESS)
        return false;
    auto setString = [h](const wchar_t* name, const std::wstring& value) { setRegString(h, name, value); };
    registerActivator(aumid, h);
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(h, L"DisplayName", nullptr, &type, nullptr, &size) != ERROR_SUCCESS)
        setString(L"DisplayName", utf8ToWide(appName.empty() ? aumid : appName));
    if (!icon.empty()) {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path p(utf8ToWide(icon));
        std::wstring ext = p.extension().wstring();
        for (auto& ch : ext) ch = static_cast<wchar_t>(towlower(ch));
        if ((ext == L".png" || ext == L".ico" || ext == L".jpg") && fs::is_regular_file(p, ec))
            setString(L"IconUri", fs::absolute(p, ec).wstring());
    }
    RegCloseKey(h);
    done.insert(aumid);
    return true;
}

bool showOnThisThread(const std::wstring& aumid, const std::wstring& xmlText, uint32_t id) {
    ComPtr<IInspectable> inspectable;
    if (FAILED(RoActivateInstance(HStringReference(RuntimeClass_Windows_Data_Xml_Dom_XmlDocument).Get(),
                                  &inspectable)))
        return false;
    ComPtr<xml::IXmlDocument> doc;
    ComPtr<xml::IXmlDocumentIO> io;
    if (FAILED(inspectable.As(&doc)) || FAILED(doc.As(&io))) return false;
    if (FAILED(io->LoadXml(HStringReference(xmlText.c_str()).Get()))) return false;

    ComPtr<notif::IToastNotificationManagerStatics> manager;
    if (FAILED(RoGetActivationFactory(
            HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager).Get(),
            IID_PPV_ARGS(&manager))))
        return false;
    ComPtr<notif::IToastNotifier> notifier;
    if (FAILED(manager->CreateToastNotifierWithId(HStringReference(aumid.c_str()).Get(), &notifier)))
        return false;
    // Toasts for an AUMID the system will not show (disabled for the app,
    // or not registered) are refused here rather than dropped silently.
    notif::NotificationSetting setting = notif::NotificationSetting_Enabled;
    if (SUCCEEDED(notifier->get_Setting(&setting)) && setting != notif::NotificationSetting_Enabled) return false;

    ComPtr<notif::IToastNotificationFactory> factory;
    if (FAILED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotification).Get(),
                                      IID_PPV_ARGS(&factory))))
        return false;
    ComPtr<notif::IToastNotification> toast;
    if (FAILED(factory->CreateToastNotification(doc.Get(), &toast))) return false;
    // The id as the toast's tag: a later notification with replacesId of
    // this one takes its place in the Action Center.
    ComPtr<notif::IToastNotification2> toast2;
    if (SUCCEEDED(toast.As(&toast2))) {
        const std::wstring tag = std::to_wstring(id);
        toast2->put_Tag(HStringReference(tag.c_str()).Get());
        toast2->put_Group(HStringReference(L"bro").Get());
    }
    // The user closing it is the page's `close`. (Clicks come through the
    // activator, ToastActivator below, which also serves a later run.)
    using DismissedHandler =
        ABI::Windows::Foundation::ITypedEventHandler<notif::ToastNotification*, notif::ToastDismissedEventArgs*>;
    EventRegistrationToken token{};
    toast->add_Dismissed(Microsoft::WRL::Callback<DismissedHandler>(
                             [id](notif::IToastNotification*, notif::IToastDismissedEventArgs* args) -> HRESULT {
                                 notif::ToastDismissalReason reason = notif::ToastDismissalReason_TimedOut;
                                 if (args && SUCCEEDED(args->get_Reason(&reason)) &&
                                     reason == notif::ToastDismissalReason_UserCanceled) {
                                     NotificationActivation a;
                                     a.id = id;
                                     a.close = true;
                                     queueNotificationActivation(std::move(a));
                                 }
                                 return S_OK;
                             })
                             .Get(),
                         &token);
    if (FAILED(notifier->Show(toast.Get()))) return false;
    // Kept, with its handler, for the life of the process (the multithreaded
    // apartment it lives in is held open by keepMta).
    static std::mutex heldMutex;
    static std::vector<ComPtr<notif::IToastNotification>> held;
    std::lock_guard<std::mutex> lock(heldMutex);
    held.push_back(toast);
    return true;
}

// The process's multithreaded apartment stays up from the first toast on, so
// the toasts kept above and the activator's registration outlive the worker
// threads that made them.
void keepMta() {
    static std::once_flag once;
    std::call_once(once, [] {
        CO_MTA_USAGE_COOKIE cookie{};
        CoIncrementMTAUsage(&cookie);
    });
}

// INotificationActivationCallback: Windows calls Activate with the arguments
// of the toast or button clicked (windowsToastXml's launch / arguments).
class ToastActivator
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          INotificationActivationCallback> {
public:
    HRESULT STDMETHODCALLTYPE Activate(LPCWSTR, LPCWSTR invokedArgs, const NOTIFICATION_USER_INPUT_DATA*,
                                       ULONG) override {
        NotificationActivation a;
        if (invokedArgs && decodeNotificationArgs(wideToUtf8(invokedArgs), a)) {
            queueNotificationActivation(std::move(a));
        }
        return S_OK;
    }
};

// GUID text "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" to a GUID.
bool parseGuid(const std::string& s, GUID& out) {
    return SUCCEEDED(CLSIDFromString(utf8ToWide(s).c_str(), &out));
}

}  // namespace

std::string windowsToastActivatorClsid(const std::string& aumid) {
    // Two FNV-1a hashes of the AUMID (different offsets) as a version-5-shaped
    // GUID: the same id names the same class in every run and every build.
    auto fnv = [&aumid](uint64_t h) {
        for (unsigned char c : "bro-toast-activator:" + aumid) {
            h ^= c;
            h *= 1099511628211ull;
        }
        return h;
    };
    const uint64_t a = fnv(14695981039346656037ull), b = fnv(0x84222325cbf29ce4ull);
    uint8_t bytes[16];
    for (int i = 0; i < 8; ++i) {
        bytes[i] = static_cast<uint8_t>(a >> (8 * i));
        bytes[8 + i] = static_cast<uint8_t>(b >> (8 * i));
    }
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x50);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "{%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8],
                  bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return buf;
}

void initWindowsToastActivation(const std::string& aumid, bool comLaunch, bool onlyIfRegistered) {
    static std::mutex mu;
    static std::set<std::string> served;
    std::lock_guard<std::mutex> lock(mu);
    if (aumid.empty() || served.count(aumid)) return;
    const std::string clsidText = windowsToastActivatorClsid(aumid);
    if (onlyIfRegistered) {
        // An app that never notified has no toast to click: no class to serve.
        const std::wstring key = L"Software\\Classes\\AppUserModelId\\" + utf8ToWide(aumid);
        wchar_t value[64] = {};
        DWORD size = sizeof(value);
        if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"CustomActivator", RRF_RT_REG_SZ, nullptr, value,
                         &size) != ERROR_SUCCESS ||
            wideToUtf8(value) != clsidText)
            return;
    }
    GUID clsid{};
    if (!parseGuid(clsidText, clsid)) return;
    served.insert(aumid);
    keepMta();
    // Registered from a thread in the multithreaded apartment, which keepMta
    // keeps alive after it returns: Activate is then called on COM's threads.
    std::thread([clsid, comLaunch] {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
        auto factory = Microsoft::WRL::Make<Microsoft::WRL::SimpleClassFactory<ToastActivator>>();
        DWORD cookie = 0;
        // Never revoked or uninitialized: the class is served until the
        // process exits.
        const HRESULT hr = CoRegisterClassObject(clsid, factory.Get(), CLSCTX_LOCAL_SERVER, REGCLS_MULTIPLEUSE,
                                                 &cookie);
        if (FAILED(hr)) {
            std::fprintf(stderr, "notifications: could not serve the toast activator (0x%08lx)%s\n",
                         static_cast<unsigned long>(hr), comLaunch ? "; the click that started this run is lost" : "");
        }
    }).join();
}

std::string windowsToastXml(const std::string& title, const std::string& body, const NotificationOptions& options,
                            uint32_t id) {
    std::string out = "<toast";
    if (options.timeoutMs == 0) out += " duration=\"long\"";
    // A click on the toast (or a button) activates the app's toast activator
    // with these arguments (initWindowsToastActivation).
    out += " launch=\"" + xmlEscape(encodeNotificationArgs(id, "", options.payload)) + "\"";
    out += " activationType=\"foreground\"";
    out += "><visual><binding template=\"ToastGeneric\">";
    out += "<text>" + xmlEscape(title) + "</text>";
    if (!body.empty()) out += "<text>" + xmlEscape(body) + "</text>";
    const std::string image = toastImageUri(options.icon);
    if (!image.empty()) out += "<image placement=\"appLogoOverride\" src=\"" + xmlEscape(image) + "\"/>";
    out += "</binding></visual>";
    if (!options.actions.empty()) {
        out += "<actions>";
        for (size_t i = 0; i < options.actions.size() && i < 5; ++i) {
            const auto& a = options.actions[i];
            out += "<action content=\"" + xmlEscape(a.title) + "\" arguments=\"" +
                   xmlEscape(encodeNotificationArgs(id, a.id, options.payload)) +
                   "\" activationType=\"foreground\"/>";
        }
        out += "</actions>";
    }
    if (options.silent) out += "<audio silent=\"true\"/>";
    out += "</toast>";
    return out;
}

bool showWindowsToast(const std::string& aumid, const std::string& appName, const std::string& title,
                      const std::string& body, const NotificationOptions& options, uint32_t id) {
    if (aumid.empty()) return false;
    if (!registerAumid(aumid, appName, options.icon)) return false;
    const std::wstring waumid = utf8ToWide(aumid);
    const std::wstring wxml = utf8ToWide(windowsToastXml(title, body, options, id));
    // Clicks on this app's toasts come to this run from now on.
    initWindowsToastActivation(aumid, false, /*onlyIfRegistered=*/false);
    keepMta();
    bool shown = false;
    std::thread worker([&] {
        const HRESULT init = RoInitialize(RO_INIT_MULTITHREADED);
        shown = showOnThisThread(waumid, wxml, id);
        if (SUCCEEDED(init)) RoUninitialize();
    });
    worker.join();
    return shown;
}

}  // namespace bro::platform::desktop
