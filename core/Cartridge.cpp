#include "Cartridge.h"
#include <ctime>
#include <fstream>
#include <iterator>

// ---------- загрузка ----------

bool Cartridge::load(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (!loadFromMemory(std::move(data))) return false;

    // Файл сохранения лежит рядом с ROM: game.gb -> game.sav
    savePath_ = path;
    savePath_.replace_extension(".sav");
    if (battery_) {
        std::ifstream s(savePath_, std::ios::binary);
        if (s) {
            std::vector<uint8_t> save((std::istreambuf_iterator<char>(s)), std::istreambuf_iterator<char>());
            for (std::size_t i = 0; i < ram_.size() && i < save.size(); ++i) ram_[i] = save[i];
            if (hasRtc_ && save.size() >= ram_.size() + 13) {   // 5 регистров + 8 байт времени сохранения
                for (int i = 0; i < 5; ++i) rtc_[i] = save[ram_.size() + i];
                int64_t t = 0;
                for (int i = 7; i >= 0; --i) t = (t << 8) | save[ram_.size() + 5 + i];
                rtcLastTick_ = t;   // пока игра была закрыта, часы "шли": updateRtc() добавит прошедшее время
                updateRtc();
                for (int i = 0; i < 5; ++i) rtcLatched_[i] = rtc_[i];
            }
        }
    }
    return true;
}

bool Cartridge::loadFromMemory(std::vector<uint8_t> rom) {
    rom_ = std::move(rom);
    if (rom_.size() < 0x150) return false;
    if (rom_.size() < 0x8000) rom_.resize(0x8000, 0xFF);

    title_.clear();
    for (int i = 0x134; i <= 0x142 && rom_[i] != 0; ++i) title_ += static_cast<char>(rom_[i]);
    cgbFlag_ = rom_[0x143];
    type_ = rom_[0x147];

    mbc_ = Mbc::Unknown;
    battery_ = hasRtc_ = false;
    switch (type_) {
        case 0x00: case 0x08: case 0x09: mbc_ = Mbc::None; battery_ = type_ == 0x09; break;
        case 0x01: case 0x02: mbc_ = Mbc::Mbc1; break;
        case 0x03: mbc_ = Mbc::Mbc1; battery_ = true; break;
        case 0x05: mbc_ = Mbc::Mbc2; break;
        case 0x06: mbc_ = Mbc::Mbc2; battery_ = true; break;
        case 0x0F: mbc_ = Mbc::Mbc3; battery_ = hasRtc_ = true; break;   // таймер + батарейка
        case 0x10: mbc_ = Mbc::Mbc3; battery_ = hasRtc_ = true; break;   // таймер + RAM + батарейка
        case 0x11: case 0x12: mbc_ = Mbc::Mbc3; break;
        case 0x13: mbc_ = Mbc::Mbc3; battery_ = true; break;
        case 0x19: case 0x1A: case 0x1C: case 0x1D: mbc_ = Mbc::Mbc5; break;
        case 0x1B: case 0x1E: mbc_ = Mbc::Mbc5; battery_ = true; break;
        default: break;
    }

    std::size_t ramSize = 0;
    switch (rom_[0x149]) {
        case 0x01: ramSize = 2 * 1024; break;
        case 0x02: ramSize = 8 * 1024; break;
        case 0x03: ramSize = 32 * 1024; break;
        case 0x04: ramSize = 128 * 1024; break;
        case 0x05: ramSize = 64 * 1024; break;
        default: break;
    }
    ram_.assign(ramSize, 0);

    ramEnable_ = mode1_ = false;
    romBankLo_ = 1; romBankHi_ = 0; ramBank_ = 0;
    for (int i = 0; i < 5; ++i) rtc_[i] = rtcLatched_[i] = 0;
    rtcLastTick_ = now();
    latchPrev_ = 0xFF;

    uint8_t sum = 0;
    for (int i = 0x134; i <= 0x14C; ++i) sum = static_cast<uint8_t>(sum - rom_[i] - 1);
    checksumOk_ = (sum == rom_[0x14D]);
    return true;
}

bool Cartridge::saveRam() const {
    if (!battery_ || savePath_.empty() || (ram_.empty() && !hasRtc_)) return false;
    std::ofstream f(savePath_, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(ram_.data()), static_cast<std::streamsize>(ram_.size()));
    if (hasRtc_) {
        f.write(reinterpret_cast<const char*>(rtc_), 5);
        int64_t t = rtcLastTick_;
        for (int i = 0; i < 8; ++i, t >>= 8) f.put(static_cast<char>(t & 0xFF));
    }
    return static_cast<bool>(f);
}

// ---------- ROM ----------

// Смещение в образе: номер банка по модулю числа банков (лишние старшие биты игнорируются, как в железе)
std::size_t Cartridge::romOffset(unsigned bank, uint16_t addr) const {
    const std::size_t banks = rom_.size() / 0x4000;
    return (bank % banks) * 0x4000 + (addr & 0x3FFF);
}

// ---------- часы MBC3 ----------

int64_t Cartridge::now() const { return timeSource ? timeSource() : static_cast<int64_t>(std::time(nullptr)); }

void Cartridge::updateRtc() {
    const int64_t t = now();
    if (rtc_[4] & 0x40) { rtcLastTick_ = t; return; }   // бит HALT: часы остановлены
    int64_t delta = t - rtcLastTick_;
    rtcLastTick_ = t;
    if (delta <= 0) return;
    int64_t days = rtc_[3] | ((rtc_[4] & 1) << 8);
    int64_t total = days * 86400 + rtc_[2] * 3600 + rtc_[1] * 60 + rtc_[0] + delta;
    days = total / 86400;
    if (days > 511) { days %= 512; rtc_[4] |= 0x80; }   // счётчик дней переполнился: бит переноса остаётся до сброса программой
    const int64_t rest = total % 86400;
    rtc_[0] = static_cast<uint8_t>(rest % 60);
    rtc_[1] = static_cast<uint8_t>(rest / 60 % 60);
    rtc_[2] = static_cast<uint8_t>(rest / 3600);
    rtc_[3] = static_cast<uint8_t>(days & 0xFF);
    rtc_[4] = static_cast<uint8_t>((rtc_[4] & 0xC0) | ((days >> 8) & 1));
}

