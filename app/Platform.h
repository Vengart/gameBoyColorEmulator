#pragma once
// Всё, что зависит от операционной системы: пути, аргументы командной строки,
// системный диалог выбора папки и растеризация шрифта.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct SDL_Window;

namespace platform {

namespace fs = std::filesystem;

// Папка, в которой лежит exe
fs::path exeDir();

// Аргументы командной строки без имени программы. На Windows читаются в Юникоде,
// поэтому пути с кириллицей не портятся.
std::vector<fs::path> commandLineArgs(int argc, char** argv);

// Системный диалог выбора папки. Пустой результат, если пользователь отменил выбор.
std::optional<fs::path> pickFolder(SDL_Window* parent, const fs::path& initial);

// Преобразования путей в UTF-8 и обратно (весь интерфейс работает с UTF-8)
std::string toUtf8(const fs::path& p);
fs::path fromUtf8(const std::string& s);

// Один символ шрифта: маска (0 или 255) размером w x h и ширина шага
struct GlyphBitmap {
    int w = 0, h = 0, advance = 0;
    std::vector<uint8_t> alpha;
};

// Растеризатор моноширинного шрифта без сглаживания (чёткие пиксели в ретро-стиле)
class GlyphRasterizer {
public:
    explicit GlyphRasterizer(int pixelHeight);
    ~GlyphRasterizer();
    GlyphRasterizer(const GlyphRasterizer&) = delete;
    GlyphRasterizer& operator=(const GlyphRasterizer&) = delete;

    int cellHeight() const { return cellHeight_; }
    GlyphBitmap render(char32_t codepoint);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int pixelHeight_;
    int cellHeight_;
};

}  // namespace platform
