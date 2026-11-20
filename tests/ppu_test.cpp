// Синтетическая проверка PPU без ROM: данные кладутся в VRAM/OAM через шину,
// затем проверяются пиксели готового кадра.
#include <cstdio>
#include <initializer_list>
#include "Bus.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

static constexpr uint32_t kShade[4] = {0xFFE0F8D0, 0xFF88C070, 0xFF346856, 0xFF081820};  // как в Ppu.cpp

// Прогнать целый кадр (70224 точки = 17556 M-тактов)
static void runFrame(gb::Bus& bus) { for (int i = 0; i < 17556; ++i) bus.tickM(); }

static void tileRow(gb::Bus& bus, int tile, int row, uint8_t lo, uint8_t hi) {
    bus.write(static_cast<uint16_t>(0x8000 + tile * 16 + row * 2), lo);
    bus.write(static_cast<uint16_t>(0x8000 + tile * 16 + row * 2 + 1), hi);
}
static void solidTile(gb::Bus& bus, int tile, int color) {
    for (int r = 0; r < 8; ++r) tileRow(bus, tile, r, (color & 1) ? 0xFF : 0, (color & 2) ? 0xFF : 0);
}

// OAM закрыта для процессора, пока PPU ищет спрайты и рисует строку, поэтому на время записи гасим экран
static void setOam(gb::Bus& bus, int offset, std::initializer_list<uint8_t> bytes) {
    const uint8_t lcdc = bus.read(0xFF40);
    bus.write(0xFF40, lcdc & 0x7F);
    int i = 0;
    for (uint8_t b : bytes) bus.write(static_cast<uint16_t>(0xFE00 + offset + i++), b);
    bus.write(0xFF40, lcdc);
}