// ---------- шина картриджа ----------

uint8_t Cartridge::read(uint16_t addr) const {
    if (addr < 0x4000) {
        // MBC1 в режиме 1 подставляет старшие биты и в нулевую область (нужно для ROM больше 512 КБ)
        const unsigned bank = (mbc_ == Mbc::Mbc1 && mode1_) ? (romBankHi_ << 5) : 0;
        return rom_[romOffset(bank, addr)];
    }
    if (addr < 0x8000) {
        unsigned bank = 1;
        switch (mbc_) {
            case Mbc::Mbc1: bank = (romBankHi_ << 5) | romBankLo_; break;
            case Mbc::Mbc3: bank = romBankLo_; break;
            case Mbc::Mbc5: bank = (romBankHi_ << 8) | romBankLo_; break;
            default: break;
        }
        if (mbc_ == Mbc::None) return rom_[addr];
        return rom_[romOffset(bank, addr)];
    }
    if (addr >= 0xA000 && addr < 0xC000) {
        if (!ramEnable_) return 0xFF;
        if (mbc_ == Mbc::Mbc3 && ramBank_ >= 0x08 && ramBank_ <= 0x0C)
            return hasRtc_ ? rtcLatched_[ramBank_ - 8] : 0xFF;
        if (ram_.empty()) return 0xFF;
        unsigned bank = 0;
        if (mbc_ == Mbc::Mbc1) bank = mode1_ ? romBankHi_ : 0;
        else if (mbc_ == Mbc::Mbc3 || mbc_ == Mbc::Mbc5) bank = ramBank_;
        return ram_[(bank * 0x2000 + (addr - 0xA000)) % ram_.size()];
    }
    return 0xFF;
}

void Cartridge::write(uint16_t addr, uint8_t v) {
    if (mbc_ == Mbc::None || mbc_ == Mbc::Unknown || mbc_ == Mbc::Mbc2) return;

    if (addr < 0x2000) {                    // 0000-1FFF: разрешение RAM (и часов)
        ramEnable_ = mbc_ == Mbc::Mbc5 ? v == 0x0A : (v & 0x0F) == 0x0A;
        return;
    }
    if (addr < 0x4000) {                    // 2000-3FFF: номер ROM-банка
        switch (mbc_) {
            case Mbc::Mbc1: romBankLo_ = v & 0x1F; if (romBankLo_ == 0) romBankLo_ = 1; break;
            case Mbc::Mbc3: romBankLo_ = v & 0x7F; if (romBankLo_ == 0) romBankLo_ = 1; break;
            case Mbc::Mbc5:
                if (addr < 0x3000) romBankLo_ = v;           // 2000-2FFF: младшие 8 бит (банк 0 разрешён)
                else romBankHi_ = v & 1;                     // 3000-3FFF: бит 8
                break;
            default: break;
        }
        return;
    }
    if (addr < 0x6000) {                    // 4000-5FFF: банк RAM / старшие биты / регистр часов
        switch (mbc_) {
            case Mbc::Mbc1: romBankHi_ = v & 0x03; break;
            case Mbc::Mbc3: ramBank_ = v; break;
            case Mbc::Mbc5: ramBank_ = v & 0x0F; break;
            default: break;
        }
        return;
    }
    if (addr < 0x8000) {                    // 6000-7FFF
        if (mbc_ == Mbc::Mbc1) mode1_ = v & 1;
        else if (mbc_ == Mbc::Mbc3) {       // защёлка часов: запись 0, затем 1
            if (latchPrev_ == 0 && v == 1 && hasRtc_) {
                updateRtc();
                for (int i = 0; i < 5; ++i) rtcLatched_[i] = rtc_[i];
            }
            latchPrev_ = v;
        }
        return;
    }
    if (addr >= 0xA000 && addr < 0xC000) {  // RAM картриджа или регистр часов
        if (!ramEnable_) return;
        if (mbc_ == Mbc::Mbc3 && ramBank_ >= 0x08 && ramBank_ <= 0x0C) {
            if (!hasRtc_) return;
            updateRtc();
            const unsigned r = ramBank_ - 8;
            static constexpr uint8_t masks[5] = {0x3F, 0x3F, 0x1F, 0xFF, 0xC1};
            rtc_[r] = v & masks[r];
            rtcLatched_[r] = rtc_[r];
            return;
        }
        if (ram_.empty()) return;
        unsigned bank = 0;
        if (mbc_ == Mbc::Mbc1) bank = mode1_ ? romBankHi_ : 0;
        else bank = ramBank_;
        ram_[(bank * 0x2000 + (addr - 0xA000)) % ram_.size()] = v;
    }
}

std::string Cartridge::typeName() const {
    std::string s;
    switch (mbc_) {
        case Mbc::None: s = "ROM only"; break;
        case Mbc::Mbc1: s = "MBC1"; break;
        case Mbc::Mbc2: s = "MBC2 (не поддерживается)"; break;
        case Mbc::Mbc3: s = "MBC3"; break;
        case Mbc::Mbc5: s = "MBC5"; break;
        default: return "неизвестный тип";
    }
    if (hasRtc_) s += " + часы";
    if (!ram_.empty()) s += " + RAM";
    if (battery_) s += " + батарейка";
    return s;
}
