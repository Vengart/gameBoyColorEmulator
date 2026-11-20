#pragma once
#include <cstdint>

namespace gb {

constexpr uint32_t kClockHz       = 4194304;  // T-такты в секунду (обычная скорость)
constexpr uint32_t kTCyclesPerM   = 4;        // один M-такт = 4 T-такта
constexpr uint32_t kDotsPerLine   = 456;      // точек PPU в строке
constexpr uint32_t kLinesPerFrame = 154;      // 144 видимые + 10 строк VBlank
constexpr uint32_t kDotsPerFrame  = kDotsPerLine * kLinesPerFrame;  // 70224

}  // namespace gb
