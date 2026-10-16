// Windows half of an app's own executable (app_exe.h): the icon and version
// resources stamped into the copied stub, and the Start menu shortcut that
// carries the app's AppUserModelID.
//
// The resources go in with the system's own updater (BeginUpdateResourceW /
// UpdateResourceW / EndUpdateResourceW, kernel32), present on every Windows
// bro runs on: no resource compiler, no SDK, nothing to install. The stub is
// built with a manifest and no icon or version of its own, so these are the
// executable's only ones.

#ifdef _WIN32

#include "engine/app_exe.h"

#include "engine/app_runtime.h"
#include "util/log.h"

#include "broimage/alpha.h"
#include "broimage/decode.h"
#include "broimage/encode.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shlobj.h>
#include <shobjidl.h>

namespace bro::engine {

namespace fs = std::filesystem;

namespace {

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

fs::path u8path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

// ---- icon -------------------------------------------------------------------

constexpr int kIconSizes[] = {16, 20, 24, 32, 40, 48, 64, 96, 128, 256};

// One square straight-alpha RGBA image of each size, the art centred.
std::vector<std::vector<uint8_t>> iconImages(const std::string& path) {
    std::vector<std::vector<uint8_t>> out;
    std::ifstream f(u8path(path), std::ios::binary);
    if (!f) return out;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return out;
    // A bitmap is decoded once and area-filtered down to each size; an SVG is
    // rasterized at each size, so a 16 px icon is drawn at 16 px.
    broimage::Image bitmap;
    const bool isBitmap = broimage::decode_memory(bytes.data(), bytes.size(), bitmap) && bitmap.width > 0;
    for (int size : kIconSizes) {
        std::vector<uint8_t> square(size_t(size) * size * 4, 0);
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
        if (isBitmap) {
            w = bitmap.width;
            h = bitmap.height;
            rgba = bitmap.pixels;
        } else if (!loadIconPixels(path, size, w, h, rgba)) {
            return {};
        }
        if (w <= 0 || h <= 0 || rgba.size() < size_t(w) * h * 4) return {};
        broimage::letterbox_rgba8_alpha(rgba.data(), w, h, square.data(), size, size, 0, 0, 0, 0,
                                        broimage::Filter::Area);
        out.push_back(std::move(square));
    }
    return out;
}

// RT_ICON data: a 32-bit DIB, height doubled for the (unused) AND mask, rows
// bottom-up in BGRA, then the mask rows, 1 bpp padded to 32 bits.
std::vector<uint8_t> iconDib(const std::vector<uint8_t>& rgba, int size) {
    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(bih);
    bih.biWidth = size;
    bih.biHeight = size * 2;
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    const size_t maskRow = size_t(((size + 31) / 32) * 4);
    bih.biSizeImage = DWORD(size_t(size) * size * 4 + maskRow * size);
    std::vector<uint8_t> out(sizeof(bih) + bih.biSizeImage, 0);
    std::memcpy(out.data(), &bih, sizeof(bih));
    uint8_t* px = out.data() + sizeof(bih);
    for (int y = 0; y < size; ++y) {
        const uint8_t* src = rgba.data() + size_t(size - 1 - y) * size * 4;
        uint8_t* dst = px + size_t(y) * size * 4;
        for (int x = 0; x < size; ++x) {
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = src[x * 4 + 3];
        }
    }
    return out;  // the mask stays zero: alpha decides
}

#pragma pack(push, 2)
struct GroupIconDir {
    WORD reserved;
    WORD type;
    WORD count;
};
struct GroupIconEntry {
    BYTE width;
    BYTE height;
    BYTE colorCount;
    BYTE reserved;
    WORD planes;
    WORD bitCount;
    DWORD bytesInRes;
    WORD id;
};
#pragma pack(pop)

// ---- version ------------------------------------------------------------------

// VS_VERSIONINFO is a tree of nodes, each { wLength, wValueLength, wType,
// szKey, pad to 4, value, pad to 4, children }. Built bottom-up.
struct VerNode {
    std::wstring key;
    std::vector<uint8_t> value;   // raw bytes
    bool text = false;            // wType 1: value is a NUL-terminated string
    std::vector<VerNode> children;
};

void pad4(std::vector<uint8_t>& b) {
    while (b.size() % 4) b.push_back(0);
}

std::vector<uint8_t> encode(const VerNode& n) {
    std::vector<uint8_t> b(6, 0);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(n.key.c_str());
    b.insert(b.end(), k, k + (n.key.size() + 1) * sizeof(wchar_t));
    pad4(b);
    b.insert(b.end(), n.value.begin(), n.value.end());
    pad4(b);
    for (const VerNode& c : n.children) {
        std::vector<uint8_t> cb = encode(c);
        b.insert(b.end(), cb.begin(), cb.end());
        pad4(b);
    }
    const WORD len = WORD(b.size());
    // A string's value length is in characters (with the NUL), a binary's in bytes.
    const WORD valueLen = WORD(n.text ? n.value.size() / sizeof(wchar_t) : n.value.size());
    const WORD type = n.text ? 1 : 0;
    std::memcpy(b.data(), &len, 2);
    std::memcpy(b.data() + 2, &valueLen, 2);
    std::memcpy(b.data() + 4, &type, 2);
    return b;
}

VerNode textNode(const std::wstring& key, const std::wstring& value) {
    VerNode n;
    n.key = key;
    n.text = true;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(value.c_str());
    n.value.assign(p, p + (value.size() + 1) * sizeof(wchar_t));
    return n;
}

void versionParts(const std::string& v, WORD parts[4]) {
    for (int i = 0; i < 4; ++i) parts[i] = 0;
    int i = 0;
    const char* p = v.c_str();
    while (*p && i < 4) {
        if (*p < '0' || *p > '9') break;
        parts[i++] = WORD(std::strtoul(p, const_cast<char**>(&p), 10));
        if (*p != '.') break;
        ++p;
    }
}

std::vector<uint8_t> versionResource(const AppDescriptor& m, const std::string& id, const std::string& exeName) {
    WORD v[4];
    versionParts(m.version, v);
    VS_FIXEDFILEINFO ffi{};
    ffi.dwSignature = 0xFEEF04BD;
    ffi.dwStrucVersion = 0x00010000;
    ffi.dwFileVersionMS = (DWORD(v[0]) << 16) | v[1];
    ffi.dwFileVersionLS = (DWORD(v[2]) << 16) | v[3];
    ffi.dwProductVersionMS = ffi.dwFileVersionMS;
    ffi.dwProductVersionLS = ffi.dwFileVersionLS;
    ffi.dwFileFlagsMask = VS_FFI_FILEFLAGSMASK;
    ffi.dwFileOS = VOS_NT_WINDOWS32;
    ffi.dwFileType = VFT_APP;

    const std::wstring name = wide(m.name.empty() ? id : m.name);
    const std::wstring version = wide(m.version.empty() ? std::string("0") : m.version);
    VerNode table;
    table.key = L"040904B0";  // US English, Unicode
    table.children.push_back(textNode(L"FileDescription", name));  // what Task Manager shows
    table.children.push_back(textNode(L"ProductName", name));
    table.children.push_back(textNode(L"FileVersion", version));
    table.children.push_back(textNode(L"ProductVersion", version));
    table.children.push_back(textNode(L"InternalName", wide(id)));
    table.children.push_back(textNode(L"OriginalFilename", wide(exeName)));
    if (!m.description.empty()) table.children.push_back(textNode(L"Comments", wide(m.description)));
    VerNode strings;
    strings.key = L"StringFileInfo";
    strings.text = true;
    strings.children.push_back(std::move(table));

    VerNode translation;
    translation.key = L"Translation";
    const WORD lang[2] = {0x0409, 1200};
    const uint8_t* lp = reinterpret_cast<const uint8_t*>(lang);
    translation.value.assign(lp, lp + sizeof(lang));
    VerNode vars;
    vars.key = L"VarFileInfo";
    vars.text = true;
    vars.children.push_back(std::move(translation));

    VerNode root;
    root.key = L"VS_VERSION_INFO";
    const uint8_t* fp = reinterpret_cast<const uint8_t*>(&ffi);
    root.value.assign(fp, fp + sizeof(ffi));
    root.children.push_back(std::move(strings));
    root.children.push_back(std::move(vars));
    return encode(root);
}

std::string lastError(const char* what) { return std::string(what) + " failed (" + std::to_string(GetLastError()) + ")"; }

}  // namespace

bool stampWindowsResources(const std::string& exePath, const AppDescriptor& manifest, const std::string& id,
                           const std::string& iconPath, std::string& err) {
    const WORD lang = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);
    std::vector<std::vector<uint8_t>> icons;
    if (!iconPath.empty()) {
        icons = iconImages(iconPath);
        if (icons.empty()) LOG_WARN("app executable: cannot read the icon %s; building without one", iconPath.c_str());
    }
    // The file's final name, not exePath's: the resources are stamped into
    // the `.new-<pid>` copy before it is moved into place.
    const std::string exeName = appExeName(id);
    const std::vector<uint8_t> version = versionResource(manifest, id, exeName);

