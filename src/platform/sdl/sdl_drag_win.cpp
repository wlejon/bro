// Drags out of an SDL window on Windows: OLE DoDragDrop with our own
// IDataObject and IDropSource, the picture through the shell's drag-image
// helper. See sdl_drag.h.
//
// DoDragDrop is modal: it runs its own message loop until the button comes
// up, so the whole drag happens inside sdlNativeDragBegin. SDL's windows are
// OLE drop targets themselves; when the drag is released over our own window
// the source cancels it instead (SDL would read it as a foreign drop) and the
// page takes its own drop. Afterwards SDL is told the mouse capture changed,
// which makes it notice the button is up again (OLE swallowed the release).
#include "platform/sdl/sdl_drag.h"
#include "util/log.h"

#include <SDL3/SDL.h>

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <ole2.h>
#include <shlobj.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace bro::platform {

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string toAnsi(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_ACP, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_ACP, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string percentDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hexValue(s[i + 1]) >= 0 && hexValue(s[i + 2]) >= 0) {
            out.push_back(static_cast<char>(hexValue(s[i + 1]) * 16 + hexValue(s[i + 2])));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

// file:///C:/a%20b.txt -> C:\a b.txt, file://server/share/x -> \\server\share\x.
// Empty for anything that is not a file URI.
std::wstring pathFromFileUri(const std::string& uri) {
    if (uri.size() < 7 || _strnicmp(uri.c_str(), "file://", 7) != 0) return {};
    std::string rest = uri.substr(7);
    std::string path;
    if (!rest.empty() && rest[0] == '/') {
        path = percentDecode(rest);
        // /C:/x -> C:/x
        if (path.size() >= 3 && std::isalpha(static_cast<unsigned char>(path[1])) && path[2] == ':')
            path.erase(0, 1);
    } else {
        size_t slash = rest.find('/');
        std::string host = rest.substr(0, slash);
        std::string tail = slash == std::string::npos ? std::string() : rest.substr(slash);
        if (host.empty() || _stricmp(host.c_str(), "localhost") == 0) {
            path = percentDecode(tail);
            if (path.size() >= 3 && path[0] == '/' && path[2] == ':') path.erase(0, 1);
        } else {
            path = "//" + host + percentDecode(tail);
        }
    }
    std::replace(path.begin(), path.end(), '/', '\\');
    return widen(path);
}

std::vector<std::string> uriListEntries(const std::string& list) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < list.size()) {
        size_t end = list.find_first_of("\r\n", pos);
        std::string line = list.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? list.size() : end + 1;
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (!line.empty() && line[0] != '#') out.push_back(line);
    }
    return out;
}

HGLOBAL globalFrom(const void* bytes, size_t size) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, size ? size : 1);
    if (!h) return nullptr;
    if (void* p = GlobalLock(h)) {
        if (size) std::memcpy(p, bytes, size);
        GlobalUnlock(h);
    }
    return h;
}

HGLOBAL duplicateGlobal(HGLOBAL src) {
    const SIZE_T size = GlobalSize(src);
    const void* p = GlobalLock(src);
    if (!p) return nullptr;
    HGLOBAL h = globalFrom(p, size);
    GlobalUnlock(src);
    return h;
}

// CF_HTML around a fragment.
std::string cfHtml(const std::string& fragment) {
    static const char* kHeader =
        "Version:0.9\r\nStartHTML:%010d\r\nEndHTML:%010d\r\nStartFragment:%010d\r\nEndFragment:%010d\r\n";
    const std::string pre = "<html><body>\r\n<!--StartFragment-->";
    const std::string post = "<!--EndFragment-->\r\n</body></html>";
    char header[256];
    const int headerLen = std::snprintf(header, sizeof header, kHeader, 0, 0, 0, 0);
    const int startHtml = headerLen;
    const int startFrag = startHtml + static_cast<int>(pre.size());
    const int endFrag = startFrag + static_cast<int>(fragment.size());
    const int endHtml = endFrag + static_cast<int>(post.size());
    std::snprintf(header, sizeof header, kHeader, startHtml, endHtml, startFrag, endFrag);
    return std::string(header) + pre + fragment + post;
}

// --- IDataObject ---

class DataObject final : public IDataObject {
public:
    ~DataObject() {
        for (auto& e : entries_) GlobalFree(e.data);
    }

