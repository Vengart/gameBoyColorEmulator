#include "Cpu.h"

namespace gb {

void Cpu::reset() {
    r_ = Registers{};
    if (bus_.cgbMode) {  // значения регистров после загрузочной ROM Game Boy Color (A = 0x11 выдаёт CGB)
        r_.a = 0x11; r_.f = 0x80;
        r_.setBc(0x0000); r_.setDe(0xFF56); r_.setHl(0x000D);
    } else {             // оригинальный Game Boy (DMG)
        r_.a = 0x01; r_.f = 0xB0;
        r_.setBc(0x0013); r_.setDe(0x00D8); r_.setHl(0x014D);
    }
    r_.sp = 0xFFFE;
    r_.pc = 0x0100;
    ime_ = false; eiCountdown_ = 0; halted_ = false; haltBug_ = false;
}

// ---------- вспомогательные операции ----------

void Cpu::push16(uint16_t v) {
    write8(--r_.sp, static_cast<uint8_t>(v >> 8));
    write8(--r_.sp, static_cast<uint8_t>(v & 0xFF));
}

uint16_t Cpu::pop16() {
    uint8_t lo = read8(r_.sp++);
    uint8_t hi = read8(r_.sp++);
    return static_cast<uint16_t>(hi << 8 | lo);
}

uint8_t Cpu::get8(int code) {
    switch (code) {
        case 0: return r_.b;
        case 1: return r_.c;
        case 2: return r_.d;
        case 3: return r_.e;
        case 4: return r_.h;
        case 5: return r_.l;
        case 6: return read8(r_.hl());  // (HL) стоит дополнительный M-такт
        default: return r_.a;
    }
}

void Cpu::set8(int code, uint8_t v) {
    switch (code) {
        case 0: r_.b = v; break;
        case 1: r_.c = v; break;
        case 2: r_.d = v; break;
        case 3: r_.e = v; break;
        case 4: r_.h = v; break;
        case 5: r_.l = v; break;
        case 6: write8(r_.hl(), v); break;
        default: r_.a = v; break;
    }
}

uint16_t Cpu::getRr(int code) const {
    switch (code) {
        case 0: return r_.bc();
        case 1: return r_.de();
        case 2: return r_.hl();
        default: return r_.sp;
    }
}

void Cpu::setRr(int code, uint16_t v) {
    switch (code) {
        case 0: r_.setBc(v); break;
        case 1: r_.setDe(v); break;
        case 2: r_.setHl(v); break;
        default: r_.sp = v; break;
    }
}

bool Cpu::condition(int cc) const {
    switch (cc) {
        case 0: return !(r_.f & kFlagZ);
        case 1: return (r_.f & kFlagZ) != 0;
        case 2: return !(r_.f & kFlagC);
        default: return (r_.f & kFlagC) != 0;
    }
}

void Cpu::setFlags(bool z, bool n, bool h, bool c) {
    r_.f = static_cast<uint8_t>((z ? kFlagZ : 0) | (n ? kFlagN : 0) | (h ? kFlagH : 0) | (c ? kFlagC : 0));
}

// ---------- арифметика ----------

void Cpu::alu(int op, uint8_t v) {
    const uint8_t a = r_.a;
    const int carry = (r_.f & kFlagC) ? 1 : 0;
    switch (op) {
        case 0: {  // ADD
            int res = a + v;
            setFlags((res & 0xFF) == 0, false, ((a & 0xF) + (v & 0xF)) > 0xF, res > 0xFF);
            r_.a = static_cast<uint8_t>(res);
            break;
        }
        case 1: {  // ADC
            int res = a + v + carry;
            setFlags((res & 0xFF) == 0, false, ((a & 0xF) + (v & 0xF) + carry) > 0xF, res > 0xFF);
            r_.a = static_cast<uint8_t>(res);
            break;
        }
        case 2: case 7: {  // SUB и CP (CP не сохраняет результат)
            int res = a - v;
            setFlags((res & 0xFF) == 0, true, (a & 0xF) < (v & 0xF), a < v);
            if (op == 2) r_.a = static_cast<uint8_t>(res);
            break;
        }
        case 3: {  // SBC
            int res = a - v - carry;
            setFlags((res & 0xFF) == 0, true, (a & 0xF) < ((v & 0xF) + carry), a < (v + carry));
            r_.a = static_cast<uint8_t>(res);
            break;
        }
        case 4: r_.a = a & v; setFlags(r_.a == 0, false, true, false); break;   // AND
        case 5: r_.a = a ^ v; setFlags(r_.a == 0, false, false, false); break;  // XOR
        default: r_.a = a | v; setFlags(r_.a == 0, false, false, false); break; // OR
    }
}

uint8_t Cpu::inc8(uint8_t v) {
    uint8_t res = static_cast<uint8_t>(v + 1);
    r_.f = static_cast<uint8_t>((r_.f & kFlagC) | (res == 0 ? kFlagZ : 0) | ((v & 0xF) == 0xF ? kFlagH : 0));
    return res;
}

uint8_t Cpu::dec8(uint8_t v) {
    uint8_t res = static_cast<uint8_t>(v - 1);
    r_.f = static_cast<uint8_t>((r_.f & kFlagC) | kFlagN | (res == 0 ? kFlagZ : 0) | ((v & 0xF) == 0 ? kFlagH : 0));
    return res;
}

void Cpu::daa() {
    uint8_t a = r_.a;
    const bool n = r_.f & kFlagN, h = r_.f & kFlagH;
    bool c = r_.f & kFlagC;
    if (!n) {
        if (c || a > 0x99) { a = static_cast<uint8_t>(a + 0x60); c = true; }
        if (h || (a & 0x0F) > 9) a = static_cast<uint8_t>(a + 0x06);
    } else {
        if (c) a = static_cast<uint8_t>(a - 0x60);
        if (h) a = static_cast<uint8_t>(a - 0x06);
    }
    r_.a = a;
    setFlags(a == 0, n, false, c);
}

uint8_t Cpu::rotateShift(int op, uint8_t v) {
    const bool oldC = r_.f & kFlagC;
    uint8_t res = 0;
    bool c = false;
    switch (op) {
        case 0: c = v & 0x80; res = static_cast<uint8_t>((v << 1) | (c ? 1 : 0)); break;       // RLC
        case 1: c = v & 0x01; res = static_cast<uint8_t>((v >> 1) | (c ? 0x80 : 0)); break;    // RRC
        case 2: c = v & 0x80; res = static_cast<uint8_t>((v << 1) | (oldC ? 1 : 0)); break;    // RL
        case 3: c = v & 0x01; res = static_cast<uint8_t>((v >> 1) | (oldC ? 0x80 : 0)); break; // RR
        case 4: c = v & 0x80; res = static_cast<uint8_t>(v << 1); break;                       // SLA
        case 5: c = v & 0x01; res = static_cast<uint8_t>((v >> 1) | (v & 0x80)); break;        // SRA
        case 6: c = false; res = static_cast<uint8_t>((v << 4) | (v >> 4)); break;             // SWAP
        default: c = v & 0x01; res = static_cast<uint8_t>(v >> 1); break;                      // SRL
    }
    setFlags(res == 0, false, false, c);
    return res;
}

uint16_t Cpu::addSpOffset(int8_t e) {
    const uint16_t sp = r_.sp;
    const uint16_t u = static_cast<uint8_t>(e);
    // Флаги считаются по младшему байту как при беззнаковом сложении
    setFlags(false, false, ((sp & 0xF) + (u & 0xF)) > 0xF, ((sp & 0xFF) + u) > 0xFF);
    return static_cast<uint16_t>(sp + e);
}

// ---------- префикс CB ----------

void Cpu::executeCb() {
    const uint8_t op = fetch8();
    const int target = op & 7;
    const int bit = (op >> 3) & 7;
    switch (op >> 6) {
        case 0:  // сдвиги и вращения: чтение и запись (HL) = 2 доп. такта
            set8(target, rotateShift(bit, get8(target)));
            break;
        case 1: {  // BIT: для (HL) только чтение
            const uint8_t v = get8(target);
            r_.f = static_cast<uint8_t>((r_.f & kFlagC) | kFlagH | ((v & (1 << bit)) ? 0 : kFlagZ));
            break;
        }
        case 2: set8(target, static_cast<uint8_t>(get8(target) & ~(1 << bit))); break;  // RES
        default: set8(target, static_cast<uint8_t>(get8(target) | (1 << bit))); break;  // SET
    }
}

// ---------- прерывания ----------

int Cpu::serviceInterrupt() {
    const uint64_t start = bus_.cpuCycles();
    const uint8_t pending = bus_.pendingInterrupts();
    int bit = 0;
    while (!(pending & (1 << bit))) ++bit;  // приоритет у младшего бита
    ime_ = false;
    bus_.acknowledgeInterrupt(bit);
    internal();
    internal();
    push16(r_.pc);
    r_.pc = static_cast<uint16_t>(0x40 + 8 * bit);
    internal();
    return static_cast<int>((bus_.cpuCycles() - start) / kTCyclesPerM);  // всего 5 M-тактов
}

// ---------- выполнение одной инструкции ----------

int Cpu::step() {
    const uint64_t start = bus_.cpuCycles();
    auto elapsed = [&] { return static_cast<int>((bus_.cpuCycles() - start) / kTCyclesPerM); };

    bus_.serviceHdma();  // HDMA останавливает процессор на время копирования

    if (halted_) {
        if (bus_.pendingInterrupts()) halted_ = false;
        else { internal(); return 1; }
    }
    if (ime_ && bus_.pendingInterrupts()) return serviceInterrupt();

    uint8_t op;
    if (haltBug_) {  // ошибка HALT: PC не увеличивается после выборки
        op = read8(r_.pc);
        haltBug_ = false;
    } else {
        op = fetch8();
    }

    if (op >= 0x40 && op < 0x80) {
        if (op == 0x76) {  // HALT
            if (!ime_ && bus_.pendingInterrupts()) haltBug_ = true;
            else halted_ = true;
        } else {
            set8((op >> 3) & 7, get8(op & 7));  // LD r, r'
        }
    } else if (op >= 0x80 && op < 0xC0) {
        alu((op >> 3) & 7, get8(op & 7));
    } else if (op < 0x40) {
        const int rr = (op >> 4) & 3;
        switch (op & 7) {
            case 0:
                switch (op) {
                    case 0x00: break;  // NOP
                    case 0x08: {       // LD (nn), SP: 5 M-тактов
                        uint16_t nn = fetch16();
                        write8(nn, static_cast<uint8_t>(r_.sp & 0xFF));
                        write8(static_cast<uint16_t>(nn + 1), static_cast<uint8_t>(r_.sp >> 8));
                        break;
                    }
                    case 0x10:  // STOP: при взведённом KEY1 переключает скорость
                        fetch8();
                        bus_.toggleSpeedIfArmed();
                        break;
                    case 0x18: {  // JR e: 3 M-такта
                        int8_t e = static_cast<int8_t>(fetch8());
                        internal();
                        r_.pc = static_cast<uint16_t>(r_.pc + e);
                        break;
                    }
                    default: {  // JR cc, e: 2 M-такта, 3 если условие выполнено
                        int8_t e = static_cast<int8_t>(fetch8());
                        if (condition((op >> 3) & 3)) {
                            internal();
                            r_.pc = static_cast<uint16_t>(r_.pc + e);
                        }
                    }
                }
                break;
            case 1:
                if (op & 8) {  // ADD HL, rr: 2 M-такта
                    const uint16_t hl = r_.hl(), v = getRr(rr);
                    const uint32_t res = static_cast<uint32_t>(hl) + v;
                    r_.f = static_cast<uint8_t>((r_.f & kFlagZ) | (((hl & 0xFFF) + (v & 0xFFF)) > 0xFFF ? kFlagH : 0) |
                                                (res > 0xFFFF ? kFlagC : 0));
                    r_.setHl(static_cast<uint16_t>(res));
                    internal();
                } else {       // LD rr, nn: 3 M-такта
                    setRr(rr, fetch16());
                }
                break;
            case 2: {  // LD (rr), A / LD A, (rr), включая HL+ и HL-
                uint16_t addr = rr == 0 ? r_.bc() : rr == 1 ? r_.de() : r_.hl();
                if (op & 8) r_.a = read8(addr); else write8(addr, r_.a);
                // Коды пары: 2 = (HL+) для 0x22/0x2A, 3 = (HL-) для 0x32/0x3A
                if (rr >= 2) r_.setHl(static_cast<uint16_t>(r_.hl() + (rr == 2 ? 1 : -1)));
                break;
            }
            case 3:  // INC rr / DEC rr: 2 M-такта
                setRr(rr, static_cast<uint16_t>(getRr(rr) + ((op & 8) ? -1 : 1)));
                internal();
                break;
            case 4: set8((op >> 3) & 7, inc8(get8((op >> 3) & 7))); break;  // INC r (для (HL): чтение + запись)
            case 5: set8((op >> 3) & 7, dec8(get8((op >> 3) & 7))); break;  // DEC r
            case 6: { uint8_t n = fetch8(); set8((op >> 3) & 7, n); break; } // LD r, n
            default:
                switch (op) {
                    case 0x07: r_.a = rotateShift(0, r_.a); r_.f &= static_cast<uint8_t>(~kFlagZ); break;  // RLCA
                    case 0x0F: r_.a = rotateShift(1, r_.a); r_.f &= static_cast<uint8_t>(~kFlagZ); break;  // RRCA
                    case 0x17: r_.a = rotateShift(2, r_.a); r_.f &= static_cast<uint8_t>(~kFlagZ); break;  // RLA
                    case 0x1F: r_.a = rotateShift(3, r_.a); r_.f &= static_cast<uint8_t>(~kFlagZ); break;  // RRA
                    case 0x27: daa(); break;
                    case 0x2F: r_.a = static_cast<uint8_t>(~r_.a); r_.f |= kFlagN | kFlagH; break;         // CPL
                    case 0x37: r_.f = static_cast<uint8_t>((r_.f & kFlagZ) | kFlagC); break;               // SCF
                    default:   r_.f = static_cast<uint8_t>((r_.f & kFlagZ) | ((r_.f & kFlagC) ? 0 : kFlagC)); break;  // CCF
                }
        }
    } else {
        switch (op) {
            case 0xC0: case 0xC8: case 0xD0: case 0xD8:  // RET cc: 2 M-такта, 5 если условие выполнено
                internal();
                if (condition((op >> 3) & 3)) { r_.pc = pop16(); internal(); }
                break;
            case 0xC1: case 0xD1: case 0xE1: case 0xF1: {  // POP rr: 3 M-такта
                uint16_t v = pop16();
                switch ((op >> 4) & 3) {
                    case 0: r_.setBc(v); break;
                    case 1: r_.setDe(v); break;
                    case 2: r_.setHl(v); break;
                    default: r_.setAf(v); break;
                }
                break;
            }
            case 0xC2: case 0xCA: case 0xD2: case 0xDA: {  // JP cc, nn: 3 M-такта, 4 если условие выполнено
                uint16_t nn = fetch16();
                if (condition((op >> 3) & 3)) { internal(); r_.pc = nn; }
                break;
            }
            case 0xC3: { uint16_t nn = fetch16(); internal(); r_.pc = nn; break; }  // JP nn: 4
            case 0xC4: case 0xCC: case 0xD4: case 0xDC: {  // CALL cc, nn: 3 M-такта, 6 если условие выполнено
                uint16_t nn = fetch16();
                if (condition((op >> 3) & 3)) { internal(); push16(r_.pc); r_.pc = nn; }
                break;
            }
            case 0xC5: case 0xD5: case 0xE5: case 0xF5: {  // PUSH rr: 4 M-такта
                uint16_t v;
                switch ((op >> 4) & 3) {
                    case 0: v = r_.bc(); break;
                    case 1: v = r_.de(); break;
                    case 2: v = r_.hl(); break;
                    default: v = r_.af(); break;
                }
                internal();
                push16(v);
                break;
            }
            case 0xC6: case 0xCE: case 0xD6: case 0xDE: case 0xE6: case 0xEE: case 0xF6: case 0xFE:
                alu((op >> 3) & 7, fetch8());
                break;
            case 0xC7: case 0xCF: case 0xD7: case 0xDF: case 0xE7: case 0xEF: case 0xF7: case 0xFF:  // RST: 4
                internal();
                push16(r_.pc);
                r_.pc = static_cast<uint16_t>(op & 0x38);
                break;
            case 0xC9: r_.pc = pop16(); internal(); break;  // RET: 4
            case 0xD9: r_.pc = pop16(); internal(); ime_ = true; eiCountdown_ = 0; break;  // RETI: 4
            case 0xCB: executeCb(); break;
            case 0xCD: { uint16_t nn = fetch16(); internal(); push16(r_.pc); r_.pc = nn; break; }  // CALL nn: 6
            case 0xE0: { uint8_t n = fetch8(); write8(static_cast<uint16_t>(0xFF00 | n), r_.a); break; }  // LDH (n), A: 3
            case 0xF0: { uint8_t n = fetch8(); r_.a = read8(static_cast<uint16_t>(0xFF00 | n)); break; }    // LDH A, (n): 3
            case 0xE2: write8(static_cast<uint16_t>(0xFF00 | r_.c), r_.a); break;                          // LD (C), A: 2
            case 0xF2: r_.a = read8(static_cast<uint16_t>(0xFF00 | r_.c)); break;                          // LD A, (C): 2
            case 0xE8: {  // ADD SP, e: 4 M-такта
                int8_t e = static_cast<int8_t>(fetch8());
                internal(); internal();
                r_.sp = addSpOffset(e);
                break;
            }
            case 0xE9: r_.pc = r_.hl(); break;  // JP HL: 1
            case 0xEA: { uint16_t nn = fetch16(); write8(nn, r_.a); break; }  // LD (nn), A: 4
            case 0xFA: { uint16_t nn = fetch16(); r_.a = read8(nn); break; }  // LD A, (nn): 4
            case 0xF3: ime_ = false; eiCountdown_ = 0; break;  // DI
            case 0xFB: eiCountdown_ = 2; break;                // EI: IME включается после следующей инструкции
            case 0xF8: {  // LD HL, SP+e: 3 M-такта
                int8_t e = static_cast<int8_t>(fetch8());
                internal();
                r_.setHl(addSpOffset(e));
                break;
            }
            case 0xF9: r_.sp = r_.hl(); internal(); break;  // LD SP, HL: 2
            default:  // неопределённые опкоды (D3, DB, DD, E3, E4, EB, EC, ED, F4, FC, FD)
                unknown_ = op;
                return 0;
        }
    }

    if (eiCountdown_ > 0 && --eiCountdown_ == 0) ime_ = true;
    return elapsed();
}

}  // namespace gb
