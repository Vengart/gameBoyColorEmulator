#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include "Apu.h"
#include "Cartridge.h"
#include "Clock.h"
#include "Ppu.h"

namespace gb {

// Кнопки: бит установлен, пока кнопка нажата
enum Button : uint8_t {
    kRight = 1 << 0, kLeft = 1 << 1, kUp = 1 << 2, kDown = 1 << 3,
    kA = 1 << 4, kB = 1 << 5, kSelect = 1 << 6, kStart = 1 << 7,
};

// Общая шина: единая точка доступа к памяти и регистрам устройств.
// Она же ведёт счёт времени: каждое обращение процессора к памяти
// занимает один M-такт, и шина продвигает на него остальные блоки.
class Bus {
public:
    Bus() : ppu_(cgbMode), apu_(cgbMode) {}

    void attach(Cartridge* cart) { cart_ = cart; }

    uint8_t read(uint16_t addr) const;
    void write(uint16_t addr, uint8_t value);

    // Один машинный такт (4 T-такта процессора): продвигает таймер, OAM DMA и PPU
    void tickM();

    // HDMA (CGB). Процессор вызывает перед каждой инструкцией: если передача ожидает, она выполняется целиком
    // (общая за один раз, HBlank-передача по 16 байт), а процессор на это время остановлен.
    void serviceHdma();

    bool oamDmaActive() const { return dmaActive_; }
    bool hdmaActive() const { return hdmaActive_; }

    // Приёмник символов последовательного порта (для тестов Blargg)
    std::function<void(char)> onSerial;

    uint64_t cpuCycles() const { return cpuCycles_; }  // T-такты процессора
    uint64_t ppuDots() const { return ppuDots_; }      // точки PPU (кадр = 70224)
    bool doubleSpeed() const { return doubleSpeed_; }

    // Режим CGB: если false, банки VRAM и WRAM не переключаются, палитры работают как на DMG.
    // Менять до запуска эмуляции.
    bool cgbMode = true;

    Ppu& ppu() { return ppu_; }
    const Ppu& ppu() const { return ppu_; }
    Apu& apu() { return apu_; }
    const Apu& apu() const { return apu_; }

    // Ввод: маска нажатых кнопок (см. Button). Запрашивает прерывание Joypad при нажатии.
    void setButtons(uint8_t pressed);

    // Прерывания: биты 0-4 = VBlank, STAT, Timer, Serial, Joypad
    uint8_t pendingInterrupts() const { return ie_ & io_[0x0F] & 0x1F; }
    void requestInterrupt(int bit) { io_[0x0F] |= static_cast<uint8_t>(1 << bit); }
    void acknowledgeInterrupt(int bit) { io_[0x0F] &= static_cast<uint8_t>(~(1 << bit)); }

private:
    Cartridge* cart_ = nullptr;
    Ppu ppu_;  // объявлен после cgbMode: PPU держит на него ссылку
    Apu apu_;  // то же самое

    std::array<uint8_t, 8 * 0x1000> wram_{};   // 8 банков по 4 КБ
    std::array<uint8_t, 0x80> io_{};
    std::array<uint8_t, 0x7F> hram_{};
    uint8_t ie_ = 0;

    uint8_t wramBank_ = 1;  // регистр SVBK (FF70), значение 1-7
    bool doubleSpeed_ = false;
    bool speedSwitchArmed_ = false;  // KEY1 бит 0

    uint8_t joypadSelect_ = 0x30;  // биты 4-5 регистра FF00
    uint8_t buttons_ = 0;
    uint8_t joypadLines() const;   // младшие 4 бита FF00 (0 = нажата)

    // Таймер: DIV это старший байт 16-битного счётчика,
    // TIMA растёт по спаду выбранного бита счётчика (при включённом таймере)
    uint16_t sysCounter_ = 0xABCC;
    bool timerSignal() const;
    // Бит счётчика, по спаду которого шагает секвенсор звука: бит 4 DIV (бит 5 в двойной скорости), 512 Гц
    uint16_t apuDivBit() const { return doubleSpeed_ ? 0x2000 : 0x1000; }
    void timerEdge(bool oldSignal);
    void incrementTima();

    uint64_t cpuCycles_ = 0;
    uint64_t ppuDots_ = 0;

    uint8_t rawRead(uint16_t addr) const;   // чтение без ограничений OAM DMA (так читают сами DMA и HDMA)
    uint8_t readIo(uint16_t addr) const;
    void writeIo(uint16_t addr, uint8_t value);

    // OAM DMA: запись в FF46, один такт задержки, затем 160 тактов по одному байту.
    // Пока он идёт, процессор видит только HRAM и регистры (адреса от FF00).
    bool dmaActive_ = false;
    uint16_t dmaSrc_ = 0, dmaPendingSrc_ = 0;
    int dmaIdx_ = 0, dmaDelay_ = 0;
    void tickOamDma();

    // HDMA: FF51/FF52 источник, FF53/FF54 назначение (в VRAM), FF55 длина и запуск
    uint16_t hdmaSrc_ = 0, hdmaDst_ = 0;
    int hdmaRemaining_ = 0;        // оставшиеся блоки по 16 байт
    bool hdmaActive_ = false, hdmaGeneral_ = false, hdmaReady_ = false;
    void writeHdmaControl(uint8_t value);
    void copyHdmaBlock();

    friend class Cpu;
    void toggleSpeedIfArmed();  // вызывается инструкцией STOP
};

}  // namespace gb
