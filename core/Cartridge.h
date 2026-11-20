#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// Тип контроллера банков (MBC), определяется по байту 0x147 заголовка
enum class Mbc { None, Mbc1, Mbc2, Mbc3, Mbc5, Unknown };

class Cartridge {
public:
    // Читает ROM и, если есть батарейка, файл сохранения .sav рядом с ним.
    // std::filesystem::path позволяет открывать файлы с любыми символами в пути (в том числе кириллицей на Windows).
    bool load(const std::filesystem::path& path);
    bool loadFromMemory(std::vector<uint8_t> rom);      // то же без файла (используется в тестах)

    // Чтение и запись по адресам картриджа: 0000-7FFF (ROM и регистры MBC) и A000-BFFF (RAM / часы)
    uint8_t read(uint16_t addr) const;
    void write(uint16_t addr, uint8_t value);

    bool saveRam() const;   // записывает RAM (и часы MBC3) в .sav; ничего не делает без батарейки

    const std::string& title() const { return title_; }
    std::string typeName() const;
    Mbc mbc() const { return mbc_; }
    bool supported() const { return mbc_ != Mbc::Unknown && mbc_ != Mbc::Mbc2; }
    bool hasBattery() const { return battery_; }
    bool cgbSupported() const { return cgbFlag_ == 0x80 || cgbFlag_ == 0xC0; }
    bool headerChecksumOk() const { return checksumOk_; }
    std::size_t romSize() const { return rom_.size(); }
    std::size_t ramSize() const { return ram_.size(); }

    // Источник времени для часов MBC3 в секундах (по умолчанию системные часы); подменяется в тестах
    std::function<int64_t()> timeSource;

private:
    std::size_t romOffset(unsigned bank, uint16_t addr) const;
    int64_t now() const;
    void updateRtc();

    std::vector<uint8_t> rom_;
    std::vector<uint8_t> ram_;
    std::string title_;
    std::filesystem::path savePath_;
    Mbc mbc_ = Mbc::None;
    bool battery_ = false, hasRtc_ = false;
    uint8_t type_ = 0, cgbFlag_ = 0;
    bool checksumOk_ = false;

    // Регистры контроллеров (набор зависит от типа MBC)
    bool ramEnable_ = false;
    unsigned romBankLo_ = 1;   // MBC1: 5 бит (0 -> 1); MBC3: 7 бит (0 -> 1); MBC5: младшие 8 бит
    unsigned romBankHi_ = 0;   // MBC1: 2 бита (банк RAM или старшие биты ROM); MBC5: бит 8
    unsigned ramBank_ = 0;     // MBC3 / MBC5: номер банка RAM, у MBC3 значения 8-0C выбирают регистр часов
    bool mode1_ = false;       // MBC1: режим 0 (простой) или 1 (расширенный)

    // Часы реального времени MBC3: секунды, минуты, часы, младшие 8 бит дней, старший бит дней + HALT + перенос
    uint8_t rtc_[5] = {0, 0, 0, 0, 0}, rtcLatched_[5] = {0, 0, 0, 0, 0};
    int64_t rtcLastTick_ = 0;
    uint8_t latchPrev_ = 0xFF;
};