    HANDLE h = BeginUpdateResourceW(wide(exePath).c_str(), FALSE);
    if (!h) { err = lastError("BeginUpdateResource"); return false; }
    bool ok = true;
    if (!icons.empty()) {
        std::vector<uint8_t> group(sizeof(GroupIconDir), 0);
        GroupIconDir dir{0, 1, WORD(icons.size())};
        std::memcpy(group.data(), &dir, sizeof(dir));
        for (size_t i = 0; i < icons.size() && ok; ++i) {
            const int size = kIconSizes[i];
            std::vector<uint8_t> dib = iconDib(icons[i], size);
            const WORD rid = WORD(i + 1);
            ok = UpdateResourceW(h, RT_ICON, MAKEINTRESOURCEW(rid), lang, dib.data(), DWORD(dib.size())) != 0;
            GroupIconEntry e{};
            e.width = BYTE(size >= 256 ? 0 : size);
            e.height = e.width;
            e.planes = 1;
            e.bitCount = 32;
            e.bytesInRes = DWORD(dib.size());
            e.id = rid;
            const uint8_t* ep = reinterpret_cast<const uint8_t*>(&e);
            group.insert(group.end(), ep, ep + sizeof(e));
        }
        // Group 1: the first icon group is the one Explorer shows for the file.
        if (ok) ok = UpdateResourceW(h, RT_GROUP_ICON, MAKEINTRESOURCEW(1), lang, group.data(), DWORD(group.size())) != 0;
    }
    if (ok) ok = UpdateResourceW(h, RT_VERSION, MAKEINTRESOURCEW(VS_VERSION_INFO), lang, const_cast<uint8_t*>(version.data()),
                                 DWORD(version.size())) != 0;
    if (!ok) {
        err = lastError("UpdateResource");
        EndUpdateResourceW(h, TRUE);
        return false;
    }
    if (!EndUpdateResourceW(h, FALSE)) { err = lastError("EndUpdateResource"); return false; }
    return true;
}

