#include "Bus.h"

namespace gb {

// ---------- таймер ----------

bool Bus::timerSignal() const {
    const uint8_t tac = io_[0x07];
    if (!(tac & 0x04)) return false;
    static constexpr uint16_t masks[4] = {1 << 9, 1 << 3, 1 << 5, 1 << 7};  // 4096, 262144, 65536, 16384 Гц
    return (sysCounter_ & masks[tac & 3]) != 0;
}

void Bus::incrementTima() {
    if (++io_[0x05] == 0) {          // переполнение: перезагрузка из TMA и прерывание Timer
        io_[0x05] = io_[0x06];
        requestInterrupt(2);
    }
}

void Bus::timerEdge(bool oldSignal) {
    if (oldSignal && !timerSignal()) incrementTima();
}

// ---------- время ----------

void Bus::tickM() {
    tickOamDma();
    for (int i = 0; i < 4; ++i) {
        const bool old = timerSignal();
        ++sysCounter_;
        timerEdge(old);
    }
    cpuCycles_ += kTCyclesPerM;

    // В режиме двойной скорости процессор вдвое быстрее, а PPU нет:
    // за один M-такт проходит 2 точки PPU вместо 4.
    const int dots = doubleSpeed_ ? kTCyclesPerM / 2 : kTCyclesPerM;
    ppuDots_ += dots;
    const uint8_t irq = ppu_.tick(dots);
    if (irq & 1) requestInterrupt(0);  // VBlank
    if (irq & 2) requestInterrupt(1);  // STAT
    if ((irq & 4) && hdmaActive_ && !hdmaGeneral_) hdmaReady_ = true;  // начался HBlank: пора слать блок HDMA
}

// ---------- карта памяти ----------

// Во время OAM DMA процессору доступны только регистры, HRAM и IE (адреса от 0xFF00); остальное он видит как 0xFF
uint8_t Bus::read(uint16_t addr) const {
    if (dmaActive_ && addr < 0xFF00) return 0xFF;
    return rawRead(addr);
}

uint8_t Bus::rawRead(uint16_t addr) const {
    if (addr < 0x8000) return cart_ ? cart_->read(addr) : 0xFF;                    // ROM
    if (addr < 0xA000) return ppu_.readVram(addr);                                 // VRAM
    if (addr < 0xC000) return cart_ ? cart_->read(addr) : 0xFF;                    // RAM картриджа
    if (addr < 0xD000) return wram_[addr - 0xC000];                                // WRAM банк 0
    if (addr < 0xE000) return wram_[wramBank_ * 0x1000 + (addr - 0xD000)];         // WRAM банк 1-7
    if (addr < 0xFE00) return rawRead(static_cast<uint16_t>(addr - 0x2000));       // Echo RAM
    if (addr < 0xFEA0) return ppu_.readOam(addr);                                  // OAM
    if (addr < 0xFF00) return 0xFF;                                                // не используется
    if (addr < 0xFF80) return readIo(addr);                                        // регистры
    if (addr < 0xFFFF) return hram_[addr - 0xFF80];                                // HRAM
    return ie_;                                                                    // IE
}

void Bus::write(uint16_t addr, uint8_t value) {
    if (dmaActive_ && addr < 0xFF00) return;
    if (addr < 0x8000) { if (cart_) cart_->write(addr, value); return; }
    if (addr < 0xA000) { ppu_.writeVram(addr, value); return; }
    if (addr < 0xC000) { if (cart_) cart_->write(addr, value); return; }
    if (addr < 0xD000) { wram_[addr - 0xC000] = value; return; }
    if (addr < 0xE000) { wram_[wramBank_ * 0x1000 + (addr - 0xD000)] = value; return; }
    if (addr < 0xFE00) { write(static_cast<uint16_t>(addr - 0x2000), value); return; }
    if (addr < 0xFEA0) { ppu_.writeOam(addr, value); return; }
    if (addr < 0xFF00) return;
    if (addr < 0xFF80) { writeIo(addr, value); return; }
    if (addr < 0xFFFF) { hram_[addr - 0xFF80] = value; return; }
    ie_ = value;
}

// ---------- джойстик ----------

// Регистр FF00 это матрица 2x4: игра выбирает группу (бит 4 = крестовина, бит 5 = кнопки), 0 = выбрана
uint8_t Bus::joypadLines() const {
    uint8_t lines = 0x0F;
    if (!(joypadSelect_ & 0x10)) lines &= static_cast<uint8_t>(~(buttons_ & 0x0F));
    if (!(joypadSelect_ & 0x20)) lines &= static_cast<uint8_t>(~((buttons_ >> 4) & 0x0F));
    return lines;
}

void Bus::setButtons(uint8_t pressed) {
    const uint8_t before = joypadLines();
    buttons_ = pressed;
    if (before & ~joypadLines()) requestInterrupt(4);  // какая-то линия перешла из 1 в 0
}

// ---------- OAM DMA ----------

// Запись в FF46 запускает передачу не сразу: следующий такт процессор ещё видит память свободной,
// затем 160 тактов подряд копируется по байту из source*0x100 в OAM.
void Bus::tickOamDma() {
    if (dmaActive_) {
        ppu_.pokeOam(static_cast<uint16_t>(0xFE00 + dmaIdx_), rawRead(static_cast<uint16_t>(dmaSrc_ + dmaIdx_)));
        if (++dmaIdx_ == 0xA0) dmaActive_ = false;
    }
    if (dmaDelay_ > 0 && --dmaDelay_ == 0) {   // новая передача перезапускает старую
        dmaSrc_ = dmaPendingSrc_;
        dmaIdx_ = 0;
        dmaActive_ = true;
    }
}

// ---------- HDMA ----------

void Bus::writeHdmaControl(uint8_t value) {
    if (hdmaActive_ && !hdmaGeneral_ && !(value & 0x80)) {   // запись с битом 7 = 0 отменяет HBlank-передачу
        hdmaActive_ = false;
        return;
    }
    hdmaRemaining_ = (value & 0x7F) + 1;
    hdmaActive_ = true;
    hdmaGeneral_ = !(value & 0x80);
    // Если запуск пришёлся на HBlank, первый блок уходит в нём же
    hdmaReady_ = !hdmaGeneral_ && ppu_.lcdOn() && ppu_.mode() == 0 && ppu_.line() < kScreenH;
}

void Bus::copyHdmaBlock() {
    for (int i = 0; i < 16; ++i) {
        const uint8_t v = rawRead(static_cast<uint16_t>(hdmaSrc_ + i));
        ppu_.pokeVram(static_cast<uint16_t>(0x8000 | ((hdmaDst_ + i) & 0x1FFF)), v);
    }
    hdmaSrc_ = static_cast<uint16_t>(hdmaSrc_ + 16);
    hdmaDst_ = static_cast<uint16_t>((hdmaDst_ + 16) & 0x1FFF);
    --hdmaRemaining_;
    // Процессор стоит, пока идёт копирование: 8 M-тактов на блок, при двойной скорости 16 (по времени то же самое)
    for (int i = 0, n = doubleSpeed_ ? 16 : 8; i < n; ++i) tickM();
}

void Bus::serviceHdma() {
    if (!hdmaActive_) return;
    if (hdmaGeneral_) {
        while (hdmaRemaining_ > 0) copyHdmaBlock();
        hdmaActive_ = hdmaGeneral_ = false;
    } else if (hdmaReady_ && ppu_.lcdOn()) {
        hdmaReady_ = false;
        copyHdmaBlock();
        if (hdmaRemaining_ == 0) hdmaActive_ = false;
    }
}

// ---------- регистры ввода-вывода ----------

uint8_t Bus::readIo(uint16_t addr) const {
    if (Ppu::ownsReg(addr)) return ppu_.readReg(addr);
    switch (addr) {
        case 0xFF00: return static_cast<uint8_t>(0xC0 | (joypadSelect_ & 0x30) | joypadLines());
        case 0xFF04: return static_cast<uint8_t>(sysCounter_ >> 8);  // DIV
        case 0xFF0F: return io_[0x0F] | 0xE0;                       // IF: старшие 3 бита читаются как 1
        case 0xFF51: case 0xFF52: case 0xFF53: case 0xFF54: return 0xFF;  // регистры адресов HDMA только для записи
        case 0xFF55:                                                // HDMA: оставшиеся блоки - 1, бит 7 = 1 когда передачи нет
            if (!cgbMode) return 0xFF;
            return static_cast<uint8_t>((hdmaActive_ ? 0x00 : 0x80) | ((hdmaRemaining_ - 1) & 0x7F));
        case 0xFF4D:                                                // KEY1
            if (!cgbMode) return 0xFF;
            return static_cast<uint8_t>((doubleSpeed_ ? 0x80 : 0x00) | 0x7E | (speedSwitchArmed_ ? 1 : 0));
        case 0xFF70: return cgbMode ? static_cast<uint8_t>(0xF8 | wramBank_) : 0xFF;  // SVBK
        default:     return io_[addr - 0xFF00];
    }
}

void Bus::writeIo(uint16_t addr, uint8_t value) {
    if (Ppu::ownsReg(addr)) { ppu_.writeReg(addr, value); return; }
    switch (addr) {
        case 0xFF00: joypadSelect_ = value & 0x30; return;
        case 0xFF04: {  // запись в DIV сбрасывает весь счётчик
            const bool old = timerSignal();
            sysCounter_ = 0;
            timerEdge(old);
            return;
        }
        case 0xFF07: {  // TAC: смена частоты тоже может дать спад сигнала
            const bool old = timerSignal();
            io_[0x07] = value & 7;
            timerEdge(old);
            return;
        }
        case 0xFF02:  // SC: запись 0x81 запускает передачу, символ берётся из SB (FF01)
            if (value == 0x81 && onSerial) onSerial(static_cast<char>(io_[0x01]));
            io_[0x02] = static_cast<uint8_t>(value & 0x7F);  // передача "завершена" сразу
            return;
        case 0xFF46:  // OAM DMA: источник 0xE0-0xFF отражает WRAM (0xC0-0xDF)
            io_[0x46] = value;
            dmaPendingSrc_ = static_cast<uint16_t>((value >= 0xE0 ? value - 0x20 : value) << 8);
            dmaDelay_ = 2;
            return;
        case 0xFF51: if (cgbMode) hdmaSrc_ = static_cast<uint16_t>((hdmaSrc_ & 0x00FF) | (value << 8)); return;
        case 0xFF52: if (cgbMode) hdmaSrc_ = static_cast<uint16_t>((hdmaSrc_ & 0xFF00) | (value & 0xF0)); return;
        case 0xFF53: if (cgbMode) hdmaDst_ = static_cast<uint16_t>((hdmaDst_ & 0x00F0) | ((value & 0x1F) << 8)); return;
        case 0xFF54: if (cgbMode) hdmaDst_ = static_cast<uint16_t>((hdmaDst_ & 0x1F00) | (value & 0xF0)); return;
        case 0xFF55: if (cgbMode) writeHdmaControl(value); return;
        case 0xFF4D:
            if (cgbMode) speedSwitchArmed_ = value & 1;
            return;
        case 0xFF70:
            if (cgbMode) { wramBank_ = value & 7; if (wramBank_ == 0) wramBank_ = 1; }
            return;
        default:
            io_[addr - 0xFF00] = value;
    }
}

void Bus::toggleSpeedIfArmed() {
    if (speedSwitchArmed_) {
        doubleSpeed_ = !doubleSpeed_;
        speedSwitchArmed_ = false;
    }
}

}  // namespace gb
