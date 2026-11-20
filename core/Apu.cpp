#include "Apu.h"
#include <cmath>

namespace gb {

namespace {

// Биты, которые читаются как 1 (недоступные для чтения или неиспользуемые), для FF10-FF26
constexpr uint8_t kReadMask[0x17] = {
    0x80, 0x3F, 0x00, 0xFF, 0xBF,  // NR10 NR11 NR12 NR13 NR14
    0xFF, 0x3F, 0x00, 0xFF, 0xBF,  // ---- NR21 NR22 NR23 NR24
    0x7F, 0xFF, 0x9F, 0xFF, 0xBF,  // NR30 NR31 NR32 NR33 NR34
    0xFF, 0xFF, 0x00, 0x00, 0xBF,  // ---- NR41 NR42 NR43 NR44
    0x00, 0x00, 0x70,              // NR50 NR51 NR52
};

// Скважность прямоугольной волны: бит n = уровень на шаге n (12,5 %, 25 %, 50 %, 75 %)
constexpr uint8_t kDuty[4] = {0x80, 0x81, 0xE1, 0x7E};

// Делители шумового канала в T-тактах (код 0 соответствует делителю 0,5)
constexpr int kNoiseDiv[8] = {8, 16, 32, 48, 64, 80, 96, 112};

constexpr int kNr10 = 0x00, kNr30 = 0x0A, kNr32 = 0x0C, kNr43 = 0x12, kNr50 = 0x14, kNr51 = 0x15;

}  // namespace

void Apu::reset() {
    r_.fill(0);
    wave_.fill(0);
    for (auto& c : ch_) c = Channel{};
    sweepOn_ = false; shadow_ = 0; sweepTimer_ = 0; negUsed_ = false;
    lfsr_ = 0x7FFF;
    waveSample_ = 0;
    fsStep_ = 0;
    power_ = true;

    // После загрузочной ROM: первый канал доиграл звук логотипа (громкость 0, но канал включён), NR52 = F1
    r_[0x01] = 0x80;  // NR11: скважность 50 %
    r_[0x02] = 0xF3;  // NR12
    r_[0x03] = 0xC1;  // NR13/NR14: частота последней ноты логотипа
    r_[0x04] = 0x07;
    r_[kNr50] = 0x77;
    r_[kNr51] = 0xF3;
    ch_[0].on = true;

    sampleClock_ = sumL_ = sumR_ = hpL_ = hpR_ = 0.0;
    samples_.clear();
    setSampleRate(sampleRate_);
}

void Apu::setSampleRate(double hz) {
    if (hz < 8000.0) hz = 8000.0;
    sampleRate_ = hz;
    cyclesPerSample_ = kClockHz / hz;
    hpCharge_ = std::pow(0.999958, cyclesPerSample_);
}

// ---------- регистры ----------

uint8_t Apu::readReg(uint16_t addr) const {
    if (addr >= 0xFF30 && addr <= 0xFF3F) {
        // Пока волновой канал играет, процессор видит байт, который сейчас читает канал
        return ch_[2].on ? wave_[ch_[2].pos >> 1] : wave_[addr - 0xFF30];
    }
    if (addr == 0xFF76) return cgb_ ? static_cast<uint8_t>(digitalOut(0) | (digitalOut(1) << 4)) : 0xFF;
    if (addr == 0xFF77) return cgb_ ? static_cast<uint8_t>(digitalOut(2) | (digitalOut(3) << 4)) : 0xFF;
    if (addr == 0xFF26) {
        uint8_t v = static_cast<uint8_t>(0x70 | (power_ ? 0x80 : 0));
        for (int i = 0; i < 4; ++i) if (ch_[i].on) v |= static_cast<uint8_t>(1 << i);
        return v;
    }
    if (addr > 0xFF26) return 0xFF;  // FF27-FF2F не используются
    const int idx = addr - 0xFF10;
    return static_cast<uint8_t>(r_[idx] | kReadMask[idx]);
}

void Apu::writeReg(uint16_t addr, uint8_t v) {
    if (addr >= 0xFF30 && addr <= 0xFF3F) {  // волновая память доступна всегда, даже без питания
        if (ch_[2].on) wave_[ch_[2].pos >> 1] = v;
        else wave_[addr - 0xFF30] = v;
        return;
    }
    if (addr == 0xFF76 || addr == 0xFF77) return;  // только чтение
    if (addr == 0xFF26) {
        const bool on = (v & 0x80) != 0;
        if (!on && power_) {
            powerOff();
        } else if (on && !power_) {
            power_ = true;
            fsStep_ = 0;  // следующий шаг секвенсора будет нулевым
            for (auto& c : ch_) c.pos = 0;
            waveSample_ = 0;
        }
        return;
    }
    if (addr > 0xFF26) return;

    const int idx = addr - 0xFF10;
    if (!power_) {
        // Без питания запись игнорируется; на DMG исключение составляют счётчики длительности
        if (!cgb_ && (idx == 0x01 || idx == 0x06 || idx == 0x0B || idx == 0x10)) writeLength(idx / 5, v);
        return;
    }

    if (idx == kNr50 || idx == kNr51) { r_[idx] = v; return; }
    if (idx == kNr10) {
        // Снятие бита вычитания после расчёта с вычитанием выключает канал
        const bool oldNegate = (r_[kNr10] & 0x08) != 0;
        r_[kNr10] = v;
        if (oldNegate && !(v & 0x08) && negUsed_) ch_[0].on = false;
        return;
    }

    const int i = idx / 5;
    switch (idx % 5) {
        case 0:  // NR30 (у остальных каналов байт не используется)
            r_[idx] = v;
            if (i == 2 && !(v & 0x80)) ch_[2].on = false;  // ЦАП волнового канала выключен
            return;
        case 1:  // длительность (и скважность у прямоугольных каналов)
            r_[idx] = v;
            writeLength(i, v);
            return;
        case 2:  // огибающая (у волнового канала: громкость)
            r_[idx] = v;
            if (i != 2 && !dacOn(i)) ch_[i].on = false;  // старшие 5 бит = 0 выключают ЦАП и канал
            return;
        case 3:  // младшие биты частоты (у шумового: параметры генератора)
            r_[idx] = v;
            return;
        default:
            writeControl(i, v);
            return;
    }
}

void Apu::writeLength(int i, uint8_t v) {
    ch_[i].length = maxLength(i) - (i == 2 ? v : (v & 0x3F));
}

void Apu::writeControl(int i, uint8_t v) {
    Channel& c = ch_[i];
    const bool lengthWasOn = (r_[5 * i + 4] & 0x40) != 0;
    const bool lengthOn = (v & 0x40) != 0;
    r_[5 * i + 4] = v;

    // Если следующий шаг секвенсора не тактирует длительность, включение длительности
    // сразу даёт лишний такт счётчика (особенность железа)
    const bool noLengthClockNext = (fsStep_ & 1) != 0;
    if (noLengthClockNext && !lengthWasOn && lengthOn && c.length > 0) {
        if (--c.length == 0 && !(v & 0x80)) c.on = false;
    }

    if (v & 0x80) {
        trigger(i);
        if (c.length == 0) {
            c.length = maxLength(i);
            if (lengthOn && noLengthClockNext) --c.length;
        }
    }
}

void Apu::trigger(int i) {
    Channel& c = ch_[i];
    c.on = dacOn(i);
    switch (i) {
        case 0:
        case 1:
            c.timer = (2048 - freq(i)) * 4;
            break;
        case 2:
            c.timer = (2048 - freq(2)) * 2 + 6;  // после запуска первый шаг наступает на 6 тактов позже
            c.pos = 0;
            break;
        default:
            c.timer = kNoiseDiv[r_[kNr43] & 7] << (r_[kNr43] >> 4);
            lfsr_ = 0x7FFF;
            break;
    }
    if (i != 2) {
        const uint8_t env = r_[5 * i + 2];
        c.volume = env >> 4;
        c.envTimer = (env & 7) ? (env & 7) : 8;
    }
    if (i == 0) {
        const int period = (r_[kNr10] >> 4) & 7, shift = r_[kNr10] & 7;
        shadow_ = freq(0);
        sweepTimer_ = period ? period : 8;
        sweepOn_ = period != 0 || shift != 0;
        negUsed_ = false;
        if (shift) sweepCalc();  // проверка переполнения сразу при запуске
    }
}

void Apu::powerOff() {
    for (int idx = 0; idx <= kNr51; ++idx) r_[idx] = 0;
    for (auto& c : ch_) {
        c.on = false;
        if (cgb_) c.length = 0;  // на CGB счётчики длительности тоже сбрасываются
    }
    sweepOn_ = false;
    negUsed_ = false;
    power_ = false;
}

// ---------- секвенсор ----------

int Apu::sweepCalc() {
    const int delta = shadow_ >> (r_[kNr10] & 7);
    int next;
    if (r_[kNr10] & 0x08) {
        next = shadow_ - delta;
        negUsed_ = true;
    } else {
        next = shadow_ + delta;
    }
    if (next > 2047) ch_[0].on = false;
    return next;
}

void Apu::clockLength() {
    for (int i = 0; i < 4; ++i) {
        Channel& c = ch_[i];
        if ((r_[5 * i + 4] & 0x40) && c.length > 0 && --c.length == 0) c.on = false;
    }
}

void Apu::clockSweep() {
    if (--sweepTimer_ > 0) return;
    const int period = (r_[kNr10] >> 4) & 7;
    sweepTimer_ = period ? period : 8;
    if (!sweepOn_ || period == 0) return;
    const int next = sweepCalc();
    if (next <= 2047 && (r_[kNr10] & 7)) {
        shadow_ = next;
        r_[0x03] = static_cast<uint8_t>(next & 0xFF);
        r_[0x04] = static_cast<uint8_t>((r_[0x04] & ~7) | (next >> 8));
        sweepCalc();  // повторная проверка переполнения с новым значением
    }
}

void Apu::clockEnvelope() {
    for (int i : {0, 1, 3}) {
        Channel& c = ch_[i];
        const uint8_t env = r_[5 * i + 2];
        const int period = env & 7;
        if (period == 0) continue;  // период 0 = огибающая стоит
        if (--c.envTimer > 0) continue;
        c.envTimer = period;
        if ((env & 0x08) && c.volume < 15) ++c.volume;
        else if (!(env & 0x08) && c.volume > 0) --c.volume;
    }
}

void Apu::frameSequencerStep() {
    if (!power_) return;
    switch (fsStep_) {
        case 0: case 4: clockLength(); break;
        case 2: case 6: clockLength(); clockSweep(); break;
        case 7: clockEnvelope(); break;
        default: break;
    }
    fsStep_ = (fsStep_ + 1) & 7;
}

// ---------- генерация ----------

int Apu::digitalOut(int i) const {
    const Channel& c = ch_[i];
    if (!c.on) return 0;
    switch (i) {
        case 0:
        case 1:
            return ((kDuty[r_[5 * i + 1] >> 6] >> c.pos) & 1) ? c.volume : 0;
        case 2: {
            const int code = (r_[kNr32] >> 5) & 3;  // 0 = тишина, 1 = 100 %, 2 = 50 %, 3 = 25 %
            return code ? (waveSample_ >> (code - 1)) : 0;
        }
        default:
            return (lfsr_ & 1) ? 0 : c.volume;
    }
}

void Apu::tick(int cycles) {
    double left = 0.0, right = 0.0;

    if (power_) {
        for (int i = 0; i < 2; ++i) {
            Channel& c = ch_[i];
            if (!c.on) continue;
            c.timer -= cycles;
            while (c.timer <= 0) {
                c.timer += (2048 - freq(i)) * 4;
                c.pos = (c.pos + 1) & 7;
            }
        }
        if (ch_[2].on) {
            Channel& c = ch_[2];
            c.timer -= cycles;
            while (c.timer <= 0) {
                c.timer += (2048 - freq(2)) * 2;
                c.pos = (c.pos + 1) & 31;
                waveSample_ = static_cast<uint8_t>((wave_[c.pos >> 1] >> ((c.pos & 1) ? 0 : 4)) & 0x0F);
            }
        }
        if (ch_[3].on && (r_[kNr43] >> 4) < 14) {  // сдвиг 14 и 15 останавливает генератор
            Channel& c = ch_[3];
            c.timer -= cycles;
            while (c.timer <= 0) {
                c.timer += kNoiseDiv[r_[kNr43] & 7] << (r_[kNr43] >> 4);
                const uint16_t bit = static_cast<uint16_t>((lfsr_ ^ (lfsr_ >> 1)) & 1);
                lfsr_ = static_cast<uint16_t>((lfsr_ >> 1) | (bit << 14));
                if (r_[kNr43] & 0x08) lfsr_ = static_cast<uint16_t>((lfsr_ & ~(1 << 6)) | (bit << 6));  // 7-битный режим
            }
        }

        // Микшер: ЦАП переводит 0..15 в 0..1, NR51 раскладывает каналы по сторонам, NR50 задаёт громкость сторон.
        // Ноль и выключенный ЦАП дают одинаковый уровень, поэтому включение/выключение ЦАП не щёлкает
        // (на железе щелчок есть, но слушать его незачем); постоянную составляющую нот убирает фильтр ниже.
        const uint8_t pan = r_[kNr51];
        for (int i = 0; i < 4; ++i) {
            if (!dacOn(i)) continue;
            const double a = digitalOut(i) / 15.0;
            if (pan & (0x10 << i)) left += a;
            if (pan & (0x01 << i)) right += a;
        }
        left *= (((r_[kNr50] >> 4) & 7) + 1) / 8.0;
        right *= ((r_[kNr50] & 7) + 1) / 8.0;
    }

    // Отсчёт выхода = среднее значение за его интервал (простейший фильтр против наложения частот)
    sumL_ += left * cycles;
    sumR_ += right * cycles;
    sampleClock_ += cycles;
    while (sampleClock_ >= cyclesPerSample_) {
        const double excess = sampleClock_ - cyclesPerSample_;
        const double inL = (sumL_ - left * excess) / cyclesPerSample_;
        const double inR = (sumR_ - right * excess) / cyclesPerSample_;
        sumL_ = left * excess;
        sumR_ = right * excess;
        sampleClock_ = excess;

        // Фильтр верхних частот как в железе: убирает постоянную составляющую ЦАП
        const double outL = inL - hpL_;
        hpL_ = inL - outL * hpCharge_;
        const double outR = inR - hpR_;
        hpR_ = inR - outR * hpCharge_;

        if (samples_.size() < static_cast<std::size_t>(sampleRate_) * 2) {  // не больше секунды
            const double gain = 32767.0 * volume_ / 2.5;  // 4 канала на полной громкости почти никогда не совпадают
            auto toPcm = [gain](double x) {
                double s = x * gain;
                if (s > 32767.0) s = 32767.0;
                if (s < -32768.0) s = -32768.0;
                return static_cast<int16_t>(s);
            };
            samples_.push_back(toPcm(outL));
            samples_.push_back(toPcm(outR));
        }
    }
}

}  // namespace gb
