#pragma once
#include <SDL.h>
#include <array>
#include <filesystem>
#include <string>

// Настройки, которые сохраняются между запусками в текстовом файле gbcemu.ini (UTF-8, строки ключ=значение)
struct Settings {
    // Клавиши для кнопок Game Boy; индекс = номер бита в gb::Button:
    // 0 Вправо, 1 Влево, 2 Вверх, 3 Вниз, 4 A, 5 B, 6 Select, 7 Start
    std::array<SDL_Scancode, 8> keys = defaultKeys();
    std::filesystem::path romDir;
    int volume = 80;  // 0..100

    static std::array<SDL_Scancode, 8> defaultKeys() {
        return {SDL_SCANCODE_RIGHT, SDL_SCANCODE_LEFT, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN,
                SDL_SCANCODE_Z,     SDL_SCANCODE_X,    SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_RETURN};
    }

    bool load(const std::filesystem::path& file);        // false, если файла нет
    bool save(const std::filesystem::path& file) const;  // false, если записать не удалось
};
