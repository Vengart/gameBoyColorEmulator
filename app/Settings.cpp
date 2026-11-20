#include "Settings.h"
#include <fstream>
#include "Platform.h"

namespace {

const char* const kKeyNames[8] = {"key_right", "key_left", "key_up", "key_down", "key_a", "key_b", "key_select", "key_start"};

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

bool parseInt(const std::string& s, int& out) {
    if (s.empty() || s.size() > 9) return false;
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}

}  // namespace

bool Settings::load(const std::filesystem::path& file) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return false;
    std::string line;
    bool first = true;
    while (std::getline(f, line)) {
        if (first) {  // пропускаем BOM, если файл правили в Блокноте
            first = false;
            if (line.size() >= 3 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));

        if (key == "rom_dir") {
            romDir = value.empty() ? std::filesystem::path() : platform::fromUtf8(value);
            continue;
        }
        int v = 0;
        if (!parseInt(value, v)) continue;
        if (key == "volume") {
            volume = v > 100 ? 100 : v;
            continue;
        }
        for (int i = 0; i < 8; ++i) {
            if (key == kKeyNames[i] && v > SDL_SCANCODE_UNKNOWN && v < SDL_NUM_SCANCODES && v != SDL_SCANCODE_ESCAPE)
                keys[i] = static_cast<SDL_Scancode>(v);
        }
    }
    // Две кнопки на одной клавише (файл правили вручную): возвращаем раскладку по умолчанию
    for (int i = 0; i < 8; ++i)
        for (int j = i + 1; j < 8; ++j)
            if (keys[i] == keys[j]) { keys = defaultKeys(); return true; }
    return true;
}

bool Settings::save(const std::filesystem::path& file) const {
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << "# Настройки эмулятора GBC EMU\n";
    f << "rom_dir=" << platform::toUtf8(romDir) << "\n";
    f << "volume=" << volume << "\n";
    f << "# коды клавиш SDL (scancode)\n";
    for (int i = 0; i < 8; ++i) f << kKeyNames[i] << "=" << static_cast<int>(keys[i]) << "\n";
    f.flush();
    return static_cast<bool>(f);
}
