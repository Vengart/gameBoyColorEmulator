// Проверка HDMA, блокировки VRAM и OAM для процессора и взаимодействия HDMA с режимами PPU.
#include <cstdio>
#include "Bus.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

static void tickUntil(gb::Bus& bus, int mode, int line = -1) {
    for (int guard = 0; guard < 200000; ++guard) {
        if (bus.ppu().mode() == mode && (line < 0 || bus.ppu().line() == line)) return;
        bus.tickM();
    }
    CHECK(false && "режим не наступил");
}
static void setSource(gb::Bus& bus, uint16_t src, uint16_t dst) {
    bus.write(0xFF51, static_cast<uint8_t>(src >> 8)); bus.write(0xFF52, static_cast<uint8_t>(src));
    bus.write(0xFF53, static_cast<uint8_t>(dst >> 8)); bus.write(0xFF54, static_cast<uint8_t>(dst));
}
static void fillWram(gb::Bus& bus) { for (int i = 0; i < 0x200; ++i) bus.write(static_cast<uint16_t>(0xC000 + i), static_cast<uint8_t>(i * 7 + 1)); }
static uint8_t expected(int i) { return static_cast<uint8_t>(i * 7 + 1); }

int main() {
    // ---------- блокировка доступа ----------
    {
        gb::Bus bus;
        bus.cgbMode = false;
        gb::Ppu& ppu = bus.ppu();
        CHECK(ppu.mode() == 2);
        // Режим 2: VRAM открыта, OAM закрыта
        bus.write(0x8000, 0x12); CHECK(bus.read(0x8000) == 0x12);
        bus.write(0xFE00, 0x34); CHECK(bus.read(0xFE00) == 0xFF);
        tickUntil(bus, 3);
        // Режим 3: закрыто всё
        bus.write(0x8000, 0x56); CHECK(bus.read(0x8000) == 0xFF);
        CHECK(bus.read(0xFE00) == 0xFF);
        tickUntil(bus, 0);
        // HBlank: открыто всё; запись в режиме 3 была проигнорирована, в режиме 2 нет
        CHECK(bus.read(0x8000) == 0x12);
        CHECK(bus.read(0xFE00) == 0x00);                 // запись 0x34 в режиме 2 тоже не прошла
        bus.write(0xFE00, 0x34); CHECK(bus.read(0xFE00) == 0x34);
        tickUntil(bus, 1);
        // VBlank: открыто всё
        bus.write(0x8001, 0x77); CHECK(bus.read(0x8001) == 0x77);
        bus.write(0xFE01, 0x78); CHECK(bus.read(0xFE01) == 0x78);
        // Экран выключен: открыто всё, независимо от режима
        bus.write(0xFF40, 0x00);
        bus.write(0x9000, 0x99); CHECK(bus.read(0x9000) == 0x99);
        bus.write(0xFE02, 0x55); CHECK(bus.read(0xFE02) == 0x55);
    }
    // ---------- OAM DMA читает VRAM с учётом блокировки ----------
    {
        gb::Bus bus;
        bus.cgbMode = false;
        bus.write(0xFF40, 0x00);
        bus.write(0x8000, 0x42);
        bus.write(0xFF40, 0x80);
        tickUntil(bus, 3);                               // VRAM закрыта: DMA прочитает 0xFF
        bus.write(0xFF46, 0x80);
        for (int i = 0; i < 4; ++i) bus.tickM();
        CHECK(bus.oamDmaActive());
        for (int i = 0; i < 170; ++i) bus.tickM();
        CHECK(!bus.oamDmaActive());
        bus.write(0xFF40, 0x00);                         // открыть OAM для чтения
        CHECK(bus.read(0xFE00) == 0xFF);                 // первый байт DMA прочитал закрытую VRAM
    }
    // ---------- HDMA: общая передача ----------
    {
        gb::Bus bus;
        bus.cgbMode = true;
        bus.write(0xFF40, 0x00);                         // экран выключен
        fillWram(bus);
        setSource(bus, 0xC000, 0x8000);
        const uint64_t before = bus.cpuCycles();
        bus.write(0xFF55, 0x01);                         // два блока = 32 байта, режим общей передачи
        CHECK(bus.hdmaActive());
        CHECK(bus.read(0xFF55) == 0x01);                 // оставшиеся блоки - 1, бит 7 = 0: идёт передача
        CHECK(bus.read(0x8000) == 0x00);                 // пока процессор не сделал шаг, ничего не скопировано
        bus.serviceHdma();
        CHECK(!bus.hdmaActive());
        CHECK(bus.read(0xFF55) == 0xFF);                 // готово
        for (int i = 0; i < 32; ++i) CHECK(bus.read(static_cast<uint16_t>(0x8000 + i)) == expected(i));
        CHECK(bus.read(0x8020) == 0x00);                 // дальше не писали
        CHECK((bus.cpuCycles() - before) / 4 == 16);     // 2 блока по 8 M-тактов
    }
    // ---------- HDMA: банк VRAM, адреса, заворачивание ----------
    {
        gb::Bus bus;
        bus.cgbMode = true;
        bus.write(0xFF40, 0x00);
        fillWram(bus);
        bus.write(0xFF4F, 1);                            // назначение: второй банк VRAM
        setSource(bus, 0xC00F, 0x9FF8);                  // младшие 4 бита игнорируются, адрес назначения 0x9FF0
        bus.write(0xFF55, 0x01);
        bus.serviceHdma();
        CHECK(bus.read(0x9FF0) == expected(0));          // 0xC00F -> 0xC000
        CHECK(bus.read(0x9FFF) == expected(15));
        CHECK(bus.read(0x8000) == expected(16));         // второй блок завернулся на начало 0x8000
        bus.write(0xFF4F, 0);
        CHECK(bus.read(0x8000) == 0x00);                 // банк 0 не тронут
    }
    // ---------- HDMA: передача по HBlank ----------
    {
        gb::Bus bus;
        bus.cgbMode = true;
        bus.write(0xFF40, 0x00);
        fillWram(bus);
        setSource(bus, 0xC000, 0x8000);
        bus.write(0xFF40, 0x80);                         // экран включён
        tickUntil(bus, 3, 0);                            // середина строки 0, не HBlank
        bus.write(0xFF55, 0x82);                         // три блока по HBlank
        CHECK(bus.hdmaActive() && bus.read(0xFF55) == 0x02);
        bus.serviceHdma();
        CHECK(bus.read(0xFF55) == 0x02);                 // HBlank ещё не наступил: ничего не передано
        tickUntil(bus, 0, 0);                            // HBlank строки 0
        bus.serviceHdma();
        CHECK(bus.read(0xFF55) == 0x01);
        CHECK(bus.read(0x8000) == expected(0) && bus.read(0x800F) == expected(15));
        CHECK(bus.read(0x8010) == 0x00);                 // второй блок ждёт следующего HBlank
        bus.serviceHdma();
        CHECK(bus.read(0xFF55) == 0x01);                 // в том же HBlank блок не повторяется
        tickUntil(bus, 3, 1);
        tickUntil(bus, 0, 1);
        bus.serviceHdma();
        CHECK(bus.read(0xFF55) == 0x00);
        CHECK(bus.read(0x8010) == expected(16));
        tickUntil(bus, 3, 2);
        tickUntil(bus, 0, 2);
        bus.serviceHdma();
        CHECK(!bus.hdmaActive() && bus.read(0xFF55) == 0xFF);
        CHECK(bus.read(0x8020) == expected(32));
    }
    // ---------- HDMA: отмена ----------
    {
        gb::Bus bus;
        bus.cgbMode = true;
        bus.write(0xFF40, 0x00);
        fillWram(bus);
        setSource(bus, 0xC000, 0x8000);
        bus.write(0xFF40, 0x80);
        tickUntil(bus, 0, 0);
        bus.write(0xFF55, 0x83);                         // запуск прямо в HBlank: первый блок уходит сразу
        bus.serviceHdma();
        CHECK(bus.read(0xFF55) == 0x02);
        bus.write(0xFF55, 0x00);                         // бит 7 = 0 во время передачи отменяет её
        CHECK(!bus.hdmaActive());
        CHECK(bus.read(0xFF55) == 0x82);                 // бит 7 = 1, остаток 3 блока
        tickUntil(bus, 3, 2);
        tickUntil(bus, 0, 2);
        bus.serviceHdma();
        CHECK(bus.read(0x8010) == 0x00);                 // после отмены ничего не копируется
    }
    // ---------- в режиме DMG регистров HDMA нет ----------
    {
        gb::Bus bus;
        bus.cgbMode = false;
        bus.write(0xFF55, 0x00);
        CHECK(!bus.hdmaActive() && bus.read(0xFF55) == 0xFF);
    }

    std::printf(failures ? "ПРОВАЛЕНО: %d\n" : "Все проверки DMA пройдены\n", failures);
    return failures != 0;
}
