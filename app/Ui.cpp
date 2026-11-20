#include "Ui.h"

std::u32string decodeUtf8(const std::string& s) {
    std::u32string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int len = 0;
        char32_t cp = 0;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { out.push_back(0xFFFD); ++i; continue; }
        if (i + len > s.size()) { out.push_back(0xFFFD); break; }
        bool ok = true;
        for (int k = 1; k < len; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { out.push_back(0xFFFD); ++i; continue; }
        out.push_back(cp);
        i += len;
    }
    return out;
}

std::string encodeUtf8(const std::u32string& s) {
    std::string out;
    for (char32_t cp : s) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

bool Ui::init(SDL_Renderer* renderer, int fontPx) {
    r_ = renderer;
    raster_ = std::make_unique<platform::GlyphRasterizer>(fontPx);
    cellH_ = raster_->cellHeight();
    return true;
}

void Ui::dropTextures() {
    for (auto& kv : cache_) if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
    cache_.clear();
}

void Ui::shutdown() {
    dropTextures();
    raster_.reset();
    r_ = nullptr;
}

const Ui::Glyph& Ui::glyph(char32_t cp) {
    auto it = cache_.find(cp);
    if (it != cache_.end()) return it->second;

    Glyph g;
    const platform::GlyphBitmap bm = raster_->render(cp);
    g.w = bm.w; g.h = bm.h; g.advance = bm.advance;
    bool any = false;
    for (uint8_t a : bm.alpha) if (a) { any = true; break; }
    if (any && bm.w > 0 && bm.h > 0) {
        std::vector<uint32_t> px(bm.alpha.size());
        for (size_t i = 0; i < px.size(); ++i) px[i] = bm.alpha[i] ? 0xFFFFFFFFu : 0x00FFFFFFu;
        g.tex = SDL_CreateTexture(r_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, bm.w, bm.h);
        if (g.tex) {
            SDL_UpdateTexture(g.tex, nullptr, px.data(), bm.w * 4);
            SDL_SetTextureBlendMode(g.tex, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(g.tex, SDL_ScaleModeNearest);
        }
    }
    return cache_.emplace(cp, g).first->second;
}

void Ui::fill(int x, int y, int w, int h, Color c) {
    if (w <= 0 || h <= 0) return;
    SDL_SetRenderDrawBlendMode(r_, c.a == 255 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r_, c.r, c.g, c.b, c.a);
    const SDL_Rect rc{x, y, w, h};
    SDL_RenderFillRect(r_, &rc);
}

void Ui::frame(int x, int y, int w, int h, Color c, int t) {
    fill(x, y, w, t, c);
    fill(x, y + h - t, w, t, c);
    fill(x, y + t, t, h - 2 * t, c);
    fill(x + w - t, y + t, t, h - 2 * t, c);
}

void Ui::box(int x, int y, int w, int h, Color fillColor, Color border, int shadow) {
    if (shadow > 0) fill(x + shadow, y + shadow, w, h, pal::kShadow);
    fill(x, y, w, h, fillColor);
    frame(x, y, w, h, border, 2);
}

int Ui::width(const std::u32string& s, int scale) {
    int w = 0;
    for (char32_t cp : s) w += glyph(cp).advance;
    return w * scale;
}

int Ui::textWidth(const std::string& utf8, int scale) { return width(decodeUtf8(utf8), scale); }

void Ui::text(int x, int y, const std::string& utf8, Color c, int scale) {
    int pen = x;
    for (char32_t cp : decodeUtf8(utf8)) {
        const Glyph& g = glyph(cp);
        if (g.tex) {
            SDL_SetTextureColorMod(g.tex, c.r, c.g, c.b);
            SDL_SetTextureAlphaMod(g.tex, c.a);
            const SDL_Rect dst{pen, y, g.w * scale, g.h * scale};
            SDL_RenderCopy(r_, g.tex, nullptr, &dst);
        }
        pen += g.advance * scale;
    }
}

void Ui::textCentered(int cx, int y, const std::string& utf8, Color c, int scale) {
    text(cx - textWidth(utf8, scale) / 2, y, utf8, c, scale);
}

std::string Ui::fit(const std::string& utf8, int maxW, bool keepEnd) {
    std::u32string s = decodeUtf8(utf8);
    if (width(s, 1) <= maxW) return utf8;
    const int ell = glyph(U'…').advance;
    while (!s.empty() && width(s, 1) + ell > maxW) {
        if (keepEnd) s.erase(s.begin());
        else s.pop_back();
    }
    return keepEnd ? "…" + encodeUtf8(s) : encodeUtf8(s) + "…";
}

std::vector<std::string> Ui::wrap(const std::string& utf8, int maxW, int maxLines) {
    std::vector<std::u32string> lines;
    std::u32string cur;
    const std::u32string all = decodeUtf8(utf8);
    size_t i = 0;
    while (i < all.size()) {
        while (i < all.size() && all[i] == U' ') ++i;
        size_t j = i;
        while (j < all.size() && all[j] != U' ') ++j;
        if (j == i) break;
        std::u32string word = all.substr(i, j - i);
        i = j;

        const std::u32string candidate = cur.empty() ? word : cur + U' ' + word;
        if (width(candidate, 1) <= maxW) { cur = candidate; continue; }
        if (!cur.empty()) { lines.push_back(cur); cur.clear(); }
        while (word.size() > 1 && width(word, 1) > maxW) {  // слово длиннее строки: режем по символам
            size_t n = 1;
            while (n < word.size() && width(word.substr(0, n + 1), 1) <= maxW) ++n;
            lines.push_back(word.substr(0, n));
            word.erase(0, n);
        }
        cur = word;
    }
    if (!cur.empty()) lines.push_back(cur);

    std::vector<std::string> out;
    if (maxLines <= 0) return out;
    if (static_cast<int>(lines.size()) > maxLines) {
        std::u32string rest;
        for (size_t k = static_cast<size_t>(maxLines) - 1; k < lines.size(); ++k) {
            if (!rest.empty()) rest += U' ';
            rest += lines[k];
        }
        lines.resize(static_cast<size_t>(maxLines));
        lines.back() = rest + U'\x01';  // метка: строку надо обрезать с многоточием
    }
    for (auto& l : lines) {
        if (!l.empty() && l.back() == U'\x01') {
            l.pop_back();
            // гарантированно ставим "…": текст продолжается
            const int ell = glyph(U'…').advance;
            while (!l.empty() && width(l, 1) + ell > maxW) l.pop_back();
            out.push_back(encodeUtf8(l) + "…");
        } else {
            out.push_back(encodeUtf8(l));
        }
    }
    return out;
}