    // Takes ownership of `data`. Replaces a format already held.
    void put(CLIPFORMAT format, HGLOBAL data) {
        if (!data) return;
        for (auto& e : entries_) {
            if (e.format == format) {
                GlobalFree(e.data);
                e.data = data;
                return;
            }
        }
        entries_.push_back({format, data});
    }
    void putBytes(CLIPFORMAT format, const std::string& bytes, bool nulTerminate) {
        std::string b = bytes;
        if (nulTerminate) b.push_back('\0');
        put(format, globalFrom(b.data(), b.size()));
    }
    void putWide(CLIPFORMAT format, const std::wstring& text) {
        put(format, globalFrom(text.c_str(), (text.size() + 1) * sizeof(wchar_t)));
    }
    bool has(CLIPFORMAT format) const {
        return std::any_of(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.format == format; });
    }
    // A DWORD the target set (CFSTR_PERFORMEDDROPEFFECT and friends); -1 none.
    long dword(CLIPFORMAT format) const {
        for (const auto& e : entries_) {
            if (e.format != format || GlobalSize(e.data) < sizeof(DWORD)) continue;
            long v = -1;
            if (const void* p = GlobalLock(e.data)) {
                v = static_cast<long>(*static_cast<const DWORD*>(p));
                GlobalUnlock(e.data);
            }
            return v;
        }
        return -1;
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
            *out = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&ref_)); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG n = InterlockedDecrement(&ref_);
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }

    // IDataObject
    STDMETHODIMP GetData(FORMATETC* fe, STGMEDIUM* medium) override {
        if (!fe || !medium) return E_INVALIDARG;
        const Entry* e = find(fe);
        if (!e) return DV_E_FORMATETC;
        HGLOBAL copy = duplicateGlobal(e->data);
        if (!copy) return E_OUTOFMEMORY;
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = copy;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }
    STDMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    STDMETHODIMP QueryGetData(FORMATETC* fe) override {
        if (!fe) return E_INVALIDARG;
        return find(fe) ? S_OK : DV_E_FORMATETC;
    }
    STDMETHODIMP GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
        if (out) out->ptd = nullptr;
        return DATA_S_SAMEFORMATETC;
    }
    // The drag-image helper and drop targets store their own formats here
    // (DragImageBits, DropDescription, the performed drop effect).
    STDMETHODIMP SetData(FORMATETC* fe, STGMEDIUM* medium, BOOL release) override {
        if (!fe || !medium) return E_INVALIDARG;
        if (fe->tymed != TYMED_HGLOBAL || medium->tymed != TYMED_HGLOBAL || !medium->hGlobal) return DV_E_TYMED;
        HGLOBAL copy = duplicateGlobal(medium->hGlobal);
        if (!copy) return E_OUTOFMEMORY;
        put(fe->cfFormat, copy);
        if (release) ReleaseStgMedium(medium);
        return S_OK;
    }
    STDMETHODIMP EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override {
        if (!out) return E_POINTER;
        if (direction != DATADIR_GET) return E_NOTIMPL;
        std::vector<FORMATETC> list;
        list.reserve(entries_.size());
        for (const auto& e : entries_) list.push_back({e.format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
        return SHCreateStdEnumFmtEtc(static_cast<UINT>(list.size()), list.data(), out);
    }
    STDMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    STDMETHODIMP DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    STDMETHODIMP EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    struct Entry {
        CLIPFORMAT format;
        HGLOBAL data;
    };
    const Entry* find(const FORMATETC* fe) const {
        if (!(fe->tymed & TYMED_HGLOBAL) || fe->dwAspect != DVASPECT_CONTENT) return nullptr;
        for (const auto& e : entries_)
            if (e.format == fe->cfFormat) return &e;
        return nullptr;
    }
    LONG ref_ = 1;
    std::vector<Entry> entries_;
};

// --- IDropSource ---

class DropSource final : public IDropSource {
public:
    DropSource(HWND hwnd, uint32_t windowId) : hwnd_(hwnd), windowId_(windowId) {}

    bool droppedOnOwnWindow() const { return ownDrop_; }
    float ownX() const { return x_; }
    float ownY() const { return y_; }

    STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *out = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&ref_)); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG n = InterlockedDecrement(&ref_);
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }

    STDMETHODIMP QueryContinueDrag(BOOL escape, DWORD keys) override {
        if (escape) {
            leave();
            return DRAGDROP_S_CANCEL;
        }
        const bool over = track();
        if (keys & MK_LBUTTON) return S_OK;
        if (over) {
            // Released over our own window: the page drops it itself, with
            // the data it set. SDL's drop target never sees it.
            ownDrop_ = true;
            return DRAGDROP_S_CANCEL;
        }
        return DRAGDROP_S_DROP;
    }
    STDMETHODIMP GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

