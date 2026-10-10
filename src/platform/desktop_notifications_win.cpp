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
#include <wrl/wrappers/corewrappers.h>

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
    auto setString = [h](const wchar_t* name, const std::wstring& value) {
        RegSetValueExW(h, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    };
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
    return SUCCEEDED(notifier->Show(toast.Get()));
}

}  // namespace

std::string windowsToastXml(const std::string& title, const std::string& body, const NotificationOptions& options) {
    std::string out = "<toast";
    if (options.timeoutMs == 0) out += " duration=\"long\"";
    out += "><visual><binding template=\"ToastGeneric\">";
    out += "<text>" + xmlEscape(title) + "</text>";
    if (!body.empty()) out += "<text>" + xmlEscape(body) + "</text>";
    const std::string image = toastImageUri(options.icon);
    if (!image.empty()) out += "<image placement=\"appLogoOverride\" src=\"" + xmlEscape(image) + "\"/>";
    out += "</binding></visual>";
    if (options.silent) out += "<audio silent=\"true\"/>";
    out += "</toast>";
    return out;
}

bool showWindowsToast(const std::string& aumid, const std::string& appName, const std::string& title,
                      const std::string& body, const NotificationOptions& options, uint32_t id) {
    if (aumid.empty()) return false;
    if (!registerAumid(aumid, appName, options.icon)) return false;
    const std::wstring waumid = utf8ToWide(aumid);
    const std::wstring wxml = utf8ToWide(windowsToastXml(title, body, options));
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