int main() {
    // ---------- DMG: фон, прокрутка, палитра ----------
    {
        gb::Bus bus;
        bus.cgbMode = false;
        gb::Ppu& ppu = bus.ppu();
        solidTile(bus, 1, 1);
        solidTile(bus, 2, 2);
        bus.write(0x9800, 1);  // клетка (0,0) = тайл 1
        bus.write(0x9801, 2);  // клетка (1,0) = тайл 2
        bus.write(0xFF47, 0xE4);  // BGP: оттенок = индекс цвета
        runFrame(bus);
        CHECK(ppu.frame()[0] == kShade[1]);
        CHECK(ppu.frame()[7] == kShade[1]);
        CHECK(ppu.frame()[8] == kShade[2]);
        CHECK(ppu.frame()[16] == kShade[0]);          // дальше пустой тайл 0
        CHECK(ppu.frame()[143 * 160] == kShade[0]);   // нижняя строка тоже отрисована

        // Палитра: инвертируем (цвет 1 -> оттенок 2)
        bus.write(0xFF47, 0x1B);
        runFrame(bus);
        CHECK(ppu.frame()[0] == kShade[2]);

        // Прокрутка по X на 8 пикселей: в нулевом пикселе теперь тайл 2
        bus.write(0xFF47, 0xE4);
        bus.write(0xFF43, 8);
        runFrame(bus);
        CHECK(ppu.frame()[0] == kShade[2]);
        bus.write(0xFF43, 0);

        // Спрайт 8x8 в левом верхнем углу (OAM: Y=16, X=8), тайл 3 цвета 3, палитра OBP0
        solidTile(bus, 3, 3);
        bus.write(0xFF48, 0xE4);
        bus.write(0xFF40, 0x13);
        setOam(bus, 0, {16, 8, 3, 0});
        bus.write(0xFF40, 0x93);  // фон + спрайты
        runFrame(bus);
        CHECK(ppu.frame()[0] == kShade[3]);
        CHECK(ppu.frame()[7 * 160 + 7] == kShade[3]);
        CHECK(ppu.frame()[8] == kShade[2]);           // справа от спрайта по-прежнему фон

        // Приоритет "под фоном": фон с ненулевым цветом перекрывает спрайт
        setOam(bus, 3, {0x80});
        runFrame(bus);
        CHECK(ppu.frame()[0] == kShade[1]);

        // Отражение по X: у тайла 4 закрашен только левый пиксель строки 0
        for (int r = 0; r < 8; ++r) tileRow(bus, 4, r, 0x80, 0x80);
        setOam(bus, 2, {4, 0x20});                        // X-flip
        bus.write(0x9800, 0); bus.write(0x9801, 0);       // убираем фон
        runFrame(bus);
        CHECK(ppu.frame()[0] == kShade[0]);
        CHECK(ppu.frame()[7] == kShade[3]);               // пиксель переехал вправо

        // OAM DMA: спрайт готовится в WRAM, затем копируется в OAM за 160 тактов
        bus.write(0xFF40, 0x13);   // экран выключен, чтобы PPU не закрывал OAM
        bus.write(0xC000, 40); bus.write(0xC001, 40); bus.write(0xC002, 3); bus.write(0xC003, 0);
        bus.write(0xFF46, 0xC0);
        CHECK(!bus.oamDmaActive());                 // передача стартует с задержкой в один такт
        bus.tickM();
        CHECK(bus.read(0xC000) == 0x28);            // этот такт память ещё свободна
        bus.tickM();
        CHECK(bus.oamDmaActive());
        CHECK(bus.read(0xC000) == 0xFF);            // идёт DMA: процессор видит память как 0xFF
        bus.write(0xC010, 0x99);                    // запись тоже игнорируется
        bus.write(0xFF80, 0x5A); CHECK(bus.read(0xFF80) == 0x5A);   // HRAM доступна всегда
        for (int i = 0; i < 159; ++i) bus.tickM();
        CHECK(bus.oamDmaActive());                  // осталась последняя точка передачи
        bus.tickM();
        CHECK(!bus.oamDmaActive());
        CHECK(bus.read(0xC010) == 0);
        CHECK(bus.read(0xFE00) == 40 && bus.read(0xFE02) == 3);
    }

    // ---------- окно ----------
    {
        gb::Bus bus;
        bus.cgbMode = false;
        solidTile(bus, 5, 3);
        for (int i = 0; i < 32; ++i) bus.write(static_cast<uint16_t>(0x9C00 + i), 5);  // первая строка карты окна
        bus.write(0xFF47, 0xE4);
        bus.write(0xFF4A, 0);     // WY
        bus.write(0xFF4B, 7 + 80);  // WX: окно начинается с x = 80
        bus.write(0xFF40, 0xF1);  // LCD, окно (бит 5), карта окна 9C00 (бит 6), тайлы 8000, фон
        runFrame(bus);
        CHECK(bus.ppu().frame()[79] == kShade[0]);
        CHECK(bus.ppu().frame()[80] == kShade[3]);
    }

    // ---------- CGB: палитры и атрибуты ----------
    {
        gb::Bus bus;
        bus.cgbMode = true;
        solidTile(bus, 1, 1);
        bus.write(0x9800, 1);
        // Палитра 1, цвет 1 = чистый красный (RGB555 = 0x001F), автоинкремент индекса
        bus.write(0xFF68, 0x80 | (1 * 8 + 1 * 2));
        bus.write(0xFF69, 0x1F);
        bus.write(0xFF69, 0x00);
        CHECK((bus.read(0xFF68) & 0x3F) == (1 * 8 + 1 * 2 + 2));  // индекс сдвинулся
        // Атрибуты тайла (банк 1 VRAM): палитра 1
        bus.write(0xFF4F, 1);
        bus.write(0x9800, 0x01);
        bus.write(0xFF4F, 0);
        runFrame(bus);
        CHECK(bus.ppu().frame()[0] == 0xFFFF0000);
        CHECK(bus.ppu().frame()[8] != 0xFFFF0000);

        // Атрибут "банк 1": тайл берётся из второго банка VRAM
        bus.write(0xFF4F, 1);
        for (int r = 0; r < 8; ++r) { bus.write(static_cast<uint16_t>(0x8010 + r * 2), 0); bus.write(static_cast<uint16_t>(0x8011 + r * 2), 0xFF); }
        bus.write(0x9800, 0x09);   // палитра 1 + банк 1
        bus.write(0xFF4F, 0);
        // В палитре 1 цвет 2 пока серый по умолчанию
        runFrame(bus);
        CHECK(bus.ppu().frame()[0] == 0xFF525252);   // 0x294A = (10,10,10) -> 82,82,82
    }

    // ---------- режимы, VBlank и STAT ----------
    {
        gb::Bus bus;
        bus.cgbMode = false;
        CHECK(bus.ppu().mode() == 2);                 // начало строки 0
        for (int i = 0; i < 20; ++i) bus.tickM();     // 80 точек
        CHECK(bus.ppu().mode() == 3);
        for (int i = 0; i < 43; ++i) bus.tickM();     // ещё 172 точки
        CHECK(bus.ppu().mode() == 0);
        // Дойти до VBlank: к моменту входа в строку 144 должен подняться бит 0 IF
        bus.write(0xFFFF, 0x03);  // IE: VBlank + STAT
        while (bus.ppu().line() < 144) bus.tickM();
        CHECK(bus.ppu().mode() == 1);
        CHECK(bus.read(0xFF0F) & 1);
        CHECK(bus.ppu().frameCount() == 1);
        CHECK(bus.read(0xFF44) == 144);

        // STAT-прерывание по совпадению LY = LYC
        bus.acknowledgeInterrupt(1);
        bus.write(0xFF45, 150);
        bus.write(0xFF41, 0x40);
        while (bus.ppu().line() < 150) bus.tickM();
        CHECK(bus.read(0xFF0F) & 2);
        CHECK(bus.read(0xFF41) & 4);
    }

    // ---------- выключение экрана ----------
    {
        gb::Bus bus;
        bus.cgbMode = false;
        solidTile(bus, 1, 3);
        bus.write(0x9800, 1);
        runFrame(bus);
        CHECK(bus.ppu().frame()[0] != kShade[0]);
        bus.write(0xFF40, 0x11);   // бит 7 = 0
        CHECK(bus.ppu().frame()[0] == kShade[0]);
        CHECK(bus.read(0xFF44) == 0);
        CHECK(bus.ppu().mode() == 0);
    }

    // ---------- джойстик ----------
    {
        gb::Bus bus;
        bus.write(0xFF00, 0x20);   // выбрана крестовина (бит 4 = 0)
        CHECK((bus.read(0xFF00) & 0x0F) == 0x0F);
        bus.setButtons(gb::kRight | gb::kA);
        CHECK((bus.read(0xFF00) & 0x0F) == 0x0E);  // Right = бит 0 в нуле
        bus.write(0xFF00, 0x10);   // выбраны кнопки
        CHECK((bus.read(0xFF00) & 0x0F) == 0x0E);  // A = бит 0 в нуле
    }

    std::printf(failures ? "ПРОВАЛЕНО: %d\n" : "Все проверки PPU пройдены\n", failures);
    return failures != 0;
}
