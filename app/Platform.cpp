#include "Platform.h"

#include <SDL.h>
#include <algorithm>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <SDL_syswm.h>
#endif

namespace platform {

std::string toUtf8(const fs::path& p) { return p.u8string(); }

fs::path fromUtf8(const std::string& s) { return fs::u8path(s); }

fs::path exeDir() {
    fs::path dir;
    if (char* base = SDL_GetBasePath()) {  // UTF-8, с завершающим разделителем
        dir = fromUtf8(base);
        SDL_free(base);
    }
    if (dir.empty()) {
        std::error_code ec;
        dir = fs::current_path(ec);
    }
    return dir;
}

std::vector<fs::path> commandLineArgs(int argc, char** argv) {
    std::vector<fs::path> args;
#ifdef _WIN32
    (void)argc; (void)argv;
    int n = 0;
    if (LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n)) {
        for (int i = 1; i < n; ++i) args.emplace_back(w[i]);
        LocalFree(w);
    }
#else
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
#endif
    return args;
}

std::optional<fs::path> pickFolder(SDL_Window* parent, const fs::path& initial) {
#ifdef _WIN32
    HWND hwnd = nullptr;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (parent && SDL_GetWindowWMInfo(parent, &info)) hwnd = info.info.win.window;

    std::optional<fs::path> result;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        if (SUCCEEDED(dlg->GetOptions(&opts)))
            dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dlg->SetTitle(L"Выберите папку с ромами (.gb / .gbc)");
        std::error_code ec;
        if (!initial.empty() && fs::is_directory(initial, ec)) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(initial.wstring().c_str(), nullptr, IID_PPV_ARGS(&item)))) {
                dlg->SetFolder(item);
                item->Release();
            }
        }
        if (SUCCEEDED(dlg->Show(hwnd))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    result = fs::path(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return result;
#else
    (void)parent; (void)initial;
    return std::nullopt;
#endif
}

// ---------- шрифт ----------

#ifdef _WIN32

struct GlyphRasterizer::Impl {
    HDC dc = nullptr;
    HFONT font = nullptr;
    HGDIOBJ oldFont = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ oldBmp = nullptr;
    uint32_t* bits = nullptr;
    int bw = 0, bh = 0;
    TEXTMETRICW tm{};
};

GlyphRasterizer::GlyphRasterizer(int pixelHeight) : impl_(std::make_unique<Impl>()), pixelHeight_(pixelHeight), cellHeight_(pixelHeight) {
    Impl& m = *impl_;
    m.dc = CreateCompatibleDC(nullptr);
    if (!m.dc) return;
    // Consolas есть во всех Windows начиная с Vista, содержит латиницу и кириллицу.
    // NONANTIALIASED_QUALITY даёт чёткие пиксельные контуры без сглаживания.
    m.font = CreateFontW(-pixelHeight, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                         CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (m.font) m.oldFont = SelectObject(m.dc, m.font);
    GetTextMetricsW(m.dc, &m.tm);
    cellHeight_ = m.tm.tmHeight > 0 ? static_cast<int>(m.tm.tmHeight) : pixelHeight;

    m.bw = std::max<int>(m.tm.tmMaxCharWidth, pixelHeight) * 2 + 4;
    m.bh = cellHeight_ + 2;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = m.bw;
    bi.bmiHeader.biHeight = -m.bh;  // строки сверху вниз
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    m.bmp = CreateDIBSection(m.dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!m.bmp) return;
    m.bits = static_cast<uint32_t*>(bits);
    m.oldBmp = SelectObject(m.dc, m.bmp);
    SetTextColor(m.dc, RGB(255, 255, 255));
    SetBkColor(m.dc, RGB(0, 0, 0));
    SetBkMode(m.dc, TRANSPARENT);
    SetTextAlign(m.dc, TA_TOP | TA_LEFT | TA_NOUPDATECP);
}

GlyphRasterizer::~GlyphRasterizer() {
    Impl& m = *impl_;
    if (m.dc) {
        if (m.oldBmp) SelectObject(m.dc, m.oldBmp);
        if (m.oldFont) SelectObject(m.dc, m.oldFont);
        DeleteDC(m.dc);
    }
    if (m.bmp) DeleteObject(m.bmp);
    if (m.font) DeleteObject(m.font);
}

GlyphBitmap GlyphRasterizer::render(char32_t cp) {
    Impl& m = *impl_;
    GlyphBitmap g;
    g.h = cellHeight_;
    g.advance = std::max(1, pixelHeight_ / 2);
    if (!m.dc || !m.bits) { g.w = g.advance; g.alpha.assign(static_cast<size_t>(g.w) * g.h, 0); return g; }

    wchar_t buf[2];
    int n = 1;
    if (cp >= 0x10000 && cp <= 0x10FFFF) {
        const char32_t v = cp - 0x10000;
        buf[0] = static_cast<wchar_t>(0xD800 + (v >> 10));
        buf[1] = static_cast<wchar_t>(0xDC00 + (v & 0x3FF));
        n = 2;
    } else {
        buf[0] = static_cast<wchar_t>(cp > 0xFFFF ? 0xFFFD : cp);
    }

    SIZE sz{};
    GetTextExtentPoint32W(m.dc, buf, n, &sz);
    g.advance = std::max<int>(1, std::min<int>(sz.cx, m.bw));
    g.w = g.advance;

    std::memset(m.bits, 0, static_cast<size_t>(m.bw) * m.bh * 4);
    TextOutW(m.dc, 0, 0, buf, n);
    GdiFlush();

    g.alpha.assign(static_cast<size_t>(g.w) * g.h, 0);
    for (int y = 0; y < g.h && y < m.bh; ++y)
        for (int x = 0; x < g.w; ++x)
            if ((m.bits[y * m.bw + x] & 0xFF) > 127) g.alpha[static_cast<size_t>(y) * g.w + x] = 255;
    return g;
}

#else  // не Windows: пустые символы (интерфейс работает, но без текста)

struct GlyphRasterizer::Impl {};

GlyphRasterizer::GlyphRasterizer(int pixelHeight) : impl_(std::make_unique<Impl>()), pixelHeight_(pixelHeight), cellHeight_(pixelHeight) {}
GlyphRasterizer::~GlyphRasterizer() = default;

GlyphBitmap GlyphRasterizer::render(char32_t) {
    GlyphBitmap g;
    g.h = cellHeight_;
    g.w = g.advance = std::max(1, pixelHeight_ / 2);
    g.alpha.assign(static_cast<size_t>(g.w) * g.h, 0);
    return g;
}

#endif

}  // namespace platform
