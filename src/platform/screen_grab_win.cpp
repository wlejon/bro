// screen_grab.h on Windows: DXGI Desktop Duplication reads each output's
// desktop image (what DWM composited, other apps' windows and DirectX
// content included, no cursor); an output it cannot duplicate (a secure
// desktop, a session not on the console, a remote session's adapter) is read
// with GDI's BitBlt instead. The thread is per-monitor DPI aware for the
// call, so every coordinate is a physical pixel.
#include "platform/screen_grab.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <shellscalingapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>

namespace bro::platform::screen {

namespace {

using Microsoft::WRL::ComPtr;

// Physical pixels for the length of one call, whatever the process declared.
struct DpiScope {
    DPI_AWARENESS_CONTEXT old = nullptr;
    DpiScope() { old = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); }
    ~DpiScope() {
        if (old) SetThreadDpiAwarenessContext(old);
    }
};

struct Output {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> output;
    Display info;
};

std::string narrow(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

double monitorScale(HMONITOR m) {
    UINT dx = 96, dy = 96;
    if (m && GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &dx, &dy) == S_OK && dx) return dx / 96.0;
    return 1.0;
}

std::vector<Output> enumerateOutputs() {
    std::vector<Output> out;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return out;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> o;
        for (UINT i = 0; adapter->EnumOutputs(i, &o) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_OUTPUT_DESC d{};
            if (FAILED(o->GetDesc(&d)) || !d.AttachedToDesktop) continue;
            Output rec;
            rec.adapter = adapter;
            o.As(&rec.output);
            rec.info.name = narrow(d.DeviceName);
            rec.info.x = d.DesktopCoordinates.left;
            rec.info.y = d.DesktopCoordinates.top;
            rec.info.width = d.DesktopCoordinates.right - d.DesktopCoordinates.left;
            rec.info.height = d.DesktopCoordinates.bottom - d.DesktopCoordinates.top;
            rec.info.scale = monitorScale(d.Monitor);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            rec.info.primary = d.Monitor && GetMonitorInfoW(d.Monitor, &mi) && (mi.dwFlags & MONITORINFOF_PRIMARY);
            out.push_back(std::move(rec));
        }
    }
    // Primary first, then as the desktop lays them out.
    std::stable_sort(out.begin(), out.end(), [](const Output& a, const Output& b) {
        if (a.info.primary != b.info.primary) return a.info.primary;
        return a.info.x != b.info.x ? a.info.x < b.info.x : a.info.y < b.info.y;
    });
    for (size_t i = 0; i < out.size(); ++i) out[i].info.index = static_cast<int>(i);
    return out;
}

// Displays from GDI where DXGI sees none (a basic display driver, some
// remote sessions).
BOOL CALLBACK collectMonitor(HMONITOR m, HDC, LPRECT, LPARAM p) {
    auto* list = reinterpret_cast<std::vector<Display>*>(p);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(m, &mi)) return TRUE;
    Display d;
    d.name = narrow(mi.szDevice);
    d.x = mi.rcMonitor.left;
    d.y = mi.rcMonitor.top;
    d.width = mi.rcMonitor.right - mi.rcMonitor.left;
    d.height = mi.rcMonitor.bottom - mi.rcMonitor.top;
    d.scale = monitorScale(m);
    d.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    list->push_back(d);
    return TRUE;
}

std::vector<Display> gdiDisplays() {
    std::vector<Display> list;
    EnumDisplayMonitors(nullptr, nullptr, collectMonitor, reinterpret_cast<LPARAM>(&list));
    std::stable_sort(list.begin(), list.end(), [](const Display& a, const Display& b) {
        if (a.primary != b.primary) return a.primary;
        return a.x != b.x ? a.x < b.x : a.y < b.y;
    });
    for (size_t i = 0; i < list.size(); ++i) list[i].index = static_cast<int>(i);
    return list;
}

// The intersection of two rectangles as x0,y0,x1,y1; false when empty.
bool intersect(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh, RECT& r) {
    r.left = std::max(ax, bx);
    r.top = std::max(ay, by);
    r.right = std::min(ax + aw, bx + bw);
    r.bottom = std::min(ay + ah, by + bh);
    return r.right > r.left && r.bottom > r.top;
}

// GDI: `r` of the screen into `g` (whose origin is g.x, g.y).
bool gdiInto(const RECT& r, Grab& g, std::string& why) {
    const int w = r.right - r.left, h = r.bottom - r.top;
    HDC screen = GetDC(nullptr);
    if (!screen) {
        why = "GetDC failed (no interactive desktop)";
        return false;
    }
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = mem ? CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    bool ok = false;
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(mem, bmp);
        ok = BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT) != 0;
        SelectObject(mem, old);
        if (ok) {
            GdiFlush();
            const uint8_t* src = static_cast<const uint8_t*>(bits);
            for (int y = 0; y < h; ++y) {
                uint8_t* dst = g.rgba.data() + ((static_cast<size_t>(r.top - g.y + y) * g.width) + (r.left - g.x)) * 4;
                const uint8_t* s = src + static_cast<size_t>(y) * w * 4;
                for (int x = 0; x < w; ++x) {
                    dst[x * 4 + 0] = s[x * 4 + 2];
                    dst[x * 4 + 1] = s[x * 4 + 1];
                    dst[x * 4 + 2] = s[x * 4 + 0];
                    dst[x * 4 + 3] = 255;
                }
            }
        } else {
            why = "BitBlt from the screen failed (the session is not on the console?)";
        }
    } else {
        why = "could not allocate the capture bitmap";
    }
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok;
}

