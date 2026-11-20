// Проверка звукового процессора через шину: регистры, длительность, питание, свип и генерация отсчётов
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include "Bus.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++failures;
}

void run(gb::Bus& bus, int mcycles) {
    for (int i = 0; i < mcycles; ++i) bus.tickM();
}

}  // namespace

int main() {
    {
        gb::Bus bus;
        check(bus.read(0xFF26) == 0xF1, "NR52 после загрузки = F1");
        check(bus.read(0xFF10) == 0x80, "NR10 читается как 80");
        check(bus.read(0xFF11) == 0xBF, "NR11 читается как BF");
        check(bus.read(0xFF13) == 0xFF, "NR13 только для записи");
        check(bus.read(0xFF24) == 0x77 && bus.read(0xFF25) == 0xF3, "NR50 = 77, NR51 = F3");
        check(bus.read(0xFF27) == 0xFF, "FF27 не используется");

        bus.write(0xFF30, 0x12);
        bus.write(0xFF3F, 0xEF);
        check(bus.read(0xFF30) == 0x12 && bus.read(0xFF3F) == 0xEF, "волновая память читается и пишется");
    }
    {
        gb::Bus bus;
        bus.write(0xFF21, 0xF0);   // NR22: громкость 15, ЦАП включён
        bus.write(0xFF20, 0x30);   // NR41 не трогаем канал 2
        bus.write(0xFF16, 0x30);   // NR21: длительность 64 - 48 = 16
        bus.write(0xFF17, 0xF0);
        bus.write(0xFF18, 0x00);
        bus.write(0xFF19, 0xC7);   // NR24: запуск + длительность
        check((bus.read(0xFF26) & 0x02) != 0, "канал 2 включился после запуска");
        run(bus, 61000);
        check((bus.read(0xFF26) & 0x02) != 0, "канал 2 ещё звучит до окончания длительности");
        run(bus, 9000);
        check((bus.read(0xFF26) & 0x02) == 0, "канал 2 выключился по счётчику длительности");
    }
    {
        gb::Bus bus;
        bus.write(0xFF17, 0x00);   // NR22 = 0: ЦАП выключен
        bus.write(0xFF19, 0x80);
        check((bus.read(0xFF26) & 0x02) == 0, "без ЦАП канал не запускается");
    }
    {
        gb::Bus bus;
        bus.write(0xFF12, 0xF0);
        bus.write(0xFF10, 0x01);   // свип: сдвиг 1, сложение
        bus.write(0xFF13, 0xFF);
        bus.write(0xFF14, 0x87);   // частота 0x7FF: 2047 + 1023 > 2047
        check((bus.read(0xFF26) & 0x01) == 0, "переполнение свипа сразу выключает канал 1");
    }
    {
        gb::Bus bus;
        bus.write(0xFF26, 0x00);
        check(bus.read(0xFF26) == 0x70, "питание выключено, все каналы молчат");
        bus.write(0xFF12, 0xF0);
        check(bus.read(0xFF12) == 0x00, "без питания запись в регистры игнорируется");
        check(bus.read(0xFF24) == 0x00, "выключение питания очищает NR50");
        bus.write(0xFF30, 0x5A);
        check(bus.read(0xFF30) == 0x5A, "волновая память доступна без питания");
        bus.write(0xFF26, 0x80);
        check(bus.read(0xFF26) == 0xF0, "питание включено");
    }
    {
        gb::Bus bus;
        bus.apu().setSampleRate(48000.0);
        bus.apu().samples().clear();
        bus.write(0xFF12, 0xF0);
        bus.write(0xFF13, 0x00);
        bus.write(0xFF14, 0x86);   // ~ 440 Гц, громкость 15
        run(bus, static_cast<int>(gb::kDotsPerFrame / gb::kTCyclesPerM));  // один кадр
        const auto& s = bus.apu().samples();
        // 48000 / 59,73 = 803,6 стереопары на кадр
        check(s.size() >= 2 * 798 && s.size() <= 2 * 809 && s.size() % 2 == 0, "за кадр получено ~804 стереоотсчёта");
        int peak = 0;
        for (int16_t v : s) peak = std::max(peak, std::abs(static_cast<int>(v)));
        check(peak > 1000, "в отсчётах есть звук");
    }
    {
        gb::Bus bus;
        bus.write(0xFF26, 0x00);
        bus.apu().samples().clear();
        run(bus, 20000);
        int peak = 0;
        for (int16_t v : bus.apu().samples()) peak = std::max(peak, std::abs(static_cast<int>(v)));
        check(peak == 0, "без питания тишина");
    }

    std::printf(failures ? "\n%d проверок не прошло\n" : "\nВсе проверки пройдены\n", failures);
    return failures ? 1 : 0;
}