// ---- Start menu ------------------------------------------------------------------

std::string startMenuShortcutPath(const std::string& name, bool system) {
    fs::path p;
    // The user's Programs folder is %APPDATA%\Microsoft\Windows\Start Menu\
    // Programs; reading it through APPDATA lets a test (or a portable setup)
    // point it elsewhere, the way the install root follows LOCALAPPDATA.
    const wchar_t* appdata = system ? nullptr : _wgetenv(L"APPDATA");
    if (appdata && *appdata) {
        p = fs::path(appdata) / L"Microsoft" / L"Windows" / L"Start Menu" / L"Programs";
    } else {
        PWSTR base = nullptr;
        if (FAILED(SHGetKnownFolderPath(system ? FOLDERID_CommonPrograms : FOLDERID_Programs, 0, nullptr, &base)))
            return {};
        p = fs::path(base);
        CoTaskMemFree(base);
    }
    // Its own folder: an app named "Calculator" must not replace another
    // program's shortcut of that name.
    std::string clean;
    for (char c : name) clean += std::strchr("<>:\"/\\|?*", c) || static_cast<unsigned char>(c) < 0x20 ? '_' : c;
    if (clean.empty()) clean = "app";
    p = p / L"Bro Apps" / u8path(clean + ".lnk");
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

bool writeStartMenuShortcut(const std::string& id, const std::string& name, const std::string& description,
                            const std::string& exePath, bool system, std::string& err) {
    const std::string lnk = startMenuShortcutPath(name, system);
    if (lnk.empty()) { err = "no Start menu folder"; return false; }
    std::error_code ec;
    fs::create_directories(u8path(lnk).parent_path(), ec);

    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool ok = false;
    IShellLinkW* link = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
        link->SetPath(wide(exePath).c_str());
        link->SetIconLocation(wide(exePath).c_str(), 0);
        if (!description.empty()) link->SetDescription(wide(description).c_str());
        IPropertyStore* store = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&store)))) {
            PROPVARIANT pv;
            if (SUCCEEDED(InitPropVariantFromString(wide(id).c_str(), &pv))) {
                if (SUCCEEDED(store->SetValue(PKEY_AppUserModel_ID, pv))) store->Commit();
                PropVariantClear(&pv);
            }
            store->Release();
        }
        IPersistFile* file = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))) {
            ok = SUCCEEDED(file->Save(wide(lnk).c_str(), TRUE));
            file->Release();
        }
        link->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    if (!ok) err = "cannot write " + lnk;
    return ok;
}

// ---- reading it back (bro --exe-info) ------------------------------------------------

