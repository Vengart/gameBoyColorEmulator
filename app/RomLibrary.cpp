#include "RomLibrary.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include "Cartridge.h"
#include "Platform.h"

namespace fs = std::filesystem;

namespace {

constexpr uintmax_t kHeaderEnd = 0x150;
constexpr uintmax_t kMaxRomSize = 16u * 1024 * 1024;  // самые большие картриджи MBC5 = 8 МБ
constexpr size_t kMaxEntries = 4000;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Название из заголовка: 0x134-0x143. У игр для GBC последние 4 символа часто занимает код производителя
// (например "POKEMON_SLVAAXE"), тогда название это первые 11 символов.
std::string headerTitle(const std::vector<uint8_t>& h) {
    std::string raw;
    for (int i = 0x134; i <= 0x142 && h[i] != 0; ++i) raw += static_cast<char>(h[i]);
    const uint8_t flag = h[0x143];
    if ((flag == 0x80 || flag == 0xC0) && raw.size() == 15) {
        bool code = true;
        for (int i = 11; i < 15; ++i) {
            const char c = raw[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) code = false;
        }
        if (code) raw.resize(11);
    }
    std::string out;
    for (char c : raw) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u > 0x7E) continue;  // только печатные символы ASCII
        out += c == '_' ? ' ' : c;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    return out;
}

// Название из имени файла: "Resident Evil Gaiden (Europe) (En,Fr).gbc" -> "Resident Evil Gaiden".
// Пометки в скобках (регион, языки, версия) убираются, подчёркивания становятся пробелами.
std::string cleanFileTitle(const std::string& stem) {
    std::string out;
    int depth = 0;
    for (char c : stem) {
        if (c == '(' || c == '[') { ++depth; continue; }
        if ((c == ')' || c == ']') && depth > 0) { --depth; continue; }
        if (depth > 0) continue;
        if (c == '_') c = ' ';
        if (c == ' ' && (out.empty() || out.back() == ' ')) continue;  // схлопываем пробелы
        out += c;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '-')) out.pop_back();
    return out;
}

}  // namespace

bool hasRomExtension(const fs::path& file) {
    const std::string ext = lower(platform::toUtf8(file.extension()));
    return ext == ".gb" || ext == ".gbc";
}

bool readRomEntry(const fs::path& file, RomEntry& out) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(file, ec);
    if (ec || size < kHeaderEnd || size > kMaxRomSize) return false;

    std::ifstream f(file, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> header(kHeaderEnd);
    f.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (f.gcount() != static_cast<std::streamsize>(header.size())) return false;

    Cartridge cart;  // разбор типа картриджа тем же кодом, что и при запуске
    if (!cart.loadFromMemory(header)) return false;

    out.path = file;
    out.size = size;
    out.fileName = platform::toUtf8(file.filename());
    out.cgbFlag = header[0x143];
    out.supported = cart.supported();
    out.typeName = cart.typeName();
    // Имя файла обычно полнее и читабельнее, чем 11-15 заглавных букв заголовка ("RES EVIL GD"),
    // поэтому заголовок ROM используется, только если из имени файла ничего не осталось
    out.title = cleanFileTitle(platform::toUtf8(file.stem()));
    if (out.title.size() < 2) out.title = headerTitle(header);
    if (out.title.size() < 2) out.title = platform::toUtf8(file.stem());
    return true;
}

std::vector<RomEntry> scanRoms(const fs::path& dir, std::string& error) {
    std::vector<RomEntry> roms;
    error.clear();
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) {
        error = "Папка не найдена";
        return roms;
    }

    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        error = "Нет доступа к папке";
        return roms;
    }
    size_t visited = 0;
    for (const fs::recursive_directory_iterator end; it != end; it.increment(ec)) {
        if (ec) { ec.clear(); break; }
        if (++visited > 50000) break;  // защита от случайно выбранного корня диска
        if (it.depth() >= 4) it.disable_recursion_pending();

        std::error_code fec;
        const fs::path& p = it->path();
        if (!it->is_regular_file(fec) || fec || !hasRomExtension(p)) continue;
        RomEntry e;
        if (readRomEntry(p, e)) roms.push_back(std::move(e));
        if (roms.size() >= kMaxEntries) break;
    }

    std::sort(roms.begin(), roms.end(), [](const RomEntry& a, const RomEntry& b) {
        const std::string la = lower(a.title), lb = lower(b.title);
        return la != lb ? la < lb : a.fileName < b.fileName;
    });
    return roms;
}