// DXGI: `r` (inside output `o`) into `g`. False (with `why`) when the output
// cannot be duplicated; the caller reads it with GDI then.
bool dxgiInto(const Output& o, const RECT& r, Grab& g, std::string& why) {
    if (!o.output) {
        why = "no IDXGIOutput1";
        return false;
    }
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    HRESULT hr = D3D11CreateDevice(o.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &device, nullptr, &ctx);
    if (FAILED(hr)) {
        why = "D3D11CreateDevice failed";
        return false;
    }
    ComPtr<IDXGIOutputDuplication> dup;
    hr = o.output->DuplicateOutput(device.Get(), &dup);
    if (FAILED(hr)) {
        why = hr == E_ACCESSDENIED ? "desktop duplication is not allowed on this desktop"
                                   : "DuplicateOutput failed";
        return false;
    }
    DXGI_OUTDUPL_DESC dd{};
    dup->GetDesc(&dd);
    // A rotated output's image comes unrotated; GDI reads it the right way up.
    if (dd.Rotation != DXGI_MODE_ROTATION_IDENTITY && dd.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
        why = "the output is rotated";
        return false;
    }
    // The first frame of a new duplication is the whole desktop image; one
    // that comes without it (nothing presented yet) is waited for briefly.
    ComPtr<IDXGIResource> res;
    DXGI_OUTDUPL_FRAME_INFO fi{};
    for (int attempt = 0; attempt < 4; ++attempt) {
        res.Reset();
        hr = dup->AcquireNextFrame(attempt == 0 ? 0 : 100, &fi, &res);
        if (SUCCEEDED(hr) && fi.LastPresentTime.QuadPart != 0) break;
        if (SUCCEEDED(hr)) dup->ReleaseFrame();
        else if (hr != DXGI_ERROR_WAIT_TIMEOUT) break;
    }
    if (FAILED(hr) || !res) {
        why = hr == DXGI_ERROR_WAIT_TIMEOUT ? "no desktop frame came" : "AcquireNextFrame failed";
        return false;
    }
    bool ok = false;
    ComPtr<ID3D11Texture2D> tex;
    if (SUCCEEDED(res.As(&tex))) {
        D3D11_TEXTURE2D_DESC td{};
        tex->GetDesc(&td);
        D3D11_TEXTURE2D_DESC sd = td;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.SampleDesc = {1, 0};
        ComPtr<ID3D11Texture2D> staging;
        if (td.Format == DXGI_FORMAT_B8G8R8A8_UNORM && SUCCEEDED(device->CreateTexture2D(&sd, nullptr, &staging))) {
            ctx->CopyResource(staging.Get(), tex.Get());
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                const auto* base = static_cast<const uint8_t*>(m.pData);
                const int tw = static_cast<int>(td.Width), th = static_cast<int>(td.Height);
                for (int y = r.top; y < r.bottom; ++y) {
                    uint8_t* dst = g.rgba.data() + (static_cast<size_t>(y - g.y) * g.width + (r.left - g.x)) * 4;
                    const int ty = y - o.info.y;
                    for (int x = r.left; x < r.right; ++x, dst += 4) {
                        const int tx = x - o.info.x;
                        if (tx < 0 || ty < 0 || tx >= tw || ty >= th) continue;
                        const uint8_t* s = base + static_cast<size_t>(ty) * m.RowPitch + static_cast<size_t>(tx) * 4;
                        dst[0] = s[2];
                        dst[1] = s[1];
                        dst[2] = s[0];
                        dst[3] = 255;
                    }
                }
                ctx->Unmap(staging.Get(), 0);
                ok = true;
            } else {
                why = "mapping the desktop image failed";
            }
        } else {
            why = "the desktop image is not 8-bit BGRA";
        }
    } else {
        why = "the desktop frame is not a texture";
    }
    dup->ReleaseFrame();
    return ok;
}