private:
    // Report where the pointer is while it is over our window; true then.
    bool track() {
        POINT pt;
        if (!GetCursorPos(&pt)) return inside_;
        if (!overWindow(pt)) {
            leave();
            return false;
        }
        ScreenToClient(hwnd_, &pt);
        const float x = static_cast<float>(pt.x), y = static_cast<float>(pt.y);
        if (!inside_ || x != x_ || y != y_) {
            inside_ = true;
            x_ = x;
            y_ = y;
            DragReport r;
            r.kind = DragReport::Kind::Motion;
            r.windowId = windowId_;
            r.x = x;
            r.y = y;
            sdlDragReport(std::move(r));
            sdlDeliverDragReports();
        }
        return true;
    }
    // Whether our window is what the pointer is over. The drag picture is a
    // window of its own that follows the pointer (the shell helper's, made
    // in this process); where it is what WindowFromPoint finds, our client
    // rectangle decides.
    bool overWindow(POINT pt) const {
        HWND at = WindowFromPoint(pt);
        HWND root = at ? GetAncestor(at, GA_ROOT) : nullptr;
        if (!root) return false;
        if (root == hwnd_) return true;
        DWORD pid = 0;
        GetWindowThreadProcessId(root, &pid);
        wchar_t cls[64] = {};
        GetClassNameW(root, cls, 64);
        if (pid != GetCurrentProcessId() && wcscmp(cls, L"SysDragImage") != 0) return false;
        RECT rc;
        if (!GetClientRect(hwnd_, &rc)) return false;
        POINT local = pt;
        ScreenToClient(hwnd_, &local);
        return PtInRect(&rc, local) != FALSE;
    }
    void leave() {
        if (!inside_) return;
        inside_ = false;
        DragReport r;
        r.kind = DragReport::Kind::Leave;
        r.windowId = windowId_;
        sdlDragReport(std::move(r));
        sdlDeliverDragReports();
    }

    LONG ref_ = 1;
    HWND hwnd_;
    uint32_t windowId_;
    bool inside_ = true;  // the drag begins over the page
    bool ownDrop_ = false;
    float x_ = -1, y_ = -1;
};

CLIPFORMAT registered(const char* name) { return static_cast<CLIPFORMAT>(RegisterClipboardFormatA(name)); }

