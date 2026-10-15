// desktop_session.h on Windows: GDI reads the composited screen (DWM's
// output, so other apps' windows are in it), SendInput injects into the
// session's input stream, ahead of low-level hooks and RegisterHotKey.
#include "platform/desktop_session.h"
#include "platform/desktop_platform.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>

namespace bro::platform::desktop {

namespace {

// Linux evdev KEY_* to the PC/AT set-1 scan code SendInput takes; 0xE0xx is
// an extended key. Codes 1-88 are the set-1 codes themselves.
uint16_t evdevToScan(uint32_t code) {
    if (code >= 1 && code <= 83) return static_cast<uint16_t>(code);
    switch (code) {
        case 86: return 0x56;    // KEY_102ND
        case 87: return 0x57;    // F11
        case 88: return 0x58;    // F12
        case 96: return 0xE01C;  // KP enter
        case 97: return 0xE01D;  // right ctrl
        case 98: return 0xE035;  // KP slash
        case 99: return 0xE037;  // sysrq / print screen
        case 100: return 0xE038; // right alt
        case 102: return 0xE047; // home
        case 103: return 0xE048; // up
        case 104: return 0xE049; // page up
        case 105: return 0xE04B; // left
        case 106: return 0xE04D; // right
        case 107: return 0xE04F; // end
        case 108: return 0xE050; // down
        case 109: return 0xE051; // page down
        case 110: return 0xE052; // insert
        case 111: return 0xE053; // delete
        case 113: return 0xE020; // mute
        case 114: return 0xE02E; // volume down
        case 115: return 0xE030; // volume up
        case 125: return 0xE05B; // left meta (Windows key)
        case 126: return 0xE05C; // right meta
        case 127: return 0xE05D; // compose / menu
        case 163: return 0xE019; // next song
        case 164: return 0xE022; // play/pause
        case 165: return 0xE010; // previous song
        case 166: return 0xE024; // stop
        default: break;
    }
    if (code >= 183 && code <= 193) return static_cast<uint16_t>(0x64 + (code - 183));  // F13-F23
    if (code == 194) return 0x76;                                                       // F24
    return 0;
}

bool send(INPUT& in) { return SendInput(1, &in, sizeof(in)) == 1; }

}  // namespace

bool clientRectOnScreen(const Window* window, int& x, int& y, int& width, int& height) {
    HWND hwnd = hwndOf(window);
    if (!hwnd) return false;
    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) return false;
    POINT origin{0, 0};
    if (!ClientToScreen(hwnd, &origin)) return false;
    x = origin.x;
    y = origin.y;
    width = rc.right - rc.left;
    height = rc.bottom - rc.top;
    return width > 0 && height > 0;
}

bool captureScreenRect(int x, int y, int width, int height, std::vector<uint8_t>& rgba, std::string* why) {
    if (width <= 0 || height <= 0) {
        if (why) *why = "empty screen rectangle";
        return false;
    }
    HDC screen = GetDC(nullptr);
    if (!screen) {
        if (why) *why = "GetDC failed (no interactive desktop?)";
        return false;
    }
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = mem ? CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    bool ok = false;
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(mem, bmp);
        ok = BitBlt(mem, 0, 0, width, height, screen, x, y, SRCCOPY | CAPTUREBLT) != 0;
        SelectObject(mem, old);
        if (ok) {
            GdiFlush();
            const size_t n = static_cast<size_t>(width) * height;
            rgba.resize(n * 4);
            const uint8_t* src = static_cast<const uint8_t*>(bits);
            for (size_t i = 0; i < n; ++i) {
                rgba[i * 4 + 0] = src[i * 4 + 2];
                rgba[i * 4 + 1] = src[i * 4 + 1];
                rgba[i * 4 + 2] = src[i * 4 + 0];
                rgba[i * 4 + 3] = 255;
            }
        } else if (why) {
            *why = "BitBlt from the screen failed (session not on the console?)";
        }
    } else if (why) {
        *why = "could not allocate the capture bitmap";
    }
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok;
}

bool sendKey(uint32_t evdevCode, bool pressed) {
    const uint16_t scan = evdevToScan(evdevCode);
    if (!scan) return false;
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = static_cast<WORD>(scan & 0xFF);
    in.ki.dwFlags = KEYEVENTF_SCANCODE;
    if (scan & 0xE000) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (!pressed) in.ki.dwFlags |= KEYEVENTF_KEYUP;
    return send(in);
}

bool sendPointerTo(int screenX, int screenY) {
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 1 || vh <= 1) return false;
    INPUT in{};
    in.type = INPUT_MOUSE;
    // Normalised 0..65535 over the virtual desktop, pixel centres.
    in.mi.dx = static_cast<LONG>(((screenX - vx) * 65535LL + (vw - 1) / 2) / (vw - 1));
    in.mi.dy = static_cast<LONG>(((screenY - vy) * 65535LL + (vh - 1) / 2) / (vh - 1));
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    return send(in);
}

bool sendPointerBy(int dx, int dy) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    return send(in);
}

bool sendButton(uint32_t evdevButton, bool pressed) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    switch (evdevButton) {
        case 0x110: in.mi.dwFlags = pressed ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
        case 0x111: in.mi.dwFlags = pressed ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
        case 0x112: in.mi.dwFlags = pressed ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
        case 0x113:
        case 0x114:
            in.mi.dwFlags = pressed ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
            in.mi.mouseData = evdevButton == 0x113 ? XBUTTON1 : XBUTTON2;
            break;
        default: return false;
    }
    return send(in);
}

bool sendWheel(int32_t wheelX, int32_t wheelY) {
    bool ok = true;
    if (wheelY) {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_WHEEL;
        in.mi.mouseData = static_cast<DWORD>(-wheelY);  // Windows: + is away from the user
        ok = send(in) && ok;
    }
    if (wheelX) {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_HWHEEL;
        in.mi.mouseData = static_cast<DWORD>(wheelX);
        ok = send(in) && ok;
    }
    return ok;
}

}  // namespace bro::platform::desktop
