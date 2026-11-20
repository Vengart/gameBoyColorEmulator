#include "Ppu.h"
#include <algorithm>

namespace gb {

namespace {

// Оттенки DMG (0 = светлый, 3 = тёмный). Для серой гаммы замените на 0xFFFFFFFF, 0xFFAAAAAA, 0xFF555555, 0xFF000000.
constexpr uint32_t kDmgShades[4] = {0xFFE0F8D0, 0xFF88C070, 0xFF346856, 0xFF081820};

uint32_t dmgColor(uint8_t palette, int index) {
    return kDmgShades[(palette >> (index * 2)) & 3];
}

// Цвет CGB: 15 бит RGB555, хранится двумя байтами (младший первым)
uint32_t cgbColor(const std::array<uint8_t, 64>& pal, int paletteNo, int index) {
    const int o = paletteNo * 8 + index * 2;
    const uint16_t c = static_cast<uint16_t>(pal[o] | (pal[o + 1] << 8));
    auto expand = [](int v) { return static_cast<uint32_t>((v << 3) | (v >> 2)); };  // 5 бит -> 8 бит
    return 0xFF000000u | (expand(c & 31) << 16) | (expand((c >> 5) & 31) << 8) | expand((c >> 10) & 31);
}

}  // namespace

void Ppu::reset() {
    vram_.fill(0);
    oam_.fill(0);
    // Если игра не задаёт палитры CGB сама, используем серую шкалу (белый, светло-серый, тёмно-серый, чёрный)
    static constexpr uint16_t grey[4] = {0x7FFF, 0x56B5, 0x294A, 0x0000};
    for (int p = 0; p < 8; ++p)
        for (int c = 0; c < 4; ++c) {
            bgPal_[p * 8 + c * 2] = objPal_[p * 8 + c * 2] = grey[c] & 0xFF;
            bgPal_[p * 8 + c * 2 + 1] = objPal_[p * 8 + c * 2 + 1] = grey[c] >> 8;
        }
    lcdc_ = 0x91; stat_ = 0; scy_ = scx_ = 0; ly_ = 0; lyc_ = 0;
    bgp_ = 0xFC; obp0_ = obp1_ = 0xFF; wy_ = wx_ = 0;
    vramBank_ = 0; bcps_ = ocps_ = 0;
    dot_ = 0; mode_ = 2; windowLine_ = 0;
    lycEqual_ = statLine_ = false;
    frameCount_ = 0;
    frame_.fill(blankColor());
    presented_.fill(blankColor());
}

uint32_t Ppu::blankColor() const { return cgb_ ? 0xFFFFFFFFu : kDmgShades[0]; }

void Ppu::turnOff() {
    ly_ = 0; dot_ = 0; mode_ = 0; windowLine_ = 0;
    statLine_ = false;
    lycEqual_ = false;
    frame_.fill(blankColor());       // выключенный экран показывает пустой цвет
    presented_.fill(blankColor());
}

void Ppu::turnOn() {
    ly_ = 0; dot_ = 0; mode_ = 2; windowLine_ = 0;
    statLine_ = false;
}

// ---------- регистры ----------

uint8_t Ppu::readReg(uint16_t addr) const {
    switch (addr) {
        case 0xFF40: return lcdc_;
        case 0xFF41: return static_cast<uint8_t>(0x80 | (stat_ & 0x78) | (lycEqual_ ? 4 : 0) | mode());
        case 0xFF42: return scy_;
        case 0xFF43: return scx_;
        case 0xFF44: return ly_;
        case 0xFF45: return lyc_;
        case 0xFF47: return bgp_;
        case 0xFF48: return obp0_;
        case 0xFF49: return obp1_;
        case 0xFF4A: return wy_;
        case 0xFF4B: return wx_;
        case 0xFF4F: return cgb_ ? static_cast<uint8_t>(0xFE | vramBank_) : 0xFF;
        case 0xFF68: return cgb_ ? static_cast<uint8_t>(bcps_ | 0x40) : 0xFF;
        case 0xFF69: return cgb_ ? bgPal_[bcps_ & 0x3F] : 0xFF;
        case 0xFF6A: return cgb_ ? static_cast<uint8_t>(ocps_ | 0x40) : 0xFF;
        case 0xFF6B: return cgb_ ? objPal_[ocps_ & 0x3F] : 0xFF;
        default: return 0xFF;
    }
}

void Ppu::writeReg(uint16_t addr, uint8_t v) {
    switch (addr) {
        case 0xFF40: {
            const bool wasOn = lcdOn();
            lcdc_ = v;
            if (wasOn && !lcdOn()) turnOff();
            else if (!wasOn && lcdOn()) turnOn();
            break;
        }
        case 0xFF41: stat_ = v & 0x78; break;
        case 0xFF42: scy_ = v; break;
        case 0xFF43: scx_ = v; break;
        case 0xFF44: break;  // LY только для чтения
        case 0xFF45: lyc_ = v; if (lcdOn()) lycEqual_ = (ly_ == lyc_); break;
        case 0xFF47: bgp_ = v; break;
        case 0xFF48: obp0_ = v; break;
        case 0xFF49: obp1_ = v; break;
        case 0xFF4A: wy_ = v; break;
        case 0xFF4B: wx_ = v; break;
        case 0xFF4F: if (cgb_) vramBank_ = v & 1; break;
        // Индекс палитры (бит 7 = автоинкремент после записи) и данные цвета
        case 0xFF68: if (cgb_) bcps_ = v & 0xBF; break;
        case 0xFF6A: if (cgb_) ocps_ = v & 0xBF; break;
        case 0xFF69:
        case 0xFF6B: {
            if (!cgb_) break;
            uint8_t& spec = (addr == 0xFF69) ? bcps_ : ocps_;
            auto& pal = (addr == 0xFF69) ? bgPal_ : objPal_;
            pal[spec & 0x3F] = v;
            if (spec & 0x80) spec = static_cast<uint8_t>(0x80 | ((spec + 1) & 0x3F));
            break;
        }
        default: break;
    }
}

// ---------- время ----------

uint8_t Ppu::tick(int dots) {
    if (!lcdOn()) return 0;
    uint8_t irq = 0;
    for (int i = 0; i < dots; ++i) {
        ++dot_;
        if (ly_ < kScreenH) {
            if (dot_ == 80) {
                mode_ = 3;                 // начало отрисовки
            } else if (dot_ == 252) {
                renderLine();              // строка рисуется целиком в конце режима 3
                mode_ = 0;                 // HBlank
                irq |= 4;                  // сигнал для HDMA: можно передать 16 байт
            }
        }
        if (dot_ == static_cast<int>(kDotsPerLine)) {
            dot_ = 0;
            ++ly_;
            if (ly_ == kScreenH) {
                mode_ = 1;                 // VBlank
                irq |= 1;
                presented_ = frame_;       // кадр готов
                ++frameCount_;
            } else if (ly_ == static_cast<int>(kLinesPerFrame)) {
                ly_ = 0;
                windowLine_ = 0;
            }
            if (ly_ < kScreenH) mode_ = 2;
        }
        lycEqual_ = (ly_ == lyc_);

        // Прерывание STAT срабатывает по переходу общего сигнала из 0 в 1
        const bool line = ((stat_ & 0x40) && lycEqual_) || ((stat_ & 0x08) && mode_ == 0) ||
                          ((stat_ & 0x10) && mode_ == 1) || ((stat_ & 0x20) && mode_ == 2);
        if (line && !statLine_) irq |= 2;
        statLine_ = line;
    }
    return irq;
}

// ---------- отрисовка строки ----------

void Ppu::renderLine() {
    uint32_t* out = &frame_[ly_ * kScreenW];
    std::array<uint8_t, kScreenW> bgColor{};  // индекс цвета фона 0-3, нужен для приоритета спрайтов
    std::array<bool, kScreenW> bgPrio{};      // атрибут "фон поверх спрайтов" (CGB)

    // На DMG бит 0 LCDC отключает фон и окно, на CGB он лишь управляет приоритетом спрайтов
    const bool bgOn = cgb_ || (lcdc_ & 0x01);
    const bool winOn = bgOn && (lcdc_ & 0x20) && ly_ >= wy_ && wx_ <= 166;
    bool windowUsed = false;

    for (int x = 0; x < kScreenW; ++x) {
        if (!bgOn) {
            out[x] = kDmgShades[0];
            continue;
        }
        const int winX = wx_ - 7;
        const bool inWindow = winOn && x >= winX;
        int px, py, mapBase;
        if (inWindow) {
            px = x - winX;
            py = windowLine_;
            mapBase = (lcdc_ & 0x40) ? 0x1C00 : 0x1800;
            windowUsed = true;
        } else {
            px = (x + scx_) & 0xFF;
            py = (ly_ + scy_) & 0xFF;
            mapBase = (lcdc_ & 0x08) ? 0x1C00 : 0x1800;
        }

        // Номер тайла лежит в банке 0, атрибуты (CGB) в банке 1 по тому же адресу
        const int mapOff = mapBase + (py >> 3) * 32 + (px >> 3);
        const uint8_t tileIdx = vram_[mapOff];
        const uint8_t attr = cgb_ ? vram_[0x2000 + mapOff] : 0;

        int row = py & 7;
        if (attr & 0x40) row = 7 - row;                                                // отражение по вертикали
        int tileAddr = (lcdc_ & 0x10) ? tileIdx * 16 : 0x1000 + static_cast<int8_t>(tileIdx) * 16;
        tileAddr += ((attr & 0x08) ? 0x2000 : 0) + row * 2;                            // банк тайла + строка
        const uint8_t lo = vram_[tileAddr], hi = vram_[tileAddr + 1];
        const int bit = (attr & 0x20) ? (px & 7) : 7 - (px & 7);                       // отражение по горизонтали
        const int color = (((hi >> bit) & 1) << 1) | ((lo >> bit) & 1);

        bgColor[x] = static_cast<uint8_t>(color);
        bgPrio[x] = (attr & 0x80) != 0;
        out[x] = cgb_ ? cgbColor(bgPal_, attr & 7, color) : dmgColor(bgp_, color);
    }
    if (windowUsed) ++windowLine_;

    // ----- спрайты -----
    if (!(lcdc_ & 0x02)) return;
    const int height = (lcdc_ & 0x04) ? 16 : 8;

    struct Sprite { int x; int index; };
    std::array<Sprite, 10> list{};
    int count = 0;
    for (int i = 0; i < 40 && count < 10; ++i) {  // не более 10 спрайтов на строку, в порядке OAM
        const int y = oam_[i * 4] - 16;
        if (ly_ >= y && ly_ < y + height) list[count++] = {oam_[i * 4 + 1] - 8, i};
    }
    // Приоритет: на DMG левее = важнее (при равенстве меньший номер), на CGB только номер в OAM
    if (!cgb_) std::stable_sort(list.begin(), list.begin() + count, [](const Sprite& a, const Sprite& b) { return a.x < b.x; });

    std::array<bool, kScreenW> taken{};
    for (int k = 0; k < count; ++k) {
        const uint8_t* o = &oam_[list[k].index * 4];
        const uint8_t attr = o[3];
        int tile = o[2];
        int row = ly_ - (o[0] - 16);
        if (attr & 0x40) row = height - 1 - row;
        if (height == 16) {
            tile = (tile & 0xFE) + (row >> 3);
            row &= 7;
        }
        const int addr = tile * 16 + row * 2 + ((cgb_ && (attr & 0x08)) ? 0x2000 : 0);
        const uint8_t lo = vram_[addr], hi = vram_[addr + 1];

        for (int px = 0; px < 8; ++px) {
            const int x = list[k].x + px;
            if (x < 0 || x >= kScreenW || taken[x]) continue;
            const int bit = (attr & 0x20) ? px : 7 - px;
            const int color = (((hi >> bit) & 1) << 1) | ((lo >> bit) & 1);
            if (color == 0) continue;  // цвет 0 у спрайтов прозрачный
            taken[x] = true;           // пиксель занят, даже если спрайт спрятан за фоном

            bool behindBg;
            if (cgb_) behindBg = (lcdc_ & 0x01) && bgColor[x] != 0 && ((attr & 0x80) || bgPrio[x]);
            else      behindBg = (attr & 0x80) && bgColor[x] != 0;
            if (behindBg) continue;

            out[x] = cgb_ ? cgbColor(objPal_, attr & 7, color)
                          : dmgColor((attr & 0x10) ? obp1_ : obp0_, color);
        }
    }
}

}  // namespace gb
