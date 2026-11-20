// Проверка контроллеров банков на синтетических ROM: в каждом 16-КБ банке первый байт равен номеру банка.
#include <cstdio>
#include <vector>
#include "Cartridge.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

static Cartridge make(uint8_t type, int banks, uint8_t ramCode) {
    std::vector<uint8_t> rom(static_cast<std::size_t>(banks) * 0x4000, 0);
    for (int b = 0; b < banks; ++b) { rom[b * 0x4000] = static_cast<uint8_t>(b); rom[b * 0x4000 + 1] = static_cast<uint8_t>(b >> 8); }
    rom[0x147] = type;
    rom[0x149] = ramCode;
    Cartridge c;
    c.loadFromMemory(rom);
    return c;
}
static unsigned bank4000(const Cartridge& c) { return c.read(0x4000) | (c.read(0x4001) << 8); }

int main() {
    // ---------- MBC1 ----------
    {
        Cartridge c = make(0x03, 128, 0x03);   // 2 МБ ROM, 32 КБ RAM
        CHECK(c.mbc() == Mbc::Mbc1 && c.hasBattery());
        CHECK(bank4000(c) == 1);
        c.write(0x2000, 5);   CHECK(bank4000(c) == 5);
        c.write(0x2000, 0);   CHECK(bank4000(c) == 1);          // банк 0 заменяется на 1
        c.write(0x2000, 0x20); CHECK(bank4000(c) == 1);         // 0x20 & 0x1F = 0 -> 1
        c.write(0x4000, 2);   c.write(0x2000, 3);
        CHECK(bank4000(c) == 0x43);                              // (2 << 5) | 3
        CHECK(c.read(0x0000) == 0);                              // режим 0: нулевая область всегда банк 0
        c.write(0x6000, 1);
        CHECK(c.read(0x0000) == 0x40);                           // режим 1: банк 0x40 и в области 0000-3FFF
        // RAM: выключена, пока не записано 0x0A
        c.write(0xA000, 0x77); CHECK(c.read(0xA000) == 0xFF);
        c.write(0x0000, 0x0A);
        c.write(0x4000, 1);                                      // режим 1: банк RAM 1
        c.write(0xA000, 0x77); CHECK(c.read(0xA000) == 0x77);
        c.write(0x4000, 0);    CHECK(c.read(0xA000) == 0);       // другой банк RAM
        c.write(0x6000, 0);    c.write(0x4000, 1);
        CHECK(c.read(0xA000) == 0);                              // режим 0: RAM всегда банк 0
        c.write(0x0000, 0x00); CHECK(c.read(0xA000) == 0xFF);    // RAM снова выключена
        // Номер банка по модулю числа банков: ROM на 32 КБ
        Cartridge small = make(0x01, 2, 0);
        small.write(0x2000, 7); CHECK(bank4000(small) == 1);     // 7 % 2
    }
    // ---------- MBC3 ----------
    {
        Cartridge c = make(0x10, 128, 0x03);   // таймер + RAM + батарейка
        CHECK(c.mbc() == Mbc::Mbc3 && c.hasBattery());
        int64_t t = 1000;
        c.timeSource = [&] { return t; };
        c.write(0x2000, 0x7F); CHECK(bank4000(c) == 0x7F);
        c.write(0x2000, 0);    CHECK(bank4000(c) == 1);
        c.write(0x0000, 0x0A);
        c.write(0x4000, 2); c.write(0xA010, 0x5A);
        c.write(0x4000, 0); CHECK(c.read(0xA010) == 0);
        c.write(0x4000, 2); CHECK(c.read(0xA010) == 0x5A);
        // Часы: выставляем 23:59:58, день 0 и даём пройти 3 секундам
        c.write(0x4000, 0x0C); c.write(0xA000, 0x40);            // HALT
        c.write(0x4000, 0x08); c.write(0xA000, 58);
        c.write(0x4000, 0x09); c.write(0xA000, 59);
        c.write(0x4000, 0x0A); c.write(0xA000, 23);
        c.write(0x4000, 0x0C); c.write(0xA000, 0x00);            // запуск
        t += 3;
        c.write(0x6000, 0); c.write(0x6000, 1);                  // защёлка
        c.write(0x4000, 0x08); CHECK(c.read(0xA000) == 1);
        c.write(0x4000, 0x09); CHECK(c.read(0xA000) == 0);
        c.write(0x4000, 0x0A); CHECK(c.read(0xA000) == 0);
        c.write(0x4000, 0x0B); CHECK(c.read(0xA000) == 1);       // день 1
        t += 10;                                                  // без новой защёлки значения не меняются
        c.write(0x4000, 0x08); CHECK(c.read(0xA000) == 1);
        // Перенос дней: 511 дней + 1 секунда
        c.write(0x4000, 0x0B); c.write(0xA000, 0xFF);
        c.write(0x4000, 0x0C); c.write(0xA000, 0x01);
        c.write(0x4000, 0x0A); c.write(0xA000, 23);
        c.write(0x4000, 0x09); c.write(0xA000, 59);
        c.write(0x4000, 0x08); c.write(0xA000, 59);
        t += 1;
        c.write(0x6000, 0); c.write(0x6000, 1);
        c.write(0x4000, 0x0C); CHECK((c.read(0xA000) & 0x81) == 0x80);   // бит переноса, день = 0
        c.write(0x4000, 0x0B); CHECK(c.read(0xA000) == 0);
    }
    // ---------- MBC5 ----------
    {
        Cartridge c = make(0x1B, 512, 0x04);   // 8 МБ ROM, 128 КБ RAM
        CHECK(c.mbc() == Mbc::Mbc5 && c.hasBattery());
        c.write(0x2000, 0);    CHECK(bank4000(c) == 0);          // у MBC5 банк 0 в окне 4000-7FFF разрешён
        c.write(0x2000, 0x25); CHECK(bank4000(c) == 0x25);
        c.write(0x3000, 1);    CHECK(bank4000(c) == 0x125);      // девятый бит
        c.write(0x2000, 0xFF); c.write(0x3000, 1); CHECK(bank4000(c) == 0x1FF);
        c.write(0x0000, 0x0A);
        for (int b = 0; b < 16; ++b) { c.write(0x4000, static_cast<uint8_t>(b)); c.write(0xA000, static_cast<uint8_t>(0xA0 + b)); }
        for (int b = 0; b < 16; ++b) { c.write(0x4000, static_cast<uint8_t>(b)); CHECK(c.read(0xA000) == 0xA0 + b); }
        c.write(0x0000, 0x0B); CHECK(c.read(0xA000) == 0xFF);    // включение только значением 0x0A
    }
    // ---------- без MBC и неподдерживаемые типы ----------
    {
        Cartridge c = make(0x00, 2, 0);
        c.write(0x2000, 1);
        CHECK(c.read(0x4000) == 1 && c.mbc() == Mbc::None);
        CHECK(!make(0x05, 4, 0).supported());
    }

    std::printf(failures ? "ПРОВАЛЕНО: %d\n" : "Все проверки MBC пройдены\n", failures);
    return failures != 0;
}
