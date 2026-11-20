#pragma once
#include <array>
#include <cstdint>
#include <vector>
#include "Clock.h"

namespace gb {

// Звуковой процессор (APU): два прямоугольных канала (первый со свипом частоты),
// волновой канал с 32 четырёхбитными отсчётами и шумовой канал на сдвиговом регистре.
// Как и PPU, не знает про SDL: получает время через tick() и копит готовые
// стереоотсчёты (int16, L R L R ...), которые оболочка забирает и отправляет в звуковую карту.
class Apu {
public:
    // cgbMode передаётся по ссылке: от режима зависят регистры PCM12/PCM34 и поведение при выключении питания
    explicit Apu(const bool& cgbMode) : cgb_(cgbMode) { reset(); }
    void reset();  // состояние после загрузочной ROM

    // Продвигает звук на cycles T-тактов обычной скорости (4 194 304 Гц; в режиме двойной скорости
    // APU, как и PPU, идёт с прежней частотой)
    void tick(int cycles);

    // Шаг секвенсора 512 Гц (по спаду бита 4 регистра DIV, в двойной скорости бита 5):
    // длительность, свип и огибающая громкости
    void frameSequencerStep();

    // Регистры 0xFF10-0xFF26, волновая память 0xFF30-0xFF3F и PCM12/PCM34 (0xFF76/0xFF77, только CGB)
    static bool ownsReg(uint16_t addr) {
        return (addr >= 0xFF10 && addr <= 0xFF3F) || addr == 0xFF76 || addr == 0xFF77;
    }
    uint8_t readReg(uint16_t addr) const;
    void writeReg(uint16_t addr, uint8_t v);

    // Частота дискретизации выходного звука (можно слегка менять на ходу для подстройки под звуковую карту)
    void setSampleRate(double hz);
    double sampleRate() const { return sampleRate_; }

    // Громкость 0..1 (умножается на выход)
    void setVolume(float v) { volume_ = v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

    // Накопленные стереоотсчёты. Оболочка забирает их и очищает буфер.
    // Чтобы память не росла, когда звук никто не забирает, буфер ограничен примерно секундой.
    std::vector<int16_t>& samples() { return samples_; }

    // Для тестов
    bool channelOn(int i) const { return ch_[i].on; }
    int lengthCounter(int i) const { return ch_[i].length; }
    bool powered() const { return power_; }

private:
    struct Channel {
        bool on = false;   // канал звучит (биты 0-3 регистра NR52)
        int length = 0;    // счётчик длительности: 0..64 (у волнового 0..256)
        int timer = 0;     // T-тактов до следующего шага частотного таймера
        int pos = 0;       // позиция в скважности (0..7) или в волновой памяти (0..31)
        int volume = 0;    // текущая громкость огибающей 0..15
        int envTimer = 0;
    };

    const bool& cgb_;
    bool power_ = true;

    // Сырые значения регистров FF10-FF2F. Регистры каналов лежат единообразно:
    // канал i занимает байты 5*i .. 5*i+4 (NRx0..NRx4); FF15 и FF1F не используются.
    std::array<uint8_t, 0x20> r_{};
    std::array<uint8_t, 16> wave_{};
    Channel ch_[4];

    // Свип первого канала
    bool sweepOn_ = false;
    int shadow_ = 0;
    int sweepTimer_ = 0;
    bool negUsed_ = false;  // с последнего запуска хотя бы один расчёт был с вычитанием

    uint16_t lfsr_ = 0x7FFF;
    uint8_t waveSample_ = 0;  // последний прочитанный из волновой памяти отсчёт (4 бита)
    int fsStep_ = 0;          // следующий шаг секвенсора 0..7

    // Вывод
    double sampleRate_ = 48000.0;
    double cyclesPerSample_ = kClockHz / 48000.0;
    double sampleClock_ = 0.0;     // накопленные такты текущего отсчёта
    double sumL_ = 0.0, sumR_ = 0.0;
    double hpL_ = 0.0, hpR_ = 0.0; // "конденсаторы" фильтра верхних частот (убирают постоянную составляющую)
    double hpCharge_ = 0.0;
    float volume_ = 1.f;
    std::vector<int16_t> samples_;

    int freq(int i) const { return ((r_[5 * i + 4] & 7) << 8) | r_[5 * i + 3]; }
    bool dacOn(int i) const { return i == 2 ? (r_[0x0A] & 0x80) != 0 : (r_[5 * i + 2] & 0xF8) != 0; }
    int maxLength(int i) const { return i == 2 ? 256 : 64; }
    int digitalOut(int i) const;  // 0..15

    void writeLength(int i, uint8_t v);
    void writeControl(int i, uint8_t v);  // NRx4
    void trigger(int i);
    int sweepCalc();
    void clockLength();
    void clockSweep();
    void clockEnvelope();
    void powerOff();
};

}  // namespace gb