// What the page set, under the Windows formats other applications read.
void fillData(DataObject& obj, const DragSource& drag) {
    std::string text, uriList, html;
    bool haveText = false, haveUris = false, haveHtml = false;
    for (const auto& [mime, bytes] : drag.data) {
        if (mime == "text/plain" && !haveText) {
            text = bytes;
            haveText = true;
        } else if (mime == "text/uri-list" && !haveUris) {
            uriList = bytes;
            haveUris = true;
        } else if (mime == "text/html" && !haveHtml) {
            html = bytes;
            haveHtml = true;
        }
    }

    if (haveUris) {
        std::vector<std::wstring> files;
        std::vector<std::string> urls;
        for (const auto& uri : uriListEntries(uriList)) {
            std::wstring path = pathFromFileUri(uri);
            if (!path.empty()) files.push_back(std::move(path));
            else urls.push_back(uri);
        }
        if (!files.empty()) {
            // CF_HDROP: a DROPFILES header, then the wide paths, each NUL-
            // terminated, and one more NUL.
            size_t chars = 1;
            for (const auto& f : files) chars += f.size() + 1;
            std::vector<uint8_t> buf(sizeof(DROPFILES) + chars * sizeof(wchar_t), 0);
            auto* df = reinterpret_cast<DROPFILES*>(buf.data());
            df->pFiles = sizeof(DROPFILES);
            df->fWide = TRUE;
            auto* out = reinterpret_cast<wchar_t*>(buf.data() + sizeof(DROPFILES));
            for (const auto& f : files) {
                std::memcpy(out, f.c_str(), (f.size() + 1) * sizeof(wchar_t));
                out += f.size() + 1;
            }
            obj.put(CF_HDROP, globalFrom(buf.data(), buf.size()));
            // Explorer copies unless the source asks otherwise.
            DWORD preferred = drag.allowCopy ? DROPEFFECT_COPY : DROPEFFECT_MOVE;
            obj.put(registered("Preferred DropEffect"), globalFrom(&preferred, sizeof preferred));
        }
        if (!urls.empty()) {
            obj.putWide(registered("UniformResourceLocatorW"), widen(urls.front()));
            obj.putBytes(registered("UniformResourceLocator"), toAnsi(widen(urls.front())), true);
            if (!haveText) {
                std::string joined;
                for (size_t i = 0; i < urls.size(); ++i) joined += (i ? "\r\n" : "") + urls[i];
                text = joined;
                haveText = true;
            }
        }
        obj.putBytes(registered("text/uri-list"), uriList, true);
    }

    if (haveText) {
        // Notepad and the edit controls want CRLF line ends.
        std::string crlf;
        crlf.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) crlf.push_back('\r');
            crlf.push_back(text[i]);
        }
        const std::wstring wide = widen(crlf);
        obj.putWide(CF_UNICODETEXT, wide);
        obj.putBytes(CF_TEXT, toAnsi(wide), true);
        // What another SDL window (another bro) reads.
        obj.putBytes(registered("text/plain;charset=utf-8"), text, true);
    }
    if (haveHtml) obj.putBytes(registered("HTML Format"), cfHtml(html), true);

    for (const auto& [mime, bytes] : drag.data) {
        if (mime == "text/plain" || mime == "text/uri-list" || mime == "text/html") continue;
        // The X11 names the engine adds for text; nothing reads them here.
        if (mime == "UTF8_STRING" || mime == "TEXT" || mime == "STRING" || mime == "text/plain;charset=utf-8")
            continue;
        if (mime == "text/rtf" || mime == "application/rtf") {
            obj.putBytes(registered("Rich Text Format"), bytes, false);
            continue;
        }
        if (mime == "image/png") obj.putBytes(registered("PNG"), bytes, false);
        // Anything else under its own MIME name, as the page set it.
        const CLIPFORMAT f = registered(mime.c_str());
        if (f && !obj.has(f)) obj.putBytes(f, bytes, false);
    }
}

// The picture, through the shell helper, which shows it over every target.
void setDragImage(DataObject& obj, const DragSource& drag) {
    if (drag.iconWidth <= 0 || drag.iconHeight <= 0 ||
        drag.iconBgra.size() < static_cast<size_t>(drag.iconWidth) * drag.iconHeight * 4)
        return;
    // The helper wants the pointer inside the picture: pad it out to reach
    // the hotspot when the icon sits off to the side.
    const int padX = std::max(0, -drag.hotX), padY = std::max(0, -drag.hotY);
    const int w = std::max(drag.iconWidth + padX, drag.hotX + 1);
    const int h = std::max(drag.iconHeight + padY, drag.hotY + 1);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) return;
    std::memset(bits, 0, static_cast<size_t>(w) * h * 4);
    for (int row = 0; row < drag.iconHeight; ++row) {
        std::memcpy(static_cast<uint8_t*>(bits) + (static_cast<size_t>(row + padY) * w + padX) * 4,
                    drag.iconBgra.data() + static_cast<size_t>(row) * drag.iconWidth * 4,
                    static_cast<size_t>(drag.iconWidth) * 4);
    }

    IDragSourceHelper* helper = nullptr;
    if (FAILED(CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper)))) {
        DeleteObject(bmp);
        return;
    }
    SHDRAGIMAGE img{};
    img.sizeDragImage = {w, h};
    img.ptOffset = {drag.hotX + padX, drag.hotY + padY};
    img.hbmpDragImage = bmp;
    img.crColorKey = CLR_NONE;
    if (FAILED(helper->InitializeFromBitmap(&img, &obj))) DeleteObject(bmp);  // owned by the helper on success
    helper->Release();
}

const char* actionName(DWORD effect) {
    if (effect & DROPEFFECT_MOVE) return "move";
    if (effect & DROPEFFECT_COPY) return "copy";
    if (effect & DROPEFFECT_LINK) return "link";
    return "none";
}

