// Проверка тактирования и шины без графики и без внешних ROM.
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include "Cpu.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

int main(int argc, char** argv) {
    const std::string romPath = argc > 1 ? argv[1] : "test.gb";

    // Собираем ROM на 32 КБ: JP 0x0150 на 0x0100, программа на 0x0150
    std::vector<uint8_t> rom(0x8000, 0x00);
    rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 0x01;      // JP 0x0150
    const uint8_t prog[] = {
        0x3E, 'H',        // LD A,'H'
        0xE0, 0x01,       // LDH (FF01),A
        0x3E, 0x81,       // LD A,0x81
        0xE0, 0x02,       // LDH (FF02),A   -> символ уходит в serial
        0x21, 0x00, 0xC0, // LD HL,0xC000
        0x36, 0x42,       // LD (HL),0x42
        0xFA, 0x00, 0xE0, // LD A,(0xE000)   Echo RAM зеркалит 0xC000
        0x18, 0xFE,       // JR -2 (бесконечный цикл)
    };
    for (size_t i = 0; i < sizeof prog; ++i) rom[0x150 + i] = prog[i];
    { std::ofstream f(romPath, std::ios::binary); f.write(reinterpret_cast<char*>(rom.data()), rom.size()); }

    Cartridge cart;
    CHECK(cart.load(romPath));
    gb::Bus bus;
    bus.attach(&cart);
    std::string serial;
    bus.onSerial = [&](char c) { serial += c; };
    gb::Cpu cpu(bus);

    // Такты по инструкциям (в M-тактах)
    CHECK(cpu.step() == 4);   // JP nn
    CHECK(cpu.regs().pc == 0x0150);
    CHECK(cpu.step() == 2);   // LD A,n
    CHECK(cpu.step() == 3);   // LDH (n),A
    CHECK(cpu.step() == 2);   // LD A,n
    CHECK(cpu.step() == 3);   // LDH (n),A
    CHECK(serial == "H");
    CHECK(cpu.step() == 3);   // LD HL,nn
    CHECK(cpu.step() == 3);   // LD (HL),n
    CHECK(cpu.step() == 4);   // LD A,(nn)
    CHECK(cpu.regs().a == 0x42);  // прочитали через Echo RAM
    CHECK(bus.cpuCycles() == (4 + 2 + 3 + 2 + 3 + 3 + 3 + 4) * 4);

    // Кадр: прокручиваем цикл JR, пока не наберётся 70224 такта
    const uint64_t startCycles = bus.cpuCycles();
    while (bus.cpuCycles() - startCycles < gb::kDotsPerFrame) CHECK(cpu.step() == 3);
    const uint64_t frameCycles = bus.cpuCycles() - startCycles;
    std::printf("T-такты за кадр: %llu (цель %u)\n", (unsigned long long)frameCycles, gb::kDotsPerFrame);
    CHECK(frameCycles >= gb::kDotsPerFrame && frameCycles < gb::kDotsPerFrame + 12);

    // Банки WRAM и VRAM (режим CGB)
    bus.write(0xFF70, 2); bus.write(0xD000, 0xAA);
    bus.write(0xFF70, 3); bus.write(0xD000, 0xBB);
    bus.write(0xFF70, 2); CHECK(bus.read(0xD000) == 0xAA);
    bus.write(0xFF70, 3); CHECK(bus.read(0xD000) == 0xBB);
    bus.write(0xFF70, 0); CHECK((bus.read(0xFF70) & 7) == 1);  // банк 0 превращается в 1
    const uint8_t lcdc = bus.read(0xFF40);
    bus.write(0xFF40, lcdc & 0x7F);   // экран выключен: иначе PPU мог бы закрыть VRAM
    bus.write(0xFF4F, 0); bus.write(0x8000, 0x11);
    bus.write(0xFF4F, 1); bus.write(0x8000, 0x22);
    bus.write(0xFF4F, 0); CHECK(bus.read(0x8000) == 0x11);
    bus.write(0xFF4F, 1); CHECK(bus.read(0x8000) == 0x22);
    bus.write(0xFF4F, 0); bus.write(0xFF40, lcdc);

    // Двойная скорость: KEY1 + STOP, затем PPU получает вдвое меньше точек на M-такт
    // ROM со STOP на 0x0100
    {
        std::vector<uint8_t> rom2(0x8000, 0x00);
        rom2[0x100] = 0x10; rom2[0x101] = 0x00;
        std::ofstream f(romPath, std::ios::binary); f.write(reinterpret_cast<char*>(rom2.data()), rom2.size());
    }
    Cartridge cart2; CHECK(cart2.load(romPath));
    gb::Bus bus2; bus2.attach(&cart2);
    gb::Cpu cpu2(bus2);
    bus2.write(0xFF4D, 1);
    cpu2.step();
    CHECK(bus2.doubleSpeed());
    const uint64_t d0 = bus2.ppuDots(), c0 = bus2.cpuCycles();
    for (int i = 0; i < 100; ++i) cpu2.step();   // NOP
    CHECK(bus2.cpuCycles() - c0 == 400);
    CHECK(bus2.ppuDots() - d0 == 200);

    // DMG: регистры CGB не должны работать
    bus.cgbMode = false;
    bus.write(0xFF4F, 1);
    CHECK(bus.read(0xFF4F) == 0xFF);

    std::printf(failures ? "ПРОВАЛЕНО: %d\n" : "Все проверки пройдены\n", failures);
    return failures != 0;
}