namespace {

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string versionString(const std::vector<uint8_t>& info, const wchar_t* key) {
    wchar_t* value = nullptr;
    UINT len = 0;
    std::wstring path = std::wstring(L"\\StringFileInfo\\040904B0\\") + key;
    if (!VerQueryValueW(info.data(), path.c_str(), reinterpret_cast<void**>(&value), &len) || !value) return {};
    return narrow(std::wstring(value, len > 0 && value[len - 1] == L'\0' ? len - 1 : len));
}

// The icon as the shell extracts it (the same call Explorer's icon handler
// makes), written as a PNG.
bool extractIconPng(const std::wstring& exe, int size, const std::string& png, std::string& note) {
    HICON icon = nullptr;
    UINT id = 0;
    if (PrivateExtractIconsW(exe.c_str(), 0, size, size, &icon, &id, 1, 0) != 1 || !icon) {
        note = "the shell found no icon";
        return false;
    }
    ICONINFO ii{};
    bool ok = false;
    if (GetIconInfo(icon, &ii) && ii.hbmColor) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = size;
        bi.bmiHeader.biHeight = -size;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        std::vector<uint8_t> px(size_t(size) * size * 4);
        HDC dc = GetDC(nullptr);
        if (GetDIBits(dc, ii.hbmColor, 0, UINT(size), px.data(), &bi, DIB_RGB_COLORS) == size) {
            for (size_t i = 0; i < px.size(); i += 4) std::swap(px[i], px[i + 2]);  // BGRA -> RGBA
            ok = broimage::encode_png_file(png, px.data(), size, size, 4);
            if (!ok) note = "cannot write " + png;
        }
        ReleaseDC(nullptr, dc);
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    DestroyIcon(icon);
    return ok;
}

std::string shortcutAumid(const std::string& lnk, std::string& target) {
    std::string aumid;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IShellLinkW* link = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
        IPersistFile* file = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))) {
            if (SUCCEEDED(file->Load(wide(lnk).c_str(), STGM_READ))) {
                wchar_t path[MAX_PATH * 4] = {};
                if (SUCCEEDED(link->GetPath(path, int(std::size(path)), nullptr, 0))) target = narrow(path);
                IPropertyStore* store = nullptr;
                if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&store)))) {
                    PROPVARIANT pv;
                    PropVariantInit(&pv);
                    if (SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
                        aumid = narrow(pv.pwszVal);
                    PropVariantClear(&pv);
                    store->Release();
                }
            }
            file->Release();
        }
        link->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return aumid;
}

}  // namespace

std::string describeWindowsExe(const std::string& exePath, const AppExeIdentity& id, const std::string& iconPng) {
    std::string out;
    const std::wstring exe = wide(exePath);
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(exe.c_str(), &handle);
    if (size > 0) {
        std::vector<uint8_t> info(size);
        if (GetFileVersionInfoW(exe.c_str(), 0, size, info.data())) {
            out += "win.FileDescription=" + versionString(info, L"FileDescription") + "\n";
            out += "win.ProductVersion=" + versionString(info, L"ProductVersion") + "\n";
            out += "win.OriginalFilename=" + versionString(info, L"OriginalFilename") + "\n";
        }
    } else {
        out += "win.version=none\n";
    }
    if (HMODULE mod = LoadLibraryExW(exe.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)) {
        std::string sizes;
        if (HRSRC r = FindResourceW(mod, MAKEINTRESOURCEW(1), RT_GROUP_ICON)) {
            if (HGLOBAL g = LoadResource(mod, r)) {
                const auto* dir = static_cast<const GroupIconDir*>(LockResource(g));
                const auto* e = reinterpret_cast<const GroupIconEntry*>(dir + 1);
                for (WORD i = 0; dir && i < dir->count; ++i) {
                    if (!sizes.empty()) sizes += ',';
                    sizes += std::to_string(e[i].width ? e[i].width : 256);
                }
            }
        }
        out += "win.icon=" + (sizes.empty() ? std::string("none") : sizes) + "\n";
        FreeLibrary(mod);
    }
    for (bool system : {false, true}) {
        const std::string lnk = startMenuShortcutPath(id.name.empty() ? id.id : id.name, system);
        std::error_code ec;
        if (lnk.empty() || !fs::exists(u8path(lnk), ec)) continue;
        std::string target;
        const std::string aumid = shortcutAumid(lnk, target);
        out += "win.shortcut=" + lnk + "\n";
        out += "win.shortcut.target=" + target + "\n";
        out += "win.shortcut.AppUserModelID=" + aumid + "\n";
    }
    if (!iconPng.empty()) {
        std::string note;
        if (extractIconPng(exe, 256, iconPng, note)) out += "win.icon.png=" + iconPng + "\n";
        else out += "win.icon.png=failed: " + note + "\n";
    }
    return out;
}

void removeStartMenuShortcut(const std::string& name, bool system) {
    const std::string lnk = startMenuShortcutPath(name, system);
    if (lnk.empty()) return;
    std::error_code ec;
    fs::remove(u8path(lnk), ec);
    fs::remove(u8path(lnk).parent_path(), ec);  // only when it is empty
}

}  // namespace bro::engine

#endif  // _WIN32
