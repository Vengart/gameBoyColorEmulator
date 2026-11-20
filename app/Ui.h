#pragma once
// Простейшие примитивы рисования для интерфейса: прямоугольники, рамки и текст в UTF-8.
#include <SDL.h>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "Platform.h"

struct Color {
    uint8_t r, g, b, a = 255;
};

// Палитра в духе экрана оригинального Game Boy
namespace pal {
constexpr Color kBg{15, 56, 15};        // самый тёмный зелёный
constexpr Color kShadow{6, 30, 6};
constexpr Color kPanel{48, 98, 48};
constexpr Color kAccent{139, 172, 15};
constexpr Color kLight{202, 220, 159};  // текст
constexpr Color kDim{120, 150, 90};
constexpr Color kGbc{232, 106, 80};     // значок игр для Game Boy Color
constexpr Color kWarn{240, 200, 80};
}  // namespace pal

// Декодирует UTF-8 в кодовые точки; некорректные байты заменяются на U+FFFD
std::u32string decodeUtf8(const std::string& s);
std::string encodeUtf8(const std::u32string& s);

class Ui {
public:
    bool init(SDL_Renderer* renderer, int fontPx);
    void shutdown();
    void dropTextures();  // после потери устройства рендеринга: символы будут созданы заново

    void fill(int x, int y, int w, int h, Color c);
    void frame(int x, int y, int w, int h, Color c, int thickness = 2);
    // Кнопка/панель в ретро-стиле: тень со смещением, заливка, рамка
    void box(int x, int y, int w, int h, Color fillColor, Color border, int shadow = 4);

    void text(int x, int y, const std::string& utf8, Color c, int scale = 1);
    void textCentered(int cx, int y, const std::string& utf8, Color c, int scale = 1);
    int textWidth(const std::string& utf8, int scale = 1);
    int lineHeight(int scale = 1) const { return cellH_ * scale; }

    // Обрезает строку до ширины maxW, добавляя "…" (в конце или, для путей, в начале)
    std::string fit(const std::string& utf8, int maxW, bool keepEnd = false);
    // Разбивает на строки шириной maxW (по словам), не больше maxLines; последняя обрезается с "…"
    std::vector<std::string> wrap(const std::string& utf8, int maxW, int maxLines);

private:
    struct Glyph {
        SDL_Texture* tex = nullptr;
        int w = 0, h = 0, advance = 0;
    };
    const Glyph& glyph(char32_t cp);
    int width(const std::u32string& s, int scale);

    SDL_Renderer* r_ = nullptr;
    std::unique_ptr<platform::GlyphRasterizer> raster_;
    std::unordered_map<char32_t, Glyph> cache_;
    int cellH_ = 16;
};