// The pointer, drawn into `g` where it overlaps: its pixels under the
// cursor go into a DIB, DrawIconEx draws the cursor over them (masks and
// colour cursors alike), and they come back.
void drawCursor(Grab& g) {
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return;
    ICONINFO ii{};
    if (!GetIconInfo(ci.hCursor, &ii)) return;
    BITMAP bm{};
    GetObject(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof(bm), &bm);
    const int cw = bm.bmWidth, ch = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;
    const int left = ci.ptScreenPos.x - static_cast<int>(ii.xHotspot);
    const int top = ci.ptScreenPos.y - static_cast<int>(ii.yHotspot);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    RECT r{};
    if (cw <= 0 || ch <= 0 || !intersect(left, top, cw, ch, g.x, g.y, g.width, g.height, r)) return;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = cw;
    bi.bmiHeader.biHeight = -ch;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = mem ? CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (bmp && bits) {
        auto* px = static_cast<uint8_t*>(bits);
        auto at = [&](int x, int y) { return g.rgba.data() + (static_cast<size_t>(y - g.y) * g.width + (x - g.x)) * 4; };
        for (int y = r.top; y < r.bottom; ++y)
            for (int x = r.left; x < r.right; ++x) {
                const uint8_t* s = at(x, y);
                uint8_t* d = px + (static_cast<size_t>(y - top) * cw + (x - left)) * 4;
                d[0] = s[2];
                d[1] = s[1];
                d[2] = s[0];
                d[3] = 255;
            }
        HGDIOBJ old = SelectObject(mem, bmp);
        DrawIconEx(mem, 0, 0, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
        GdiFlush();
        SelectObject(mem, old);
        for (int y = r.top; y < r.bottom; ++y)
            for (int x = r.left; x < r.right; ++x) {
                uint8_t* d = at(x, y);
                const uint8_t* s = px + (static_cast<size_t>(y - top) * cw + (x - left)) * 4;
                d[0] = s[2];
                d[1] = s[1];
                d[2] = s[0];
            }
    }
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

// The lock screen (and UAC's prompt) is the Winlogon desktop, which no user
// program may read; the user's own is "Default".
bool inputDesktopIsSecure() {
    HDESK d = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!d) return true;  // not even its name is ours to see
    wchar_t name[64] = {};
    DWORD len = 0;
    const bool ok = GetUserObjectInformationW(d, UOI_NAME, name, sizeof(name), &len) != 0;
    CloseDesktop(d);
    return ok && _wcsicmp(name, L"Default") != 0;
}

}  // namespace

std::vector<Display> displays(std::string* why) {
    DpiScope dpi;
    std::vector<Display> list;
    for (const Output& o : enumerateOutputs()) list.push_back(o.info);
    if (list.empty()) list = gdiDisplays();
    if (list.empty() && why) *why = "no display is attached to this desktop";
    return list;
}

Status grab(int x, int y, int width, int height, Grab& out, std::string& why, bool cursor) {
    DpiScope dpi;
    std::vector<Output> outputs = enumerateOutputs();
    std::vector<Display> infos;
    for (const Output& o : outputs) infos.push_back(o.info);
    if (infos.empty()) infos = gdiDisplays();
    if (infos.empty()) {
        why = "no display is attached to this desktop";
        return Status::Unsupported;
    }
    // The virtual desktop: the union of the displays.
    int vx0 = infos[0].x, vy0 = infos[0].y, vx1 = vx0 + infos[0].width, vy1 = vy0 + infos[0].height;
    for (const Display& d : infos) {
        vx0 = std::min(vx0, d.x);
        vy0 = std::min(vy0, d.y);
        vx1 = std::max(vx1, d.x + d.width);
        vy1 = std::max(vy1, d.y + d.height);
    }
    if (width <= 0 || height <= 0) {
        x = vx0;
        y = vy0;
        width = vx1 - vx0;
        height = vy1 - vy0;
    }
    RECT clip{};
    if (!intersect(x, y, width, height, vx0, vy0, vx1 - vx0, vy1 - vy0, clip)) {
        why = "the rectangle is outside the desktop";
        return Status::Failed;
    }
    out.x = clip.left;
    out.y = clip.top;
    out.width = clip.right - clip.left;
    out.height = clip.bottom - clip.top;
    out.rgba.assign(static_cast<size_t>(out.width) * out.height * 4, 0);
    for (size_t i = 3; i < out.rgba.size(); i += 4) out.rgba[i] = 255;

    bool any = false, usedGdi = false, usedDxgi = false;
    std::string lastWhy;
    for (const Display& d : infos) {
        RECT part{};
        if (!intersect(out.x, out.y, out.width, out.height, d.x, d.y, d.width, d.height, part)) continue;
        std::string dxgiErr = "no DXGI output", gdiErr;
        bool done = false;
        if (static_cast<size_t>(d.index) < outputs.size()) done = dxgiInto(outputs[d.index], part, out, dxgiErr);
        if (done) {
            usedDxgi = true;
        } else {
            done = gdiInto(part, out, gdiErr);
            usedGdi = usedGdi || done;
        }
        if (done) any = true;
        else lastWhy = "DXGI: " + dxgiErr + "; GDI: " + gdiErr;
    }
    if (!any) {
        why = lastWhy.empty() ? "the desktop could not be read" : lastWhy;
        if (inputDesktopIsSecure()) why = "the screen is locked (the secure desktop has the display): " + why;
        return Status::Denied;
    }
    out.backend = usedDxgi && !usedGdi ? "dxgi" : usedGdi && !usedDxgi ? "gdi" : "dxgi+gdi";
    if (cursor) drawCursor(out);
    return Status::Ok;
}

}  // namespace bro::platform::screen
