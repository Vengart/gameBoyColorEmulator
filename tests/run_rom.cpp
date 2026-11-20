// Запуск тестового ROM без графики: вывод последовательного порта идёт в консоль.
// Код возврата: 0 = "Passed", 1 = "Failed", 2 = неизвестный опкод, 3 = превышен лимит времени.
#include <cstdio>
#include <string>
#include "Cpu.h"

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: run_rom <rom> [max_tcycles] [--dmg]\n"); return 64; }
    uint64_t limit = 200'000'000ULL;  // в T-тактах
    bool dmg = false;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--dmg") dmg = true;
        else limit = std::stoull(argv[i]);
    }

    Cartridge cart;
    if (!cart.load(argv[1])) { std::fprintf(stderr, "cannot load %s\n", argv[1]); return 64; }
    static gb::Bus bus;  // статический: объект большой
    bus.cgbMode = !dmg;
    bus.attach(&cart);
    std::string out;
    bus.onSerial = [&](char c) { out += c; std::putchar(c); };
    gb::Cpu cpu(bus);

    while (bus.cpuCycles() < limit) {
        if (cpu.step() == 0) {
            std::printf("\nНеизвестный опкод 0x%02X по адресу 0x%04X\n", cpu.lastUnknownOpcode(), cpu.regs().pc - 1);
            return 2;
        }
        if (out.find("Passed") != std::string::npos) { std::printf("\n[%llu T-тактов]\n", (unsigned long long)bus.cpuCycles()); return 0; }
        if (out.find("Failed") != std::string::npos) { std::printf("\n"); return 1; }

        // Тесты звука Blargg (dmg_sound, cgb_sound) пишут результат в RAM картриджа:
        // A001-A003 = DE B0 61, A000 = 80 пока тест идёт, затем код результата (0 = успех), с A004 текст
        if ((bus.cpuCycles() & 0xFFFF) < 32 && bus.read(0xA001) == 0xDE && bus.read(0xA002) == 0xB0 &&
            bus.read(0xA003) == 0x61 && bus.read(0xA000) != 0x80) {
            std::string text;
            for (uint16_t a = 0xA004; a < 0xBFFF && bus.read(a) != 0; ++a) text += static_cast<char>(bus.read(a));
            const int code = bus.read(0xA000);
            std::printf("%s\n[код %d, %llu T-тактов]\n", text.c_str(), code, (unsigned long long)bus.cpuCycles());
            return code == 0 ? 0 : 1;
        }
    }
    std::printf("\nЛимит времени (%llu тактов), PC=0x%04X\n", (unsigned long long)limit, cpu.regs().pc);
    return 3;
}