// Test seam: BRO_DRAG_TEST_TARGET names a file holding an IDropTarget that
// another process marshaled (CoMarshalInterface, table-strong). The drag is
// offered straight to it — DragEnter, DragOver, Drop with this drag's data
// object, across processes as OLE does it — instead of following the mouse
// through DoDragDrop, so the source side is checked without moving anyone's
// pointer. Answers as DoDragDrop would; false when there is no such target.
bool dragToTestTarget(IDataObject* data, DWORD allowed, HRESULT& hr, DWORD& effect) {
    const char* path = std::getenv("BRO_DRAG_TEST_TARGET");
    if (!path || !*path) return false;
    std::string bytes;
    if (FILE* f = std::fopen(path, "rb")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) bytes.append(buf, n);
        std::fclose(f);
    }
    IStream* stream = nullptr;
    if (bytes.empty() || FAILED(CreateStreamOnHGlobal(globalFrom(bytes.data(), bytes.size()), TRUE, &stream))) {
        LOG_WARN("drag: BRO_DRAG_TEST_TARGET '%s' holds no marshaled drop target", path);
        return false;
    }
    IDropTarget* target = nullptr;
    const HRESULT um = CoUnmarshalInterface(stream, IID_IDropTarget, reinterpret_cast<void**>(&target));
    stream->Release();
    if (FAILED(um) || !target) {
        LOG_WARN("drag: BRO_DRAG_TEST_TARGET unmarshal failed (0x%08lx)", static_cast<unsigned long>(um));
        return false;
    }
    POINTL pt{0, 0};
    DWORD e = allowed;
    target->DragEnter(data, MK_LBUTTON, pt, &e);
    e = allowed;
    if (SUCCEEDED(target->DragOver(MK_LBUTTON, pt, &e)) && (e & allowed)) {
        e = allowed;
        target->Drop(data, 0, pt, &e);
        hr = DRAGDROP_S_DROP;
        effect = e & allowed;
    } else {
        target->DragLeave();
        hr = DRAGDROP_S_CANCEL;
        effect = DROPEFFECT_NONE;
    }
    target->Release();
    return true;
}

}  // namespace

bool sdlNativeDragSupported() { return true; }

bool sdlNativeDragBegin(SDL_Window* window, uint32_t windowId, const DragSource& drag) {
    HWND hwnd = static_cast<HWND>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!hwnd) return false;
    // SDL initialized OLE on this thread for its drop targets; this only
    // takes another reference (or initializes it, if SDL fell back).
    const HRESULT ole = OleInitialize(nullptr);
    if (FAILED(ole)) {
        LOG_WARN("drag: OleInitialize failed (0x%08lx)", static_cast<unsigned long>(ole));
        return false;
    }

    auto* data = new DataObject();
    fillData(*data, drag);
    setDragImage(*data, drag);
    auto* source = new DropSource(hwnd, windowId);

    DWORD allowed = 0;
    if (drag.allowCopy) allowed |= DROPEFFECT_COPY;
    if (drag.allowMove) allowed |= DROPEFFECT_MOVE;
    if (!allowed) allowed = DROPEFFECT_COPY;

    DWORD effect = DROPEFFECT_NONE;
    HRESULT hr = S_OK;
    if (!dragToTestTarget(data, allowed, hr, effect)) hr = DoDragDrop(data, source, allowed, &effect);

    std::string action = "none";
    if (hr == DRAGDROP_S_DROP) {
        if (effect == DROPEFFECT_NONE) {
            // An optimized move (Explorer moving the file itself) answers
            // NONE and says what it did through the data object.
            long performed = data->dword(registered("Logical Performed DropEffect"));
            if (performed < 0) performed = data->dword(registered("Performed DropEffect"));
            if (performed > 0) effect = static_cast<DWORD>(performed);
        }
        action = actionName(effect);
    }
    const bool own = source->droppedOnOwnWindow();
    const float ox = source->ownX(), oy = source->ownY();
    source->Release();
    data->Release();
    OleUninitialize();

    // OLE swallowed the release: tell SDL its capture changed, which makes
    // it read the buttons again and queue the release, and drop that and
    // whatever SDL's own drop target queued while the drag passed over us.
    sdlDragFinished(SDL_BUTTON_LEFT);
    SendMessageW(hwnd, WM_CAPTURECHANGED, 0, 0);
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_EVENT_DROP_FILE, SDL_EVENT_DROP_POSITION);

    if (own) {
        DragReport drop;
        drop.kind = DragReport::Kind::Drop;
        drop.windowId = windowId;
        drop.x = ox;
        drop.y = oy;
        sdlDragReport(std::move(drop));
    }
    DragReport end;
    end.action = action;
    sdlDragReport(std::move(end));
    return true;
}

}  // namespace bro::platform
