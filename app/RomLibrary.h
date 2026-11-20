#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Найденный в папке ROM и сведения из его заголовка
struct RomEntry {
    std::filesystem::path path;
    std::string title;     // название для карточки (из заголовка, иначе из имени файла)
    std::string fileName;  // имя файла в UTF-8
    std::string typeName;  // тип картриджа (MBC)
    uint8_t cgbFlag = 0;   // 0x80 = поддерживает GBC, 0xC0 = только GBC
    bool supported = true; // контроллер картриджа поддерживается эмулятором
    uintmax_t size = 0;
};

// Ищет файлы .gb и .gbc в папке и её подпапках (до 4 уровней), читает заголовки.
// Файлы, которые не похожи на ROM (меньше заголовка или слишком большие), пропускаются.
// Результат отсортирован по названию. error заполняется, если папку прочитать нельзя.
std::vector<RomEntry> scanRoms(const std::filesystem::path& dir, std::string& error);

// Читает заголовок одного файла; false, если это не ROM
bool readRomEntry(const std::filesystem::path& file, RomEntry& out);

bool hasRomExtension(const std::filesystem::path& file);
