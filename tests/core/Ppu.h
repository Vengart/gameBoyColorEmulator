#pragma once
#include <array>
#include <cstdint>
#include "Clock.h"

namespace gb {

constexpr int kScreenW = 160;
constexpr int kScreenH = 144;

// Видеоконтроллер. Не знает ни про SDL, ни про шину: получает время через tick()
// и отдаёт готовый кадр в формате ARGB8888 (0xAARRGGBB).
class Ppu {
public:
    using Frame = std::array<uint32_t, kScreenW * kScreenH>;

    // cgbMode передаётся по ссылке, чтобы PPU всегда видел актуальный режим шины
    explicit Ppu(const bool& cgbMode) : cgb_(cgbMode) { reset(); }
    void reset();

    // Продвигает PPU на заданное число точек (dots).
    // Возвращает маску событий: бит 0 = VBlank, бит 1 = STAT, бит 2 = начало HBlank на видимой строке (для HDMA).
    uint8_t tick(int dots);

    // Видеопамять (0x8000-0x9FFF, банк выбирается регистром VBK) и OAM (0xFE00-0xFE9F) глазами процессора.
    // Пока PPU занят, доступ закрыт: VRAM недоступна в режиме 3, OAM в режимах 2 и 3.
    // Чтение закрытой области даёт 0xFF, запись игнорируется. При выключенном экране всё открыто.
    bool vramBlocked() const { return lcdOn() && mode_ == 3; }
    bool oamBlocked() const { return lcdOn() && (mode_ == 2 || mode_ == 3); }
    uint8_t readVram(uint16_t addr) const { return vramBlocked() ? 0xFF : vram_[vramBank_ * 0x2000 + (addr - 0x8000)]; }
    void writeVram(uint16_t addr, uint8_t v) { if (!vramBlocked()) vram_[vramBank_ * 0x2000 + (addr - 0x8000)] = v; }
    uint8_t readOam(uint16_t addr) const { return oamBlocked() ? 0xFF : oam_[addr - 0xFE00]; }
    void writeOam(uint16_t addr, uint8_t v) { if (!oamBlocked()) oam_[addr - 0xFE00] = v; }

    // Запись в обход блокировки: так пишут DMA и HDMA, а не процессор
    void pokeVram(uint16_t addr, uint8_t v) { vram_[vramBank_ * 0x2000 + (addr - 0x8000)] = v; }
    void pokeOam(uint16_t addr, uint8_t v) { oam_[addr - 0xFE00] = v; }

    // Регистры: 0xFF40-0xFF45, 0xFF47-0xFF4B, 0xFF4F (VBK), 0xFF68-0xFF6B (палитры CGB).
    // 0xFF46 (OAM DMA) обрабатывает шина, потому что ей нужен доступ ко всей памяти.
    static bool ownsReg(uint16_t addr) {
        return (addr >= 0xFF40 && addr <= 0xFF4B && addr != 0xFF46) || addr == 0xFF4F ||
               (addr >= 0xFF68 && addr <= 0xFF6B);
    }
    uint8_t readReg(uint16_t addr) const;
    void writeReg(uint16_t addr, uint8_t v);

    // Последний полностью отрисованный кадр (обновляется при входе в VBlank)
    const Frame& frame() const { return presented_; }
    uint64_t frameCount() const { return frameCount_; }

    // Для отладки и тестов
    int mode() const { return lcdOn() ? mode_ : 0; }
    int line() const { return ly_; }
    bool lcdOn() const { return (lcdc_ & 0x80) != 0; }

private:
    const bool& cgb_;

    std::array<uint8_t, 2 * 0x2000> vram_{};   // 2 банка по 8 КБ
    std::array<uint8_t, 0xA0> oam_{};          // 40 спрайтов по 4 байта
    std::array<uint8_t, 64> bgPal_{};          // 8 палитр по 4 цвета RGB555 (CGB)
    std::array<uint8_t, 64> objPal_{};

    uint8_t lcdc_ = 0, stat_ = 0, scy_ = 0, scx_ = 0, ly_ = 0, lyc_ = 0;
    uint8_t bgp_ = 0, obp0_ = 0, obp1_ = 0, wy_ = 0, wx_ = 0;
    uint8_t vramBank_ = 0, bcps_ = 0, ocps_ = 0;

    int dot_ = 0;          // позиция внутри строки, 0..455
    int mode_ = 2;         // 0 HBlank, 1 VBlank, 2 поиск спрайтов, 3 отрисовка
    int windowLine_ = 0;   // внутренний счётчик строк окна
    bool lycEqual_ = false;
    bool statLine_ = false;
    uint64_t frameCount_ = 0;

    Frame frame_{};        // кадр, который сейчас рисуется
    Frame presented_{};    // последний завершённый кадр

    void renderLine();
    void turnOff();
    void turnOn();
    uint32_t blankColor() const;
};

}  // namespace gb
