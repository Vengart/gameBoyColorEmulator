#pragma once
#include <cstdint>
#include "Bus.h"

namespace gb {

// Биты регистра флагов F
constexpr uint8_t kFlagZ = 0x80, kFlagN = 0x40, kFlagH = 0x20, kFlagC = 0x10;

struct Registers {
    uint8_t a = 0, f = 0, b = 0, c = 0, d = 0, e = 0, h = 0, l = 0;
    uint16_t sp = 0, pc = 0;

    uint16_t af() const { return static_cast<uint16_t>(a << 8 | f); }
    uint16_t bc() const { return static_cast<uint16_t>(b << 8 | c); }
    uint16_t de() const { return static_cast<uint16_t>(d << 8 | e); }
    uint16_t hl() const { return static_cast<uint16_t>(h << 8 | l); }
    void setAf(uint16_t v) { a = v >> 8; f = v & 0xF0; }  // младшие 4 бита F всегда нули
    void setBc(uint16_t v) { b = v >> 8; c = v & 0xFF; }
    void setDe(uint16_t v) { d = v >> 8; e = v & 0xFF; }
    void setHl(uint16_t v) { h = v >> 8; l = v & 0xFF; }
};

class Cpu {
public:
    explicit Cpu(Bus& bus) : bus_(bus) { reset(); }

    void reset();  // состояние после загрузочной ROM (DMG)

    // Выполняет одну инструкцию (или обработку прерывания, или один такт HALT)
    // и возвращает число M-тактов. Возвращает 0, если опкод не определён.
    int step();

    const Registers& regs() const { return r_; }
    uint8_t lastUnknownOpcode() const { return unknown_; }

private:
    Bus& bus_;
    Registers r_;
    uint8_t unknown_ = 0;

    bool ime_ = false;        // общий выключатель прерываний
    int eiCountdown_ = 0;     // EI включает IME только после следующей инструкции
    bool halted_ = false;
    bool haltBug_ = false;    // ошибка HALT: следующий байт читается дважды

    // Каждое обращение к памяти стоит ровно один M-такт
    uint8_t read8(uint16_t addr) { uint8_t v = bus_.read(addr); bus_.tickM(); return v; }
    void write8(uint16_t addr, uint8_t v) { bus_.write(addr, v); bus_.tickM(); }
    uint8_t fetch8() { return read8(r_.pc++); }
    uint16_t fetch16() { uint8_t lo = fetch8(); uint8_t hi = fetch8(); return static_cast<uint16_t>(hi << 8 | lo); }
    void internal() { bus_.tickM(); }  // такт без обращения к памяти

    void push16(uint16_t v);
    uint16_t pop16();

    // Операнд по коду 0-7: B C D E H L (HL) A
    uint8_t get8(int code);
    void set8(int code, uint8_t v);
    // Пара по коду 0-3: BC DE HL SP
    uint16_t getRr(int code) const;
    void setRr(int code, uint16_t v);

    bool condition(int cc) const;  // 0 NZ, 1 Z, 2 NC, 3 C
    void setFlags(bool z, bool n, bool h, bool c);

    void alu(int op, uint8_t v);        // ADD ADC SUB SBC AND XOR OR CP
    uint8_t inc8(uint8_t v);
    uint8_t dec8(uint8_t v);
    void daa();
    uint8_t rotateShift(int op, uint8_t v);  // RLC RRC RL RR SLA SRA SWAP SRL (флаги как у CB-команд)
    uint16_t addSpOffset(int8_t e);          // общая часть ADD SP,e и LD HL,SP+e

    void executeCb();
    int serviceInterrupt();
};

}  // namespace gb
