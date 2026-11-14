// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "video_core/renderer_gxm/usse_vs_gen.h"

#include <array>
#include <cstring>
#include <map>
#include <string>
#include <utility>

namespace Gxm::Usse {
namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s32 = std::int32_t;

// ---------------------------------------------------------------------------
// 1. EL ENSAMBLADOR
// ---------------------------------------------------------------------------

/**
 * Cada familia de instrucciones es un patron de 64 caracteres: '0' y '1' son
 * bits fijos, '-' bits que no se miran y cada letra un campo (los bits de una
 * misma letra, de mas a menos significativo). Son los patrones que documenta
 * Vita3K (usse_translator_entry.cpp), los mismos de gxp_dis.py: con ellos se
 * reconstruyen identicas las 23.495 instrucciones de fragmentos del banco del
 * 03-11, asi que lo que se arma aqui se puede leer de vuelta con usse_sem.py.
 */
struct Family {
    u64 value = 0;
    std::array<std::array<s8, 8>, 128> bits{}; ///< por letra, posiciones (0-63), -1 = fin
    std::array<u8, 128> count{};

    explicit Family(const char* pattern) {
        for (auto& list : bits) {
            list.fill(-1);
        }
        for (u32 i = 0; i < 64; i++) {
            const char ch = pattern[i];
            const u32 bit = 63 - i;
            if (ch == '1') {
                value |= 1ull << bit;
            } else if (ch != '0' && ch != '-') {
                const u32 at = count[static_cast<u8>(ch)]++;
                bits[static_cast<u8>(ch)][at] = static_cast<s8>(bit);
            }
        }
    }

    void Set(u64& word, char field, u32 v) const {
        const u32 n = count[static_cast<u8>(field)];
        for (u32 i = 0; i < n; i++) {
            if ((v >> (n - 1 - i)) & 1u) {
                word |= 1ull << bits[static_cast<u8>(field)][i];
            }
        }
    }

    [[nodiscard]] int Bit(char field) const {
        return bits[static_cast<u8>(field)][0];
    }
};

const Family kV32nmad{"00001pppsrrydcbawwwwneeeemmoiittkkllffffffzzzzzzzggghhhhhhjjjjjj"};
const Family kVdp{"00011pppsc0oderaagttnwwwwbflllkkhhiijjjjjjzzzzmmmqqqyyyxxxuuuuuu"};
const Family kVcomp{"00110pppsddyenr-aaaaobbccmmff-ttkk--ggggggg-------hhhhhhh---wwww"};
const Family kVmov{"00111pppstrydecbmmaanoooiwwwwkllffgghhhhjjjjjjqqqqqquuuuuuvvvvvv"};
const Family kVpck{"01000pppsnuydercaaaaffftttmmmmbbkkllgggggggoohiijjqqqqqqvwwwwwwx"};
const Family kVtst{"01001ppps-oydrceavttiizzmhhhnnbbkkffgggggggwlluuuujjjjjjjqqqqqqq"};
const Family kI32mad{"10101pps-nrcdeba0tttif00yy000kgghhjjlllllllmmmmmmmoooooooqqqqqqq"};
const Family kVldst{"111oopppsnmycrbakkkkddeetgffihjlqquuvvvvvvvwwwwwwwxxxxxxxzzzzzzz"};

/// La primera instruccion de TODOS los programas de vertices del corpus (PHAS) y la
/// ultima (emitir el vertice y acabar). Tal cual.
constexpr u64 kWordPhas = 0xfa44070000000000ull;
constexpr u64 kWordEnd = 0xfb275000a0200000ull;
/// Esperar a la carga pedida por el canal 0 (detras de cada LD, como en 14.832
/// sitios del corpus con esta misma palabra).
constexpr u64 kWordWdf = 0xf920000000000000ull;
/// Salto relativo sin condicion; el predicado va en los bits 56-58 y el
/// desplazamiento (en instrucciones, con signo) en los 20 de abajo.
constexpr u64 kWordBr = 0xf800004000000000ull;

enum class Bank : u8 { Temp, Out, Pa, Sa, Const, Imm, Internal };

struct Reg {
    Bank bank = Bank::Temp;
    u16 n = 0; ///< palabra de 32 bits; Const: indice; Imm: valor; Internal: 0-3

    bool operator==(const Reg& o) const {
        return bank == o.bank && n == o.n;
    }
};

constexpr Reg kZeroConst{Bank::Const, 0}; ///< c0: x = 0
constexpr Reg kOneConst{Bank::Const, 3};  ///< c3: x = 1, y = 1
constexpr Reg kZeroOne{Bank::Const, 1};   ///< c1: x = 0, y = 1

/// Selector de componente de una fuente: x-w, o las constantes 0 y 1 (que el
/// swizzle largo de la USSE da gratis).
enum Sel : u8 { X = 0, Y = 1, Z = 2, W = 3, Zero = 4, One = 5 };

/// Los 16 swizzles "estandar" de cuatro componentes (los unicos que admite la
/// segunda fuente de casi todo).
constexpr u8 kSw4[16][4] = {
    {X, X, X, X}, {Y, Y, Y, Y}, {Z, Z, Z, Z}, {W, W, W, W}, {X, Y, Z, W}, {Y, Z, W, W},
    {X, Y, Z, Z}, {X, X, Y, Z}, {X, Y, X, Y}, {X, Y, W, Z}, {Z, X, Y, W}, {Z, W, Z, W},
    {Y, Z, X, Z}, {X, X, Y, Y}, {X, Z, W, W}, {X, Y, Z, One},
};
constexpr u8 kSwXyzw = 4;
constexpr u8 kSwXxxx = 0;
constexpr u8 kSwYyyy = 1;
constexpr u8 kSwZzzz = 2;
constexpr u8 kSwZwzw = 11;

/// Predicado: ninguno, p0 o !p0 (con eso basta). Cada familia lo codifica a su manera.
enum class Pred : u8 { None, P0, NotP0 };

constexpr u32 ExtPred(Pred p) { // vmov, vpck, vcomp, vtst, vldst, br
    return p == Pred::None ? 0u : p == Pred::P0 ? 1u : 5u;
}
constexpr u32 VecPred(Pred p) { // v32nmad, vdp
    return p == Pred::None ? 0u : p == Pred::P0 ? 1u : 4u;
}

enum class Nop : u8 { Mul = 0, Add = 1, Frc = 2, Min = 5, Max = 6 };
enum class Comp : u8 { Rcp = 0, Rsq = 1, Log = 2, Exp = 3 };
enum class Cmp : u8 { Eq, Ne, Lt, Le, Gt, Ge };

/// Una instruccion ya armada, con lo que hace falta para el repaso final.
struct Ins {
    u64 word = 0;
    u8 writes_internal = 0; ///< bit por registro interno
    u8 reads_internal = 0;
    s8 nosched_bit = -1; ///< donde va su bit "nosched", si lo tiene
    bool barrier = false; ///< salto, etiqueta o espera: los internos no pasan de aqui
    s32 label = -1;       ///< salto pendiente de parchear
};

struct BankSel {
    u32 sel;
    u32 ext;
};

BankSel DestSel(Bank b) {
    switch (b) {
    case Bank::Out:
        return {1, 0};
    case Bank::Pa:
        return {2, 0};
    case Bank::Sa:
        return {0, 1};
    default: // Temp e Internal
        return {0, 0};
    }
}

BankSel Src12Sel(Bank b) {
    switch (b) {
    case Bank::Out:
        return {1, 0};
    case Bank::Pa:
        return {2, 0};
    case Bank::Sa:
        return {3, 0};
    case Bank::Const:
        return {1, 1};
    case Bank::Imm:
        return {2, 1};
    default:
        return {0, 0};
    }
}

BankSel Src0Sel(Bank b) {
    switch (b) {
    case Bank::Pa:
        return {1, 0};
    case Bank::Out:
        return {0, 1};
    case Bank::Sa:
        return {1, 1};
    default:
        return {0, 0};
    }
}

/// Numero de registro en un campo de 'bits' con numeracion DOBLE (los de coma
/// flotante van de dos en dos palabras): los cuatro ultimos son los internos.
u32 DoubleNum(Reg r, u32 bits) {
    if (r.bank == Bank::Internal) {
        return (1u << bits) - 4u + r.n;
    }
    if (r.bank == Bank::Const || r.bank == Bank::Imm) {
        return r.n;
    }
    return r.n >> 1;
}

/// Y con numeracion sencilla (palabra a palabra, 7 bits).
u32 SingleNum(Reg r) {
    if (r.bank == Bank::Internal) {
        return 124u + r.n;
    }
    return r.n;
}

u8 InternalBit(Reg r) {
    return r.bank == Bank::Internal ? static_cast<u8>(1u << r.n) : 0;
}

class Assembler {
public:
    std::vector<Ins> code;
    bool overflow = false;

    void Raw(u64 word, bool barrier = false) {
        Ins ins;
        ins.word = word;
        ins.barrier = barrier;
        code.push_back(ins);
    }

    /// vmul / vadd / vfrc / vmin / vmax: d.mask = op(s1.sw1, s2.sw2). La primera
    /// fuente lleva swizzle libre (con ceros y unos) y puede ir negada o en valor
    /// absoluto; la segunda, uno de los 16 estandar, y solo valor absoluto.
    void Nmad(Nop op, Reg d, u32 mask, Reg s1, const u8 sw1[4], bool neg1, bool abs1, Reg s2,
              u32 sw2, bool abs2, Pred pred = Pred::None) {
        Check(d, 6, true);
        Check(s1, 6, true);
        Check(s2, 6, true);
        u64 w = kV32nmad.value;
        const u32 sw = sw1[0] | (sw1[1] << 3) | (sw1[2] << 6) | (sw1[3] << 9);
        const BankSel db = DestSel(d.bank);
        const BankSel b1 = Src12Sel(s1.bank);
        const BankSel b2 = Src12Sel(s2.bank);
        kV32nmad.Set(w, 'p', VecPred(pred));
        kV32nmad.Set(w, 's', 1);
        kV32nmad.Set(w, 'r', (sw >> 10) & 3);
        kV32nmad.Set(w, 'd', db.ext);
        kV32nmad.Set(w, 'c', (sw >> 9) & 1);
        kV32nmad.Set(w, 'b', b1.ext);
        kV32nmad.Set(w, 'a', b2.ext);
        kV32nmad.Set(w, 'w', sw2);
        kV32nmad.Set(w, 'e', mask);
        kV32nmad.Set(w, 'm', (neg1 ? 1u : 0u) | (abs1 ? 2u : 0u));
        kV32nmad.Set(w, 'o', abs2 ? 1u : 0u);
        kV32nmad.Set(w, 'i', (sw >> 7) & 3);
        kV32nmad.Set(w, 't', db.sel);
        kV32nmad.Set(w, 'k', b1.sel);
        kV32nmad.Set(w, 'l', b2.sel);
        kV32nmad.Set(w, 'f', DoubleNum(d, 6));
        kV32nmad.Set(w, 'z', sw & 0x7F);
        kV32nmad.Set(w, 'g', static_cast<u32>(op));
        kV32nmad.Set(w, 'h', DoubleNum(s1, 6));
        kV32nmad.Set(w, 'j', DoubleNum(s2, 6));
        Push(w, kV32nmad.Bit('n'), InternalBit(d), InternalBit(s1) | InternalBit(s2));
    }

    /// vmov.f32 d.mask = s.sw (swizzle estandar). En registros que no son
    /// internos solo existen las componentes x e y.
    void Vmov(Reg d, u32 mask, Reg s, u32 sw, Pred pred = Pred::None) {
        Check(d, 6, true);
        Check(s, 6, true);
        // Con una sola componente, el swizzle repetido de la que se lee (xxxx...),
        // que es como lo escribe siempre el compilador de Sony.
        for (u32 j = 0; j < 4; j++) {
            if (mask == (1u << j) && kSw4[sw][j] <= W) {
                sw = kSw4[sw][j];
            }
        }
        u64 w = kVmov.value;
        const BankSel db = DestSel(d.bank);
        const BankSel b1 = Src12Sel(s.bank);
        kVmov.Set(w, 'p', ExtPred(pred));
        kVmov.Set(w, 's', 1);
        kVmov.Set(w, 'd', db.ext);
        kVmov.Set(w, 'c', b1.ext);
        kVmov.Set(w, 'o', 5); // f32
        kVmov.Set(w, 'w', sw);
        kVmov.Set(w, 'l', db.sel);
        kVmov.Set(w, 'f', b1.sel);
        kVmov.Set(w, 'h', mask);
        kVmov.Set(w, 'j', DoubleNum(d, 6));
        kVmov.Set(w, 'u', DoubleNum(s, 6));
        Push(w, kVmov.Bit('n'), InternalBit(d), InternalBit(s));
    }

    /// vmovc.f32 d.mask = (t <0 | <=0 | ==0 | !=0) ? a.sw : b.sw, componente a
    /// componente: la de 't' que se mira es la misma que se escribe.
    /// test: 0 !=0, 1 ==0, 2 <0, 3 <=0.
    void Vmovc(Reg d, u32 mask, Reg t, u32 test, Reg a, Reg b, u32 sw) {
        Check(d, 6, true);
        Check(t, 6, true);
        Check(a, 6, true);
        Check(b, 6, true);
        u64 w = kVmov.value;
        const BankSel db = DestSel(d.bank);
        const BankSel b0 = Src0Sel(t.bank);
        const BankSel b1 = Src12Sel(a.bank);
        const BankSel b2 = Src12Sel(b.bank);
        kVmov.Set(w, 's', 1);
        kVmov.Set(w, 't', (test >> 1) & 1);
        kVmov.Set(w, 'i', test & 1);
        kVmov.Set(w, 'd', db.ext);
        kVmov.Set(w, 'e', b0.ext);
        kVmov.Set(w, 'c', b1.ext);
        kVmov.Set(w, 'b', b2.ext);
        kVmov.Set(w, 'm', 1);
        kVmov.Set(w, 'o', 5);
        kVmov.Set(w, 'w', sw);
        kVmov.Set(w, 'k', b0.sel);
        kVmov.Set(w, 'l', db.sel);
        kVmov.Set(w, 'f', b1.sel);
        kVmov.Set(w, 'g', b2.sel);
        kVmov.Set(w, 'h', mask);
        kVmov.Set(w, 'j', DoubleNum(d, 6));
        kVmov.Set(w, 'q', DoubleNum(t, 6));
        kVmov.Set(w, 'u', DoubleNum(a, 6));
        kVmov.Set(w, 'v', DoubleNum(b, 6));
        Push(w, kVmov.Bit('n'), InternalBit(d), InternalBit(t) | InternalBit(a) | InternalBit(b));
    }

    /// vpck.f32.f32: copia con swizzle LIBRE (sin constantes ni signo). La fuente
    /// son dos registros de 64 bits: s_lo (componentes 0 y 1) y s_hi (2 y 3).
    void VpckMove(Reg d, u32 mask, Reg s_lo, Reg s_hi, const u8 sel[4]) {
        Check(d, 7, false);
        Check(s_lo, 6, true);
        Check(s_hi, 6, true);
        u64 w = kVpck.value;
        const BankSel db = DestSel(d.bank);
        const BankSel b1 = Src12Sel(s_lo.bank);
        const BankSel b2 = Src12Sel(s_hi.bank);
        kVpck.Set(w, 's', 1);
        kVpck.Set(w, 'd', db.ext);
        kVpck.Set(w, 'r', b1.ext);
        kVpck.Set(w, 'c', b2.ext);
        kVpck.Set(w, 'f', 6);
        kVpck.Set(w, 't', 6);
        kVpck.Set(w, 'm', mask);
        kVpck.Set(w, 'b', db.sel);
        kVpck.Set(w, 'k', b1.sel);
        kVpck.Set(w, 'l', b2.sel);
        kVpck.Set(w, 'g', SingleNum(d));
        kVpck.Set(w, 'o', sel[3]);
        kVpck.Set(w, 'i', sel[1]);
        kVpck.Set(w, 'j', sel[2]);
        kVpck.Set(w, 'q', DoubleNum(s_lo, 6));
        kVpck.Set(w, 'v', (sel[0] >> 1) & 1);
        kVpck.Set(w, 'w', DoubleNum(s_hi, 6));
        kVpck.Set(w, 'x', sel[0] & 1);
        Push(w, kVpck.Bit('n'), InternalBit(d), InternalBit(s_lo) | InternalBit(s_hi));
    }

    /// vpck.u16.f32 d.x = (entero sin signo) s.x. Para los indices de uniformes.
    void VpckToU16(Reg d, Reg s) {
        Check(d, 7, false);
        Check(s, 6, true);
        u64 w = kVpck.value;
        const BankSel db = DestSel(d.bank);
        const BankSel b1 = Src12Sel(s.bank);
        kVpck.Set(w, 's', 1);
        kVpck.Set(w, 'd', db.ext);
        kVpck.Set(w, 'r', b1.ext);
        kVpck.Set(w, 'c', 1); // segunda fuente: inmediato 0
        kVpck.Set(w, 'f', 6);
        kVpck.Set(w, 't', 3);
        kVpck.Set(w, 'm', 1);
        kVpck.Set(w, 'b', db.sel);
        kVpck.Set(w, 'k', b1.sel);
        kVpck.Set(w, 'l', 2);
        kVpck.Set(w, 'g', SingleNum(d));
        kVpck.Set(w, 'q', DoubleNum(s, 6));
        Push(w, kVpck.Bit('n'), InternalBit(d), InternalBit(s));
    }

    /// vrcp / vrsq / vlog / vexp (base 2): d.mask = f(s.comp), escalar.
    void Vcomp(Comp op, Reg d, u32 mask, Reg s, u32 comp, Pred pred = Pred::None) {
        Check(d, 7, true);
        Check(s, 7, true);
        u64 w = kVcomp.value;
        const BankSel db = DestSel(d.bank);
        const BankSel b1 = Src12Sel(s.bank);
        kVcomp.Set(w, 'p', ExtPred(pred));
        kVcomp.Set(w, 's', 1);
        kVcomp.Set(w, 'e', db.ext);
        kVcomp.Set(w, 'r', b1.ext);
        kVcomp.Set(w, 'b', static_cast<u32>(op));
        kVcomp.Set(w, 'f', comp);
        kVcomp.Set(w, 't', db.sel);
        kVcomp.Set(w, 'k', b1.sel);
        kVcomp.Set(w, 'g', DoubleNum(d, 7));
        kVcomp.Set(w, 'h', DoubleNum(s, 7));
        kVcomp.Set(w, 'w', mask);
        Push(w, kVcomp.Bit('o'), InternalBit(d), InternalBit(s));
    }

    /// p0 = (s.x [cmp] 0), o con 'chan_y' (s.y [cmp] 0). Con 'chain' solo se
    /// evalua si p0 ya era cierto (un Y logico).
    void TestZero(Cmp cmp, Reg s, bool chan_y, bool chain) {
        Check(s, 7, true);
        u64 w = kVtst.value;
        const BankSel b1 = Src12Sel(s.bank);
        u32 zero = 0;
        u32 sign = 0;
        u32 comb = 1;
        switch (cmp) {
        case Cmp::Eq:
            zero = 1;
            break;
        case Cmp::Ne:
            zero = 2;
            break;
        case Cmp::Lt:
            zero = 2;
            sign = 1;
            break;
        case Cmp::Le:
            zero = 1;
            sign = 1;
            comb = 0;
            break;
        case Cmp::Gt:
            zero = 2;
            sign = 2;
            break;
        case Cmp::Ge:
            zero = 1;
            sign = 2;
            comb = 0;
            break;
        }
        kVtst.Set(w, 'p', chain ? 1u : 0u);
        kVtst.Set(w, 's', 1);
        kVtst.Set(w, 'd', 1);
        kVtst.Set(w, 'c', b1.ext);
        kVtst.Set(w, 'e', 1); // la segunda fuente es la constante c0
        kVtst.Set(w, 'a', 1); // f32
        kVtst.Set(w, 'v', chan_y ? 1u : 0u);
        kVtst.Set(w, 'i', sign);
        kVtst.Set(w, 'z', zero);
        kVtst.Set(w, 'm', comb);
        kVtst.Set(w, 'h', chan_y ? 1u : 0u);
        kVtst.Set(w, 'b', 1);
        kVtst.Set(w, 'k', b1.sel);
        kVtst.Set(w, 'f', 1);
        kVtst.Set(w, 'u', 14); // resta
        kVtst.Set(w, 'j', DoubleNum(s, 7));
        Push(w, -1, 0, InternalBit(s));
    }

    /// d = s * 16 + sa (enteros). Para pasar de indice de uniforme a direccion.
    void I32Mad16(Reg d, Reg s, Reg sa_literal) {
        u64 w = kI32mad.value;
        kI32mad.Set(w, 'b', 1); // src1 = inmediato
        kI32mad.Set(w, 'i', 1);
        kI32mad.Set(w, 'y', 2);
        kI32mad.Set(w, 'k', 1); // src0 = pa
        kI32mad.Set(w, 'g', 2); // destino = pa
        kI32mad.Set(w, 'h', 2);
        kI32mad.Set(w, 'j', 3); // src2 = sa
        kI32mad.Set(w, 'l', d.n);
        kI32mad.Set(w, 'm', s.n);
        kI32mad.Set(w, 'o', 16);
        kI32mad.Set(w, 'q', sa_literal.n);
        Push(w, kI32mad.Bit('n'), 0, 0);
    }

    /// Carga 'count' palabras del buffer de uniformes en d (temporal): la
    /// direccion es sa_ptr + desplazamiento (inmediato en palabras, o un registro
    /// sa/pa con 0x10000 + bytes). Hay que esperarla despues con Wdf().
    void Ld(Reg d, u32 count, Reg sa_ptr, Reg offset) {
        u64 w = kVldst.value;
        const BankSel b1 = Src12Sel(offset.bank);
        kVldst.Set(w, 'o', 1);
        kVldst.Set(w, 's', 1);
        kVldst.Set(w, 'r', 1); // base en sa
        kVldst.Set(w, 'b', b1.ext);
        kVldst.Set(w, 'a', 1);
        kVldst.Set(w, 'k', count - 1);
        kVldst.Set(w, 'h', 1);
        kVldst.Set(w, 'q', b1.sel);
        kVldst.Set(w, 'u', 2);
        kVldst.Set(w, 'v', d.n);
        kVldst.Set(w, 'w', sa_ptr.n);
        kVldst.Set(w, 'x', offset.n);
        Push(w, kVldst.Bit('n'), 0, 0);
    }

    void Wdf() {
        Raw(kWordWdf, true);
    }

    /// d.comp = dot(interno 'gpi' (xyz o xyzw), s.sw) con el signo de 'neg'.
    void Vdp(bool vec4, Reg d, u32 mask, u32 gpi, Reg s, const u8 sw[4], bool neg) {
        Check(d, 6, true);
        Check(s, 6, true);
        u64 w = kVdp.value;
        const BankSel db = DestSel(d.bank);
        const BankSel b1 = Src12Sel(s.bank);
        kVdp.Set(w, 's', 1);
        kVdp.Set(w, 'o', vec4 ? 1u : 0u);
        kVdp.Set(w, 'd', db.ext);
        kVdp.Set(w, 'r', b1.ext);
        kVdp.Set(w, 'a', 3);
        kVdp.Set(w, 'w', mask);
        kVdp.Set(w, 'b', neg ? 1u : 0u);
        kVdp.Set(w, 'k', db.sel);
        kVdp.Set(w, 'h', b1.sel);
        kVdp.Set(w, 'i', gpi);
        kVdp.Set(w, 'j', DoubleNum(d, 6));
        kVdp.Set(w, 'z', 4); // xyzw (o xyz en el de tres)
        kVdp.Set(w, 'x', sw[0]);
        kVdp.Set(w, 'y', sw[1]);
        kVdp.Set(w, 'q', sw[2]);
        kVdp.Set(w, 'm', vec4 ? sw[3] : 0u);
        kVdp.Set(w, 'u', DoubleNum(s, 6));
        Push(w, kVdp.Bit('n'), InternalBit(d), static_cast<u8>((1u << gpi) | InternalBit(s)));
    }

    [[nodiscard]] s32 NewLabel() {
        labels.push_back(-1);
        return static_cast<s32>(labels.size() - 1);
    }

    void Bind(s32 label) {
        labels[static_cast<std::size_t>(label)] = static_cast<s32>(code.size());
        barrier_next = true;
    }

    void Br(s32 label, Pred pred = Pred::None) {
        Ins ins;
        ins.word = kWordBr | (static_cast<u64>(ExtPred(pred)) << 56);
        ins.barrier = true;
        ins.label = label;
        code.push_back(ins);
    }

    /// Parchea los saltos y pone "nosched" donde un registro interno sigue vivo.
    bool Finish() {
        for (std::size_t i = 0; i < code.size(); i++) {
            Ins& ins = code[i];
            if (ins.label < 0) {
                continue;
            }
            const s32 target = labels[static_cast<std::size_t>(ins.label)];
            if (target < 0) {
                return false;
            }
            const s32 offset = target - static_cast<s32>(i);
            if (offset < -0x80000 || offset > 0x7FFFF) {
                return false;
            }
            ins.word |= static_cast<u32>(offset) & 0xFFFFFu;
        }
        /**
         * NOSCHED. En el corpus, 107.249 instrucciones tras las que queda vivo
         * un registro interno lo llevan puesto (hay otras 61.437 que no, asi
         * que no es obligatorio en todas partes; ponerlo siempre es lo que no
         * puede salir mal). Los internos nunca cruzan aqui un salto, una
         * etiqueta ni una espera de carga: en el corpus tampoco (481 de 44.741
         * esperas, y con un analisis lineal que no ve los saltos).
         */
        u8 live = 0;
        for (std::size_t i = code.size(); i-- > 0;) {
            Ins& ins = code[i];
            if (ins.barrier) {
                live = 0;
            }
            if (live != 0 && ins.nosched_bit >= 0) {
                ins.word |= 1ull << ins.nosched_bit;
            }
            live = static_cast<u8>((live & ~ins.writes_internal) | ins.reads_internal);
            if (ins.barrier) {
                live = 0;
            }
        }
        return !overflow;
    }

private:
    std::vector<s32> labels;
    bool barrier_next = false;

    void Push(u64 word, int nosched_bit, u8 writes, u8 reads) {
        Ins ins;
        ins.word = word;
        ins.nosched_bit = static_cast<s8>(nosched_bit);
        ins.writes_internal = writes;
        ins.reads_internal = reads;
        code.push_back(ins);
    }

    /// Que el registro cabe en su campo (y es par donde la numeracion es doble).
    void Check(Reg r, u32 bits, bool is_double) {
        if (r.bank == Bank::Internal || r.bank == Bank::Const || r.bank == Bank::Imm) {
            return;
        }
        const u32 limit = is_double ? ((1u << bits) - 4u) * 2u : (1u << bits) - 4u;
        if (r.n >= limit || (is_double && (r.n & 1u) != 0)) {
            overflow = true;
        }
    }
};

// ---------------------------------------------------------------------------
// 2. EL TRADUCTOR
// ---------------------------------------------------------------------------

constexpr u32 kProgramWords = 4096;
constexpr u32 kMaxInstructions = 6000;
constexpr u32 kMaxCallDepth = 12;
/// Palabras de temporales que alcanza la numeracion doble de 6 bits.
constexpr u32 kTempLimit = 120;
/// Palabras de SA: el puntero al buffer y los literales.
constexpr u32 kSaLimit = 118;

/// Una fuente ya resuelta: registro base de cuatro palabras, de donde sale cada
/// componente y el signo.
struct Src {
    Reg reg;
    u8 sel[4] = {X, Y, Z, W};
    bool neg = false;

    [[nodiscard]] bool HasConst() const {
        return reg.bank == Bank::Const || sel[0] > W || sel[1] > W || sel[2] > W || sel[3] > W;
    }
};

/// A donde va el resultado de una instruccion de la USSE: una mitad (64 bits)
/// de un registro normal, o un interno entero. comp[j] es la componente de la
/// PICA que cae en la componente j del destino.
struct View {
    Reg reg;
    u32 n = 2;
    u8 comp[4] = {0, 1, 0, 0};
};

class Translator {
public:
    Translator(const VsRequest& request_, VsResult& result_) : rq{request_}, out{result_} {}

    bool Run() {
        // Las entradas que vienen de un flujo, a sus registros PA en orden.
        for (u32 reg = 0; reg < 16; reg++) {
            if (rq.inputs[reg].kind == VsInput::Array) {
                input_pa[reg] = static_cast<s32>(pa_words);
                pa_words += 4;
            }
        }
        pa_index = Reg{Bank::Pa, static_cast<u16>(pa_words)};
        pa_words += 4;
        if (pa_words > 120) {
            return Fail("usse: demasiadas entradas");
        }
        sa_pointer = Reg{Bank::Sa, 0};
        sa_words = 2;

        end_label = as.NewLabel();
        Frame main{rq.main_offset, kProgramWords, true, {}};
        if (!EmitFrame(main, 0) || failed) {
            return Fail(reason != nullptr ? reason : "usse: programa");
        }
        as.Bind(end_label);
        ForgetUniforms();
        if (!Epilogue() || failed) {
            return Fail(reason != nullptr ? reason : "usse: salidas");
        }
        as.Raw(kWordEnd);

        // Lo de delante se arma al final: ya se sabe que registros hay que
        // dejar en su valor de partida. Los saltos son relativos, asi que
        // poner instrucciones delante no los mueve.
        Assembler body = std::move(as);
        as = Assembler{};
        as.Raw(kWordPhas);
        Prologue();
        const std::size_t shift = as.code.size();
        if (!body.Finish() || as.overflow) {
            return Fail("usse: saltos o registros");
        }
        (void)shift;
        for (const Ins& ins : body.code) {
            as.code.push_back(ins);
            as.code.back().label = -1; // ya parcheado
        }
        if (!as.Finish()) {
            return Fail("usse: registros");
        }
        if (as.code.size() > kMaxInstructions || temp_words > kTempLimit ||
            sa_words > kSaLimit) {
            return Fail("usse: demasiado grande");
        }
        WriteContainer();
        out.instructions = static_cast<u32>(as.code.size());
        out.temp_words = temp_words;
        out.pa_words = pa_words;
        out.sa_words = sa_words;
        out.uses_ints = uses_ints;
        out.uses_bools = uses_bools;
        out.uses_defaults = uses_defaults;
        return true;
    }

private:
    struct Frame {
        u32 begin;
        u32 end;
        bool main;
        std::map<u32, s32> labels; ///< destinos de saltos hacia delante
    };

    struct LoopInfo {
        s32 break_label;
    };

    /// Lo que se sabe de un registro de la PICA para dejarlo (o no) en su valor
    /// de partida: que componentes se han escrito seguro y cuales se leen antes.
    struct RegState {
        s32 base = -1;
        u8 sure = 0;
        u8 need = 0;
    };

    const VsRequest& rq;
    VsResult& out;
    Assembler as;
    bool failed = false;
    const char* reason = nullptr;

    u32 pa_words = 0;
    u32 temp_words = 0;
    u32 sa_words = 0;
    std::array<s32, 16> input_pa{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    std::array<RegState, 16> temps{};
    std::array<RegState, 16> attrs{};
    std::array<s32, 16> default_base{-1, -1, -1, -1, -1, -1, -1, -1,
                                     -1, -1, -1, -1, -1, -1, -1, -1};
    s32 cmp_base = -1;
    s32 addr_base = -1;
    std::array<s32, 4> loop_base{-1, -1, -1, -1};
    Reg pa_index;
    Reg sa_pointer;
    std::map<u32, u32> literals; ///< valor crudo -> palabra de SA (par)
    std::vector<std::pair<u32, u32>> literal_list;

    /// Mientras no haya habido ningun salto ni etiqueta, lo escrito es seguro.
    bool linear = true;
    s32 end_label = -1;
    std::vector<LoopInfo> loops;
    u32 emitted_pica = 0;

    bool uses_ints = false;
    bool uses_bools = false;
    bool uses_defaults = false;
    bool uses_wbuf = false;

    /// Los huecos donde se cargan los uniformes. La clave es la palabra del
    /// buffer, o el uniforme con su registro de direccion.
    struct UniformSlot {
        s32 base = -1;
        u32 key = ~0u;
        u32 age = 0;
        u32 pinned = 0;
    };
    std::array<UniformSlot, 3> slots{};
    u32 slot_clock = 0;
    u32 pin_stamp = 1;

    bool Fail(const char* why) {
        if (!failed) {
            failed = true;
            reason = why;
        }
        out.reason = reason;
        return false;
    }

    // ---- registros ----

    s32 AllocTemp(u32 words) {
        const s32 base = static_cast<s32>(temp_words);
        temp_words += words;
        if (temp_words > kTempLimit) {
            Fail("usse: sin temporales");
            return 0;
        }
        return base;
    }

    Reg TempReg(s32 base, u32 offset = 0) {
        return Reg{Bank::Temp, static_cast<u16>(base + static_cast<s32>(offset))};
    }

    RegState& TempState(u32 index) {
        RegState& state = temps[index];
        if (state.base < 0) {
            state.base = AllocTemp(4);
        }
        return state;
    }

    RegState& AttrState(u32 index) {
        RegState& state = attrs[index];
        if (state.base < 0) {
            state.base = AllocTemp(4);
        }
        return state;
    }

    void NoteRead(RegState& state, u32 comps) {
        state.need |= static_cast<u8>(comps & ~state.sure);
    }

    void NoteWrite(RegState& state, u32 mask) {
        if (linear) {
            state.sure |= static_cast<u8>(mask);
        }
    }

    Reg CmpReg() {
        if (cmp_base < 0) {
            cmp_base = AllocTemp(2);
        }
        return TempReg(cmp_base);
    }

    Reg AddrReg(u32 half) {
        if (addr_base < 0) {
            addr_base = AllocTemp(4);
        }
        return TempReg(addr_base, half * 2);
    }

    u32 Literal(u32 raw) {
        const auto it = literals.find(raw);
        if (it != literals.end()) {
            return it->second;
        }
        const u32 word = sa_words;
        sa_words += 2;
        if (sa_words > kSaLimit) {
            Fail("usse: sin literales");
            return 2;
        }
        literals.emplace(raw, word);
        literal_list.emplace_back(word, raw);
        return word;
    }

    Reg FloatLiteral(float value) {
        u32 raw;
        std::memcpy(&raw, &value, sizeof(raw));
        return Reg{Bank::Sa, static_cast<u16>(Literal(raw))};
    }

    // ---- uniformes ----

    void ForgetUniforms() {
        for (auto& slot : slots) {
            slot.key = ~0u;
        }
    }

    void ForgetIndexed(u32 address_register) {
        for (auto& slot : slots) {
            if (slot.key != ~0u && ((slot.key >> 16) & 3u) == address_register) {
                slot.key = ~0u;
            }
        }
    }

    UniformSlot& PickSlot(u32 key, bool& hit) {
        for (auto& slot : slots) {
            if (slot.key == key && slot.base >= 0) {
                slot.age = ++slot_clock;
                slot.pinned = pin_stamp;
                hit = true;
                return slot;
            }
        }
        hit = false;
        UniformSlot* best = nullptr;
        for (auto& slot : slots) {
            if (slot.pinned == pin_stamp) {
                continue;
            }
            if (best == nullptr || slot.age < best->age) {
                best = &slot;
            }
        }
        if (best == nullptr) {
            Fail("usse: sin huecos de uniforme");
            best = &slots[0];
        }
        if (best->base < 0) {
            best->base = AllocTemp(4);
        }
        best->key = key;
        best->age = ++slot_clock;
        best->pinned = pin_stamp;
        return *best;
    }

    /// Cuatro palabras del buffer de uniformes, desde 'word', en un hueco.
    Reg LoadWords(u32 word) {
        bool hit = false;
        UniformSlot& slot = PickSlot(word, hit);
        const Reg dest = TempReg(slot.base);
        if (!hit) {
            EmitLoad(dest, word);
        }
        return dest;
    }

    void EmitLoad(Reg dest, u32 word) {
        if (word <= 127) {
            as.Ld(dest, 4, sa_pointer, Reg{Bank::Imm, static_cast<u16>(word)});
        } else {
            const u32 literal = Literal(0x10000u + word * 4u);
            as.Ld(dest, 4, sa_pointer, Reg{Bank::Sa, static_cast<u16>(literal)});
        }
        as.Wdf();
    }

    /**
     * Un uniforme flotante leido con registro de direccion: c[indice + a]. Como
     * la traduccion a Cg, el indice se acota a 0-95 (los juegos no leen fuera de
     * sus 96 y asi la GPU nunca lee fuera del buffer). La secuencia es la del
     * compilador: a coma flotante se suma y se acota, vpck a entero, por 16 mas
     * 0x10000, y la carga con ese registro de desplazamiento.
     */
    Reg LoadIndexed(u32 index, u32 address_register) {
        const u32 key = 0x40000000u | (address_register << 16) | index;
        bool hit = false;
        UniformSlot& slot = PickSlot(key, hit);
        const Reg dest = TempReg(slot.base);
        if (hit) {
            return dest;
        }
        const u32 component = address_register - 1; // 0 a0.x, 1 a0.y, 2 aL
        const Reg address = AddrReg(component / 2);
        const u8 pick = component == 1 ? Y : X;
        const u8 sw[4] = {pick, pick, pick, pick};
        const Reg base_literal = FloatLiteral(static_cast<float>(index));
        const Reg top_literal = FloatLiteral(95.0f);
        const Reg scale_literal = Reg{Bank::Sa, static_cast<u16>(Literal(0x10000u))};
        const u8 xxxx[4] = {X, X, X, X};
        as.Nmad(Nop::Add, pa_index, 1, address, sw, false, false, base_literal, kSwXxxx, false);
        as.Nmad(Nop::Max, pa_index, 1, pa_index, xxxx, false, false, kZeroConst, kSwXxxx, false);
        as.Nmad(Nop::Min, pa_index, 1, pa_index, xxxx, false, false, top_literal, kSwXxxx, false);
        as.VpckToU16(pa_index, pa_index);
        as.I32Mad16(pa_index, pa_index, scale_literal);
        as.Ld(dest, 4, sa_pointer, pa_index);
        as.Wdf();
        return dest;
    }

    // ---- fuentes ----

    /// Los selectores de la fuente 'which' (0-2) de un patron de swizzle.
    static void Selectors(u32 pattern, u32 which, u8 sel[4], bool& neg) {
        static constexpr u32 kNegate[3] = {4, 13, 22};
        static constexpr u32 kSelect[3] = {11, 20, 29};
        neg = ((pattern >> kNegate[which]) & 1u) != 0;
        for (u32 i = 0; i < 4; i++) {
            sel[i] = static_cast<u8>((pattern >> (kSelect[which] - 2 * i)) & 3u);
        }
    }

    /// 'used': que componentes de la PICA lee de verdad la instruccion.
    Src Fetch(u32 reg, u32 address_register, u32 pattern, u32 which, u32 used) {
        Src src;
        Selectors(pattern, which, src.sel, src.neg);
        u32 read = 0;
        for (u32 i = 0; i < 4; i++) {
            if ((used >> i) & 1u) {
                read |= 1u << src.sel[i];
            }
        }
        if (reg < 0x10) {
            const VsInput& input = rq.inputs[reg];
            if (input.kind == VsInput::Array) {
                src.reg = Reg{Bank::Pa, static_cast<u16>(input_pa[reg])};
                // Lo que el flujo no trae vale 0, 0, 0 y 1, como en VertexLoader.
                for (u32 i = 0; i < 4; i++) {
                    if (src.sel[i] >= input.components) {
                        src.sel[i] = src.sel[i] == W ? One : Zero;
                    }
                }
            } else if (input.kind == VsInput::Default) {
                if (default_base[reg] < 0) {
                    default_base[reg] = AllocTemp(4);
                    uses_defaults = true;
                }
                src.reg = TempReg(default_base[reg]);
            } else {
                src.reg = kZeroConst;
                src.sel[0] = src.sel[1] = src.sel[2] = src.sel[3] = X;
                src.neg = false;
            }
        } else if (reg < 0x20) {
            RegState& state = TempState(reg - 0x10);
            NoteRead(state, read);
            src.reg = TempReg(state.base);
        } else {
            const u32 index = reg - 0x20;
            if (index >= 96) {
                Fail("usse: uniforme fuera de rango");
                src.reg = kZeroConst;
                return src;
            }
            src.reg = address_register != 0 ? LoadIndexed(index, address_register)
                                            : LoadWords(kUniformFloatWord + index * 4);
        }
        return src;
    }

    /// Vale como segunda fuente para escribir la vista 'view' con 'mask'?
    static int SecondSwizzle(const Src& src, const View& view, u32 mask) {
        if (src.neg) {
            return -1;
        }
        for (int p = 0; p < 15; p++) { // el 15 lleva un uno: no hace falta
            bool ok = true;
            for (u32 j = 0; j < view.n && ok; j++) {
                if (((mask >> j) & 1u) != 0 && kSw4[p][j] != src.sel[view.comp[j]]) {
                    ok = false;
                }
            }
            if (ok) {
                // Una constante solo vale tal cual es (c0 = 0 en x): el resto no.
                if (src.reg.bank == Bank::Const && src.reg.n != 0) {
                    return -1;
                }
                return p;
            }
        }
        return -1;
    }

    static void FirstSwizzle(const Src& src, const View& view, u8 sw[4]) {
        for (u32 j = 0; j < 4; j++) {
            sw[j] = j < view.n ? src.sel[view.comp[j]] : static_cast<u8>(X);
        }
    }

    static Src Identity(Reg reg) {
        Src src;
        src.reg = reg;
        return src;
    }

    /// Copia la fuente (con su swizzle, sus constantes y su signo) a un interno:
    /// desde ahi vale como segunda fuente de cualquier cosa.
    Src Materialize(const Src& src, u32 internal) {
        const Reg dest{Bank::Internal, static_cast<u16>(internal)};
        if (!src.neg && !src.HasConst() && src.reg.bank != Bank::Const) {
            const Reg hi{src.reg.bank, static_cast<u16>(src.reg.n + 2)};
            if (src.reg.bank == Bank::Internal) {
                as.Vmov(dest, 0xF, src.reg, kSwXyzw);
                Src same = src;
                same.reg = dest;
                return same;
            }
            as.VpckMove(dest, 0xF, src.reg, hi, src.sel);
        } else {
            u8 sw[4] = {src.sel[0], src.sel[1], src.sel[2], src.sel[3]};
            as.Nmad(Nop::Mul, dest, 0xF, src.reg, sw, src.neg, false, kOneConst, kSwXxxx, false);
        }
        return Identity(dest);
    }

    static View Half(Reg base, u32 half) {
        View view;
        view.reg = Reg{base.bank, static_cast<u16>(base.n + half * 2)};
        view.n = 2;
        view.comp[0] = static_cast<u8>(half * 2);
        view.comp[1] = static_cast<u8>(half * 2 + 1);
        return view;
    }

    static View Whole(u32 internal) {
        View view;
        view.reg = Reg{Bank::Internal, static_cast<u16>(internal)};
        view.n = 4;
        for (u32 i = 0; i < 4; i++) {
            view.comp[i] = static_cast<u8>(i);
        }
        return view;
    }

    /// La mascara de la PICA (bit i = componente i) vista desde una vista.
    static u32 ViewMask(const View& view, u32 mask) {
        u32 result = 0;
        for (u32 j = 0; j < view.n; j++) {
            if ((mask >> view.comp[j]) & 1u) {
                result |= 1u << j;
            }
        }
        return result;
    }

    /// Una operacion de dos fuentes sobre una vista. false si ninguna de las dos
    /// puede ir de segunda fuente tal como esta.
    bool TryNmad(Nop op, const View& view, u32 mask, const Src& a, const Src& b,
                 bool commutative) {
        const u32 vmask = ViewMask(view, mask);
        if (vmask == 0) {
            return true;
        }
        const bool mul = op == Nop::Mul;
        // En el producto el signo de las dos se junta en la primera.
        const bool a_neg = mul ? (a.neg != b.neg) : a.neg;
        Src bb = b;
        if (mul) {
            bb.neg = false;
        }
        int p = SecondSwizzle(bb, view, vmask);
        if (p >= 0) {
            u8 sw[4];
            FirstSwizzle(a, view, sw);
            as.Nmad(op, view.reg, vmask, a.reg, sw, a_neg, false, bb.reg, static_cast<u32>(p),
                    false);
            return true;
        }
        if (!commutative) {
            return false;
        }
        Src aa = a;
        if (mul) {
            aa.neg = false;
        }
        p = SecondSwizzle(aa, view, vmask);
        if (p >= 0) {
            u8 sw[4];
            FirstSwizzle(b, view, sw);
            as.Nmad(op, view.reg, vmask, b.reg, sw, mul ? a_neg : b.neg, false, aa.reg,
                    static_cast<u32>(p), false);
            return true;
        }
        return false;
    }

    static bool Overlaps(Reg dest, const Src& src) {
        return src.reg.bank == dest.bank && src.reg.n == dest.n;
    }

    /// interno -> registro normal, por mitades.
    void Store(u32 internal, Reg dest, u32 mask, Pred pred = Pred::None) {
        const Reg src{Bank::Internal, static_cast<u16>(internal)};
        if ((mask & 3u) != 0) {
            as.Vmov(Reg{dest.bank, dest.n}, mask & 3u, src, kSwXyzw, pred);
        }
        if ((mask & 12u) != 0) {
            as.Vmov(Reg{dest.bank, static_cast<u16>(dest.n + 2)}, (mask >> 2) & 3u, src, kSwZwzw,
                    pred);
        }
    }

    /// El escalar de la x de un interno, a todas las componentes de 'mask'.
    void StoreScalar(u32 internal, Reg dest, u32 mask, Pred pred = Pred::None) {
        const Reg src{Bank::Internal, static_cast<u16>(internal)};
        if ((mask & 3u) != 0) {
            as.Vmov(Reg{dest.bank, dest.n}, mask & 3u, src, kSwXxxx, pred);
        }
        if ((mask & 12u) != 0) {
            as.Vmov(Reg{dest.bank, static_cast<u16>(dest.n + 2)}, (mask >> 2) & 3u, src, kSwXxxx,
                    pred);
        }
    }

    /**
     * dest.mask = op(a, b), con todo lo que haga falta: los signos (la USSE solo
     * niega la primera fuente), el swizzle de la segunda (que no es libre) y el
     * caso de que el destino sea tambien una fuente y haya que escribir las dos
     * mitades (la segunda leeria lo que acaba de escribir la primera).
     */
    void Binary(Nop op, Reg dest, u32 mask, Src a, Src b) {
        const bool commutative = true; // mul, add, min y max lo son
        if (op != Nop::Mul) {
            if (a.neg && b.neg) {
                a = Materialize(a, 1);
            }
            if (b.neg) {
                std::swap(a, b); // la negada, de primera
            }
        }
        const bool lo = (mask & 3u) != 0;
        const bool hi = (mask & 12u) != 0;
        const bool alias = lo && hi && (Overlaps(dest, a) || Overlaps(dest, b));
        const bool can_swap = commutative && !(op != Nop::Mul && a.neg);
        if (alias) {
            const View whole = Whole(0);
            if (!TryNmad(op, whole, mask, a, b, can_swap)) {
                b = Materialize(b, 2);
                TryNmad(op, whole, mask, a, b, false);
            }
            Store(0, dest, mask);
            return;
        }
        // Se prueba en seco si alguna mitad necesita la copia, para hacerla una vez.
        for (u32 half = 0; half < 2; half++) {
            const View view = Half(dest, half);
            const u32 vmask = ViewMask(view, mask);
            if (vmask == 0) {
                continue;
            }
            Src bb = b;
            Src aa = a;
            if (op == Nop::Mul) {
                bb.neg = false;
                aa.neg = false;
            }
            if (SecondSwizzle(bb, view, vmask) < 0 &&
                !(can_swap && SecondSwizzle(aa, view, vmask) >= 0)) {
                const bool keep_sign = op == Nop::Mul && (a.neg != b.neg);
                Src plain = b;
                if (op == Nop::Mul) {
                    plain.neg = false;
                }
                b = Materialize(plain, 2);
                if (op == Nop::Mul) {
                    a.neg = keep_sign;
                }
                break;
            }
        }
        for (u32 half = 0; half < 2; half++) {
            TryNmad(op, Half(dest, half), mask, a, b, can_swap);
        }
    }

    /// dest.mask = a (con swizzle y signo).
    void Move(Reg dest, u32 mask, const Src& a) {
        const bool lo = (mask & 3u) != 0;
        const bool hi = (mask & 12u) != 0;
        if (lo && hi && Overlaps(dest, a)) {
            Materialize(a, 0);
            Store(0, dest, mask);
            return;
        }
        for (u32 half = 0; half < 2; half++) {
            const View view = Half(dest, half);
            const u32 vmask = ViewMask(view, mask);
            if (vmask == 0) {
                continue;
            }
            bool plain = !a.neg && a.reg.bank != Bank::Const && a.reg.bank != Bank::Internal;
            for (u32 j = 0; j < 2; j++) {
                if (((vmask >> j) & 1u) != 0 && a.sel[view.comp[j]] > W) {
                    plain = false;
                }
            }
            if (plain && vmask != 3u) {
                // Una sola componente: VMOV con la fuente repetida (xxxx, yyyy, zzzz o
                // wwww). El compilador de Sony nunca escribe una palabra suelta con
                // VPCK (ni una vez en 880.000 instrucciones); con VMOV, 33.000.
                const u8 s = a.sel[view.comp[vmask == 1u ? 0 : 1]];
                as.Vmov(view.reg, vmask, a.reg, s);
            } else if (plain) {
                u8 sel[4] = {X, X, X, X};
                for (u32 j = 0; j < 2; j++) {
                    const u8 s = a.sel[view.comp[j]];
                    sel[j] = s > W ? static_cast<u8>(X) : s;
                }
                as.VpckMove(view.reg, vmask, a.reg, Reg{a.reg.bank, static_cast<u16>(a.reg.n + 2)},
                            sel);
            } else {
                u8 sw[4];
                FirstSwizzle(a, view, sw);
                as.Nmad(Nop::Mul, view.reg, vmask, a.reg, sw, a.neg, false, kOneConst, kSwXxxx,
                        false);
            }
        }
    }

    // ---- instrucciones de la PICA ----

    struct Dest {
        bool valid = false;
        Reg reg;
        RegState* state = nullptr;
    };

    Dest FetchDest(u32 reg) {
        Dest dest;
        if (reg < 0x10) {
            const s32 attr = rq.output_attr[reg];
            if (attr < 0 || attr >= 16) {
                return dest; // salida sin mapear: se tira, como en el hardware
            }
            dest.state = &AttrState(static_cast<u32>(attr));
        } else if (reg < 0x20) {
            dest.state = &TempState(reg - 0x10);
        } else {
            return dest;
        }
        dest.valid = true;
        dest.reg = TempReg(dest.state->base);
        return dest;
    }

    static u32 PicaMask(u32 pattern) {
        u32 mask = 0;
        for (u32 i = 0; i < 4; i++) {
            if (pattern & (8u >> i)) {
                mask |= 1u << i;
            }
        }
        return mask;
    }

    void Arithmetic(u32 opcode, u32 word) {
        const u32 pattern = rq.swizzle[word & 0x7F];
        const bool inverted = opcode == 0x18 || opcode == 0x19 || opcode == 0x1A || opcode == 0x1B;
        const u32 address = (word >> 19) & 3u;
        const u32 src1_reg = inverted ? (word >> 14) & 0x1F : (word >> 12) & 0x7F;
        const u32 src2_reg = inverted ? (word >> 7) & 0x7F : (word >> 7) & 0x1F;
        const u32 dest_reg = (word >> 21) & 0x1F;
        u32 mask = PicaMask(pattern);
        pin_stamp++;

        if (opcode == 0x2E || opcode == 0x2F) { // CMP
            Compare(word, pattern, src1_reg, src2_reg, address);
            return;
        }
        if (opcode == 0x12) { // MOVA
            const u32 amask = mask & 3u;
            if (amask == 0) {
                return;
            }
            const Src a = Fetch(src1_reg, address, pattern, 0, amask);
            MoveAddress(a, amask);
            return;
        }

        const Dest dest = FetchDest(dest_reg);
        if (!dest.valid || mask == 0) {
            return;
        }
        // Que componentes de cada fuente se leen de verdad.
        u32 used1 = mask;
        u32 used2 = mask;
        switch (opcode) {
        case 0x01: // DP3
            used1 = used2 = 7;
            break;
        case 0x02: // DP4
        case 0x03: // DPH
        case 0x18:
            used1 = used2 = 15;
            break;
        case 0x05: // EX2
        case 0x06: // LG2
        case 0x0E: // RCP
        case 0x0F: // RSQ
            used1 = 1;
            used2 = 0;
            break;
        case 0x0B: // FLR
        case 0x13: // MOV
            used2 = 0;
            break;
        default:
            break;
        }
        const Src a = Fetch(src1_reg, inverted ? 0 : address, pattern, 0, used1);
        Src b;
        if (used2 != 0) {
            b = Fetch(src2_reg, inverted ? address : 0, pattern, 1, used2);
        }
        if (failed) {
            return;
        }

        switch (opcode) {
        case 0x00: // ADD
            Binary(Nop::Add, dest.reg, mask, a, b);
            break;
        case 0x08: // MUL
            Binary(Nop::Mul, dest.reg, mask, a, b);
            break;
        case 0x0C: // MAX
            Binary(Nop::Max, dest.reg, mask, a, b);
            break;
        case 0x0D: // MIN
            Binary(Nop::Min, dest.reg, mask, a, b);
            break;
        case 0x13: // MOV
            Move(dest.reg, mask, a);
            break;
        case 0x01: // DP3
            Dot(dest.reg, mask, a, b, false, false);
            break;
        case 0x02: // DP4
            Dot(dest.reg, mask, a, b, true, false);
            break;
        case 0x03: // DPH
        case 0x18: // DPHI
            Dot(dest.reg, mask, a, b, true, true);
            break;
        case 0x09: // SGE
        case 0x1A: // SGEI
            SetOnCompare(dest.reg, mask, a, b, true);
            break;
        case 0x0A: // SLT
        case 0x1B: // SLTI
            SetOnCompare(dest.reg, mask, a, b, false);
            break;
        case 0x0B: // FLR
            Floor(dest.reg, mask, a);
            break;
        case 0x0E: // RCP
            Scalar(Comp::Rcp, dest.reg, mask, a, Cmp::Ne);
            break;
        case 0x0F: // RSQ
            Scalar(Comp::Rsq, dest.reg, mask, a, Cmp::Gt);
            break;
        case 0x05: // EX2
            Scalar(Comp::Exp, dest.reg, mask, a, Cmp::Eq, false);
            break;
        case 0x06: // LG2
            Scalar(Comp::Log, dest.reg, mask, a, Cmp::Eq, false);
            break;
        default:
            Fail("usse: aritmetica");
            return;
        }
        if (opcode == 0x0E || opcode == 0x0F) {
            // Con la guarda puede no escribirse: lo de antes cuenta como leido.
            NoteRead(*dest.state, mask);
        } else {
            NoteWrite(*dest.state, mask);
        }
    }

    void MultiplyAdd(u32 opcode, u32 word) {
        const bool inverted = opcode < 0x38;
        const u32 pattern = rq.swizzle[word & 0x1F];
        const u32 address = (word >> 22) & 3u;
        const u32 src1_reg = (word >> 17) & 0x1F;
        const u32 src2_reg = inverted ? (word >> 12) & 0x1F : (word >> 10) & 0x7F;
        const u32 src3_reg = inverted ? (word >> 5) & 0x7F : (word >> 5) & 0x1F;
        const u32 dest_reg = (word >> 24) & 0x1F;
        const u32 mask = PicaMask(pattern);
        pin_stamp++;
        const Dest dest = FetchDest(dest_reg);
        if (!dest.valid || mask == 0) {
            return;
        }
        Src a = Fetch(src1_reg, 0, pattern, 0, mask);
        Src b = Fetch(src2_reg, inverted ? 0 : address, pattern, 1, mask);
        const Src c = Fetch(src3_reg, inverted ? address : 0, pattern, 2, mask);
        if (failed) {
            return;
        }
        // El producto, entero, al interno 0; la suma, por mitades al destino.
        const View whole = Whole(0);
        if (!TryNmad(Nop::Mul, whole, mask, a, b, true)) {
            const bool sign = a.neg != b.neg;
            b.neg = false;
            b = Materialize(b, 2);
            a.neg = sign;
            TryNmad(Nop::Mul, whole, mask, a, b, false);
        }
        const Src product = Identity(Reg{Bank::Internal, 0});
        if ((mask & 3u) != 0 && (mask & 12u) != 0 && Overlaps(dest.reg, c)) {
            // La segunda mitad leeria lo que acaba de escribir la primera.
            u8 sw[4];
            FirstSwizzle(c, whole, sw);
            as.Nmad(Nop::Add, whole.reg, mask, c.reg, sw, c.neg, false, product.reg, kSwXyzw,
                    false);
            Store(0, dest.reg, mask);
            NoteWrite(*dest.state, mask);
            return;
        }
        for (u32 half = 0; half < 2; half++) {
            const View view = Half(dest.reg, half);
            const u32 vmask = ViewMask(view, mask);
            if (vmask == 0) {
                continue;
            }
            u8 sw[4];
            FirstSwizzle(c, view, sw);
            const int p = SecondSwizzle(product, view, vmask);
            as.Nmad(Nop::Add, view.reg, vmask, c.reg, sw, c.neg, false, product.reg,
                    static_cast<u32>(p), false);
        }
        NoteWrite(*dest.state, mask);
    }

    /// Producto escalar: una fuente a un interno y la otra, con swizzle libre,
    /// contra el. El resultado, a todas las componentes de la mascara.
    void Dot(Reg dest, u32 mask, Src a, Src b, bool vec4, bool homogeneous) {
        if (homogeneous) {
            a.sel[3] = One; // DPH: (a.xyz, 1) . b
        }
        const bool neg = a.neg != b.neg;
        a.neg = false;
        b.neg = false;
        if (b.reg.bank == Bank::Const) {
            // Una entrada que nadie carga (ceros). La fuente con swizzle libre del VDP
            // nunca es del banco de constantes en lo que compila Sony (0 de 57.000):
            // se cambian los papeles, que la otra va copiada a un interno.
            if (a.reg.bank == Bank::Const) {
                Move(dest, mask, b); // ceros por ceros (con DPH tambien: b.w es 0)
                return;
            }
            std::swap(a, b);
        }
        Materialize(a, 1);
        u8 sw[4] = {b.sel[0], b.sel[1], b.sel[2], b.sel[3]};
        u32 count = 0;
        u32 only = 0;
        for (u32 i = 0; i < 4; i++) {
            if ((mask >> i) & 1u) {
                count++;
                only = i;
            }
        }
        if (count == 1) {
            const Reg half{dest.bank, static_cast<u16>(dest.n + (only & 2u))};
            as.Vdp(vec4, half, 1u << (only & 1u), 1, b.reg, sw, neg);
            return;
        }
        as.Vdp(vec4, Reg{Bank::Internal, 0}, 1, 1, b.reg, sw, neg);
        StoreScalar(0, dest, mask);
    }

    /// SGE / SLT: la diferencia de cada mitad a un interno y la seleccion de 1 o 0.
    void SetOnCompare(Reg dest, u32 mask, const Src& a, const Src& b, bool greater_equal) {
        Src minus_b = b;
        minus_b.neg = !b.neg;
        Src aa = a;
        const bool both = (mask & 3u) != 0 && (mask & 12u) != 0;
        if (aa.neg || (both && Overlaps(dest, aa))) {
            aa = Materialize(aa, 1);
        }
        if (both && Overlaps(dest, minus_b)) {
            // Ya con su signo cambiado.
            minus_b = Materialize(minus_b, 2);
        }
        const Reg diff{Bank::Internal, 0};
        for (u32 half = 0; half < 2; half++) {
            const View target = Half(dest, half);
            const u32 vmask = ViewMask(target, mask);
            if (vmask == 0) {
                continue;
            }
            View view;
            view.reg = diff;
            view.n = 2;
            view.comp[0] = static_cast<u8>(half * 2);
            view.comp[1] = static_cast<u8>(half * 2 + 1);
            const u32 pica_half_mask = mask & (3u << (half * 2));
            if (SecondSwizzle(aa, view, vmask) < 0) {
                aa = Materialize(aa, 1);
            }
            u8 sw[4];
            FirstSwizzle(minus_b, view, sw);
            const int p = SecondSwizzle(aa, view, vmask);
            (void)pica_half_mask;
            as.Nmad(Nop::Add, diff, vmask, minus_b.reg, sw, minus_b.neg, false, aa.reg,
                    static_cast<u32>(p), false);
            // diff < 0 es "a < b".
            if (greater_equal) {
                as.Vmovc(target.reg, vmask, diff, 2, kZeroConst, kOneConst, kSwXyzw);
            } else {
                as.Vmovc(target.reg, vmask, diff, 2, kOneConst, kZeroConst, kSwXyzw);
            }
        }
    }

    /// floor(a) = -(0 - floor(a)): el VFRC de la USSE da s1 - floor(s2).
    void Floor(Reg dest, u32 mask, const Src& a) {
        Src value = a;
        const View whole = Whole(0);
        if (SecondSwizzle(value, whole, mask) < 0) {
            value = Materialize(value, 1);
        }
        const u8 zeros[4] = {X, X, X, X};
        const int p = SecondSwizzle(value, whole, mask);
        as.Nmad(Nop::Frc, whole.reg, mask, kZeroConst, zeros, false, false, value.reg,
                static_cast<u32>(p), false);
        Src negative = Identity(whole.reg);
        negative.neg = true;
        for (u32 half = 0; half < 2; half++) {
            const View view = Half(dest, half);
            const u32 vmask = ViewMask(view, mask);
            if (vmask == 0) {
                continue;
            }
            u8 sw[4];
            FirstSwizzle(negative, view, sw);
            as.Nmad(Nop::Mul, view.reg, vmask, negative.reg, sw, true, false, kOneConst, kSwXxxx,
                    false);
        }
    }

    /// RCP, RSQ, EX2, LG2: escalares sobre la x de la fuente. Las dos primeras
    /// llevan la misma guarda que la traduccion a Cg (y que la de GLSL): si el
    /// valor no vale, el destino se queda como estaba.
    void Scalar(Comp op, Reg dest, u32 mask, const Src& a, Cmp guard, bool guarded = true) {
        const Reg value{Bank::Internal, 0};
        const u8 sw[4] = {a.sel[0], a.sel[0], a.sel[0], a.sel[0]};
        as.Nmad(Nop::Mul, value, 1, a.reg, sw, a.neg, false, kOneConst, kSwXxxx, false);
        Pred pred = Pred::None;
        if (guarded) {
            as.TestZero(guard, value, false, false);
            pred = Pred::P0;
        }
        u32 count = 0;
        u32 only = 0;
        for (u32 i = 0; i < 4; i++) {
            if ((mask >> i) & 1u) {
                count++;
                only = i;
            }
        }
        if (count == 1) {
            const Reg half{dest.bank, static_cast<u16>(dest.n + (only & 2u))};
            as.Vcomp(op, half, 1u << (only & 1u), value, 0, pred);
            return;
        }
        as.Vcomp(op, value, 1, value, 0, pred);
        StoreScalar(0, dest, mask, pred);
    }

    /// CMP: la diferencia de x e y a un interno y, con ella, un 1 o un 0 en cada
    /// bandera (dos palabras de un temporal).
    void Compare(u32 word, u32 pattern, u32 src1_reg, u32 src2_reg, u32 address) {
        const u32 op_x = (word >> 24) & 7u;
        const u32 op_y = (word >> 21) & 7u;
        if (op_x > 5 || op_y > 5) {
            Fail("usse: comparacion");
            return;
        }
        const Src a = Fetch(src1_reg, address, pattern, 0, 3);
        const Src b = Fetch(src2_reg, 0, pattern, 1, 3);
        if (failed) {
            return;
        }
        Src minus_b = b;
        minus_b.neg = !b.neg;
        Src aa = a;
        View view;
        view.reg = Reg{Bank::Internal, 0};
        view.n = 2;
        view.comp[0] = 0;
        view.comp[1] = 1;
        if (SecondSwizzle(aa, view, 3) < 0) {
            aa = Materialize(aa, 1);
        }
        u8 sw[4];
        FirstSwizzle(minus_b, view, sw);
        as.Nmad(Nop::Add, view.reg, 3, minus_b.reg, sw, minus_b.neg, false, aa.reg,
                static_cast<u32>(SecondSwizzle(aa, view, 3)), false);
        const Reg flags = CmpReg();
        const auto select = [&](u32 op, u32 mask) {
            // Con d = a - b: que prueba y cual de los dos valores va si se cumple.
            switch (op) {
            case 0: // ==
                as.Vmovc(flags, mask, view.reg, 1, kOneConst, kZeroConst, kSwXyzw);
                break;
            case 1: // !=
                as.Vmovc(flags, mask, view.reg, 1, kZeroConst, kOneConst, kSwXyzw);
                break;
            case 2: // <
                as.Vmovc(flags, mask, view.reg, 2, kOneConst, kZeroConst, kSwXyzw);
                break;
            case 3: // <=
                as.Vmovc(flags, mask, view.reg, 3, kOneConst, kZeroConst, kSwXyzw);
                break;
            case 4: // >
                as.Vmovc(flags, mask, view.reg, 3, kZeroConst, kOneConst, kSwXyzw);
                break;
            default: // >=
                as.Vmovc(flags, mask, view.reg, 2, kZeroConst, kOneConst, kSwXyzw);
                break;
            }
        };
        if (op_x == op_y) {
            select(op_x, 3);
        } else {
            select(op_x, 1);
            select(op_y, 2);
        }
    }

    /// MOVA: el registro de direccion es el valor truncado hacia cero, que se
    /// guarda en coma flotante (siempre es un entero pequeno).
    void MoveAddress(const Src& a, u32 mask) {
        const Reg value{Bank::Internal, 2};
        const Reg down{Bank::Internal, 0};
        const Reg up{Bank::Internal, 1};
        const u8 sw[4] = {a.sel[0], a.sel[1], a.sel[0], a.sel[1]};
        as.Nmad(Nop::Mul, value, 3, a.reg, sw, a.neg, false, kOneConst, kSwXxxx, false);
        // 0 - floor(|v|), y su opuesto: el primero si v es negativo.
        const u8 zeros[4] = {X, X, X, X};
        const u8 xy[4] = {X, Y, X, Y};
        as.Nmad(Nop::Frc, down, 3, kZeroConst, zeros, false, false, value, kSwXyzw, true);
        as.Nmad(Nop::Mul, up, 3, down, xy, true, false, kOneConst, kSwXxxx, false);
        as.Vmovc(AddrReg(0), mask, value, 2, down, up, kSwXyzw);
        if (mask & 1u) {
            ForgetIndexed(1);
        }
        if (mask & 2u) {
            ForgetIndexed(2);
        }
    }

    // ---- control de flujo ----

    /// Deja en p0 la condicion de un salto por las banderas de CMP. Devuelve con
    /// que predicado se cumple.
    Pred Condition(u32 word) {
        const u32 op = (word >> 22) & 3u;
        const bool ref_x = ((word >> 25) & 1u) != 0;
        const bool ref_y = ((word >> 24) & 1u) != 0;
        const Reg flags = CmpReg();
        switch (op) {
        case 2: // solo x
            as.TestZero(Cmp::Ne, flags, false, false);
            return ref_x ? Pred::P0 : Pred::NotP0;
        case 3: // solo y
            as.TestZero(Cmp::Ne, flags, true, false);
            return ref_y ? Pred::P0 : Pred::NotP0;
        case 1: // las dos
            as.TestZero(ref_x ? Cmp::Ne : Cmp::Eq, flags, false, false);
            as.TestZero(ref_y ? Cmp::Ne : Cmp::Eq, flags, true, true);
            return Pred::P0;
        default: // alguna: p0 = ninguna de las dos
            as.TestZero(ref_x ? Cmp::Eq : Cmp::Ne, flags, false, false);
            as.TestZero(ref_y ? Cmp::Eq : Cmp::Ne, flags, true, true);
            return Pred::NotP0;
        }
    }

    /// p0 = el uniforme booleano 'id' esta puesto.
    void BoolTest(u32 id) {
        uses_bools = true;
        pin_stamp++;
        const Reg value = LoadWords(kUniformBoolWord + id);
        as.TestZero(Cmp::Ne, value, false, false);
    }

    static Pred Not(Pred p) {
        return p == Pred::P0 ? Pred::NotP0 : Pred::P0;
    }

    void FlowBreak() {
        linear = false;
    }

    bool EmitFrame(Frame& frame, u32 depth) {
        u32 pc = frame.begin;
        while (pc < frame.end) {
            if (failed) {
                return false;
            }
            const auto label = frame.labels.find(pc);
            if (label != frame.labels.end()) {
                as.Bind(label->second);
                frame.labels.erase(label);
                ForgetUniforms();
                FlowBreak();
            }
            if (pc >= kProgramWords) {
                return Fail("usse: sin END");
            }
            if (++emitted_pica > 8192 || as.code.size() > kMaxInstructions) {
                return Fail("usse: demasiado grande");
            }
            const u32 word = rq.code[pc];
            const u32 opcode = word >> 26;
            if (opcode >= 0x30) { // MADI y MAD
                MultiplyAdd(opcode, word);
                pc++;
                continue;
            }
            const u32 num = word & 0xFFu;
            const u32 target = (word >> 10) & 0xFFFu;
            switch (opcode) {
            case 0x21: // NOP
                pc++;
                break;
            case 0x22: // END
                if (frame.main && frame.labels.empty()) {
                    return true; // lo que sigue no se alcanza
                }
                as.Br(end_label);
                FlowBreak();
                pc++;
                break;
            case 0x20:   // BREAK
            case 0x23: { // BREAKC
                if (loops.empty()) {
                    return Fail("usse: break sin bucle");
                }
                if (opcode == 0x23) {
                    const Pred taken = Condition(word);
                    as.Br(loops.back().break_label, taken);
                } else {
                    as.Br(loops.back().break_label);
                }
                FlowBreak();
                pc++;
                break;
            }
            case 0x24:   // CALL
            case 0x25:   // CALLC
            case 0x26: { // CALLU
                if (depth >= kMaxCallDepth || target + num > kProgramWords) {
                    return Fail("usse: llamadas");
                }
                s32 skip = -1;
                if (opcode != 0x24) {
                    skip = as.NewLabel();
                    Pred taken;
                    if (opcode == 0x25) {
                        taken = Condition(word);
                    } else {
                        BoolTest((word >> 22) & 0xFu);
                        taken = Pred::P0;
                    }
                    as.Br(skip, Not(taken));
                    FlowBreak();
                }
                Frame body{target, target + num, false, {}};
                if (!EmitFrame(body, depth + 1)) {
                    return false;
                }
                if (skip >= 0) {
                    as.Bind(skip);
                    ForgetUniforms();
                }
                pc++;
                break;
            }
            case 0x27:   // IFU
            case 0x28: { // IFC
                const u32 else_at = target;
                const u32 endif_at = target + num;
                if (else_at <= pc || endif_at > frame.end) {
                    return Fail("usse: estructura");
                }
                if (!NoLabelsInside(frame, pc, endif_at)) {
                    return false;
                }
                Pred taken;
                if (opcode == 0x28) {
                    taken = Condition(word);
                } else {
                    BoolTest((word >> 22) & 0xFu);
                    taken = Pred::P0;
                }
                const s32 else_label = as.NewLabel();
                as.Br(else_label, Not(taken));
                FlowBreak();
                Frame then_body{pc + 1, else_at, false, {}};
                if (!EmitFrame(then_body, depth)) {
                    return false;
                }
                if (num != 0) {
                    const s32 endif_label = as.NewLabel();
                    as.Br(endif_label);
                    as.Bind(else_label);
                    ForgetUniforms();
                    Frame else_body{else_at, endif_at, false, {}};
                    if (!EmitFrame(else_body, depth)) {
                        return false;
                    }
                    as.Bind(endif_label);
                } else {
                    as.Bind(else_label);
                }
                ForgetUniforms();
                pc = endif_at;
                break;
            }
            case 0x29: { // LOOP
                if (target < pc || target + 1 > frame.end || loops.size() >= loop_base.size()) {
                    return Fail("usse: estructura");
                }
                if (!NoLabelsInside(frame, pc, target + 1)) {
                    return false;
                }
                if (!EmitLoop(pc, target, (word >> 22) & 3u, depth)) {
                    return false;
                }
                pc = target + 1;
                break;
            }
            case 0x2C:   // JMPC
            case 0x2D: { // JMPU
                if (target <= pc || target >= frame.end) {
                    return Fail("usse: salto no estructurado");
                }
                if (target == pc + 1) {
                    pc++;
                    break;
                }
                Pred taken;
                if (opcode == 0x2C) {
                    taken = Condition(word);
                } else {
                    // Salta cuando el booleano vale !(num & 1).
                    BoolTest((word >> 22) & 0xFu);
                    taken = (num & 1u) == 0 ? Pred::P0 : Pred::NotP0;
                }
                auto it = frame.labels.find(target);
                if (it == frame.labels.end()) {
                    it = frame.labels.emplace(target, as.NewLabel()).first;
                }
                as.Br(it->second, taken);
                FlowBreak();
                pc++;
                break;
            }
            case 0x2A: // EMIT
            case 0x2B: // SETEMIT
                return Fail("usse: geometria");
            default:
                if (opcode <= 0x1B || opcode == 0x2E || opcode == 0x2F) {
                    Arithmetic(opcode, word);
                    pc++;
                    break;
                }
                return Fail("usse: instruccion");
            }
        }
        if (failed) {
            return false;
        }
        if (!frame.labels.empty()) {
            // Un salto a mitad de un bloque que se tradujo aparte, o al final.
            return Fail("usse: salto no estructurado");
        }
        if (frame.main) {
            return Fail("usse: sin END");
        }
        return true;
    }

    /// Ningun salto pendiente puede caer DENTRO de un bloque (IF o LOOP) que se
    /// traduce aparte: su etiqueta no se llegaria a poner.
    bool NoLabelsInside(const Frame& frame, u32 begin, u32 end) {
        for (const auto& [at, label] : frame.labels) {
            if (at > begin && at < end) {
                return Fail("usse: salto no estructurado");
            }
        }
        return true;
    }

    /**
     * LOOP: vueltas, valor inicial del registro de direccion y su paso salen del
     * uniforme entero. El contador va acotado a 255 (el de la PICA es de 8
     * bits): con el uniforme sin escribir, un bucle sin tope deja la GPU girando
     * hasta el GPUCRASH (la intro de Zafiro Alfa, 24-10).
     */
    bool EmitLoop(u32 pc, u32 last, u32 int_id, u32 depth) {
        uses_ints = true;
        pin_stamp++;
        const u32 level = static_cast<u32>(loops.size());
        if (loop_base[level] < 0) {
            loop_base[level] = AllocTemp(2);
        }
        const Reg counter = TempReg(loop_base[level]);
        const Reg setup = LoadWords(kUniformIntWord + int_id * 4);
        const Reg top = FloatLiteral(255.0f);
        const u8 xxxx[4] = {X, X, X, X};
        const u8 yyyy[4] = {Y, Y, Y, Y};
        // contador.x = min(vueltas, 255); contador.y = paso; aL = inicial.
        as.Nmad(Nop::Min, counter, 1, setup, xxxx, false, false, top, kSwXxxx, false);
        as.Vmov(counter, 2, setup, kSwZzzz);
        as.Vmov(AddrReg(1), 1, setup, kSwYyyy);
        ForgetIndexed(3);

        const s32 again = as.NewLabel();
        const s32 done = as.NewLabel();
        as.Bind(again);
        ForgetUniforms();
        FlowBreak();
        loops.push_back(LoopInfo{done});
        Frame body{pc + 1, last + 1, false, {}};
        if (!EmitFrame(body, depth)) {
            return false;
        }
        loops.pop_back();
        // aL += paso; contador -= 1; otra vuelta mientras no baje de cero.
        as.Nmad(Nop::Add, AddrReg(1), 1, AddrReg(1), xxxx, false, false, counter, kSwYyyy, false);
        as.Nmad(Nop::Add, counter, 1, kOneConst, xxxx, true, false, counter, kSwXxxx, false);
        (void)yyyy;
        as.TestZero(Cmp::Ge, counter, false, false);
        as.Br(again, Pred::P0);
        as.Bind(done);
        ForgetUniforms();
        ForgetIndexed(3);
        return true;
    }

    // ---- principio y final ----

    /// De donde sale cada componente de una salida de cuatro (o menos).
    struct OutComp {
        s32 attr = -1; ///< -1: vale uno
        u8 comp = 0;
    };

    OutComp Semantic(u32 slot) {
        OutComp result;
        const s32 packed = rq.semantic[slot];
        if (packed >= 0) {
            result.attr = packed / 4;
            result.comp = static_cast<u8>(packed & 3);
        }
        return result;
    }

    /// Escribe la mitad de una salida (dos palabras de 'o') desde dos componentes.
    void EmitOutHalf(Reg target, u32 vmask, const OutComp comps[2], bool clamp_abs) {
        for (u32 first = 0; first < 2; first++) {
            if (((vmask >> first) & 1u) == 0) {
                continue;
            }
            // Las dos juntas si salen del mismo atributo (o de ninguno).
            u32 group = 1u << first;
            if (first == 0 && ((vmask >> 1) & 1u) != 0 &&
                (comps[1].attr == comps[0].attr || comps[1].attr < 0 || comps[0].attr < 0)) {
                group = 3;
            }
            s32 attr = comps[first].attr;
            if (group == 3 && attr < 0) {
                attr = comps[1].attr;
            }
            Reg source = kOneConst;
            u8 sw[4] = {X, X, X, X};
            if (attr >= 0) {
                RegState& state = AttrState(static_cast<u32>(attr));
                source = TempReg(state.base);
                u32 read = 0;
                for (u32 j = 0; j < 2; j++) {
                    if ((group >> j) & 1u) {
                        if (comps[j].attr >= 0) {
                            sw[j] = comps[j].comp;
                            read |= 1u << comps[j].comp;
                        } else {
                            sw[j] = One;
                        }
                    }
                }
                NoteRead(state, read);
            } else {
                sw[0] = sw[1] = X; // c3.x = 1
            }
            if (clamp_abs) {
                // min(|c|, 1): el hardware dobla el color que se sale.
                as.Nmad(Nop::Min, target, group, source, sw, false, true, kOneConst, kSwXxxx,
                        false);
            } else {
                as.Nmad(Nop::Mul, target, group, source, sw, false, false, kOneConst, kSwXxxx,
                        false);
            }
            if (group == 3) {
                break;
            }
        }
    }

    void EmitOutput(u32 out_word, const u32* slots4, u32 count, bool clamp_abs) {
        const u32 first = out_word & ~1u;
        const u32 last = (out_word + count - 1) & ~1u;
        for (u32 reg = first; reg <= last; reg += 2) {
            OutComp comps[2];
            u32 vmask = 0;
            for (u32 j = 0; j < 2; j++) {
                const u32 word = reg + j;
                if (word >= out_word && word < out_word + count) {
                    comps[j] = Semantic(slots4[word - out_word]);
                    vmask |= 1u << j;
                }
            }
            EmitOutHalf(Reg{Bank::Out, static_cast<u16>(reg)}, vmask, comps, clamp_abs);
        }
    }

    bool Epilogue() {
        // Los numeros de VSOutputAttributes::Semantic.
        static constexpr u32 kPosition[4] = {0, 1, 2, 3};
        static constexpr u32 kQuaternion[4] = {4, 5, 6, 7};
        static constexpr u32 kColor[4] = {8, 9, 10, 11};
        static constexpr u32 kTc0[2] = {12, 13};
        static constexpr u32 kTc1[2] = {14, 15};
        static constexpr u32 kTc0w[1] = {16};
        static constexpr u32 kView[3] = {18, 19, 20};
        static constexpr u32 kTc2[2] = {22, 23};

        // El W-buffer necesita su uniforme ANTES de usar ningun interno.
        Reg wbuf{};
        if (rq.wbuffer) {
            uses_wbuf = true;
            pin_stamp++;
            wbuf = LoadWords(kUniformWbufWord);
        }
        if (rq.debug_position_input >= 0) {
            const u32 reg = static_cast<u32>(rq.debug_position_input);
            if (reg >= 16 || input_pa[reg] < 0) {
                return Fail("usse: entrada de posicion");
            }
            const Reg pa{Bank::Pa, static_cast<u16>(input_pa[reg])};
            as.Vmov(Reg{Bank::Out, 0}, 3, pa, kSwXyzw);
            as.Vmov(Reg{Bank::Out, 2}, 3, pa, kSwZwzw);
        } else {
            EmitOutput(0, kPosition, 4, false);
        }
        if (rq.wbuffer && rq.debug_position_input < 0) {
            // z = clamp(z * escala + w * desplazamiento, 0, 1) * w.
            const OutComp z = Semantic(2);
            const OutComp w = Semantic(3);
            const Reg i0{Bank::Internal, 0};
            const Reg i1{Bank::Internal, 1};
            const auto source = [&](const OutComp& c, u8 sw[4]) {
                if (c.attr < 0) {
                    sw[0] = sw[1] = sw[2] = sw[3] = X;
                    return kOneConst;
                }
                sw[0] = sw[1] = sw[2] = sw[3] = c.comp;
                return TempReg(AttrState(static_cast<u32>(c.attr)).base);
            };
            u8 sz[4];
            u8 sw4[4];
            const Reg rz = source(z, sz);
            const Reg rw = source(w, sw4);
            const u8 xxxx[4] = {X, X, X, X};
            as.Nmad(Nop::Mul, i0, 1, rz, sz, false, false, wbuf, kSwXxxx, false);
            as.Nmad(Nop::Mul, i1, 1, rw, sw4, false, false, wbuf, kSwYyyy, false);
            as.Nmad(Nop::Add, i0, 1, i0, xxxx, false, false, i1, kSwXxxx, false);
            as.Nmad(Nop::Max, i0, 1, i0, xxxx, false, false, kZeroConst, kSwXxxx, false);
            as.Nmad(Nop::Min, i0, 1, i0, xxxx, false, false, kOneConst, kSwXxxx, false);
            as.Nmad(Nop::Mul, Reg{Bank::Out, 2}, 1, rw, sw4, false, false, i0, kSwXxxx, false);
        }
        EmitOutput(4, kColor, 4, true);
        EmitOutput(8, kTc0, 2, false);
        EmitOutput(10, kTc1, 2, false);
        EmitOutput(12, kTc2, 2, false);
        u32 next = 14;
        const bool lit = rq.lit || rq.debug_position_input >= 0;
        if (lit) {
            if (rq.debug_position_input >= 0) {
                EmitOutput(14, kPosition, 4, false);
            } else {
                EmitOutput(14, kQuaternion, 4, false);
            }
            EmitOutput(18, kView, 3, false);
            next = 21;
        }
        if (rq.proj) {
            EmitOutput(next, kTc0w, 1, false);
        }
        return true;
    }

    void Prologue() {
        // Los atributos por defecto, a sus temporales.
        for (u32 reg = 0; reg < 16; reg++) {
            if (default_base[reg] >= 0) {
                EmitLoad(TempReg(default_base[reg]), kUniformDefaultWord + reg * 4);
            }
        }
        // Y lo que se lee antes de escribirse, a (0, 0, 0, 1), que es con lo que
        // arranca cada registro en la traduccion a Cg.
        const auto init = [this](const RegState& state) {
            if (state.base < 0 || state.need == 0) {
                return;
            }
            if (state.need & 3u) {
                as.Vmov(TempReg(state.base), state.need & 3u, kZeroConst, kSwXxxx);
            }
            if (state.need & 12u) {
                as.Vmov(TempReg(state.base, 2), (state.need >> 2) & 3u, kZeroOne, kSwXyzw);
            }
        };
        for (const RegState& state : temps) {
            init(state);
        }
        for (const RegState& state : attrs) {
            init(state);
        }
        if (cmp_base >= 0) {
            as.Vmov(TempReg(cmp_base), 3, kZeroConst, kSwXxxx);
        }
        if (addr_base >= 0) {
            as.Vmov(TempReg(addr_base), 3, kZeroConst, kSwXxxx);
            as.Vmov(TempReg(addr_base, 2), 3, kZeroConst, kSwXxxx);
        }
    }

    // ---- el contenedor GXP ----

    void Put32(std::vector<u8>& data, std::size_t at, u32 value) {
        std::memcpy(data.data() + at, &value, 4);
    }

    void Put16(std::vector<u8>& data, std::size_t at, u16 value) {
        std::memcpy(data.data() + at, &value, 2);
    }

    /**
     * El mismo orden que deja el compilador: cabecera (0x9C), salidas (32
     * bytes), programa secundario (vacio), programa primario alineado a 8,
     * literales, contenedores, buffers de uniformes, parametros y nombres. Los
     * desplazamientos de la cabecera son relativos a su propio campo.
     */
    void WriteContainer() {
        struct Param {
            std::string name;
            u8 category; ///< 0 atributo, 1 uniforme
            u8 components;
            u8 container;
            u32 array_size;
            u32 resource;
        };
        std::vector<Param> params;
        u64 attribute_mask = 0;
        for (u32 reg = 0; reg < 16; reg++) {
            if (input_pa[reg] < 0) {
                continue;
            }
            const u32 pa = static_cast<u32>(input_pa[reg]);
            params.push_back({"vs_in" + std::to_string(reg), 0, 4, 0, 1, pa});
            const u32 comps = rq.inputs[reg].components;
            attribute_mask |= static_cast<u64>((1u << (comps > 4 ? 4 : comps)) - 1u) << pa;
        }
        params.push_back({"vs_f", 1, 4, 14, 96, kUniformFloatWord});
        u32 buffer_words = kUniformIntWord;
        if (uses_ints) {
            params.push_back({"vs_i", 1, 4, 14, 4, kUniformIntWord});
            buffer_words = kUniformBoolWord;
        }
        if (uses_bools) {
            params.push_back({"vs_b", 1, 1, 14, 16, kUniformBoolWord});
            buffer_words = kUniformDefaultWord;
        }
        if (uses_defaults) {
            params.push_back({"vs_default", 1, 4, 14, 16, kUniformDefaultWord});
            buffer_words = kUniformWbufWord;
        }
        if (uses_wbuf) {
            params.push_back({"vs_wbuf", 1, 2, 14, 1, kUniformWbufWord});
            buffer_words = kUniformWords;
        }
        if (uses_bools && buffer_words < kUniformDefaultWord + 4) {
            // Los booleanos se cargan de cuatro en cuatro palabras: el ultimo
            // lee tres mas alla, que tienen que ser del buffer.
            buffer_words = kUniformDefaultWord + 4;
        }

        constexpr std::size_t kHeader = 0x9C;
        const std::size_t varyings_at = kHeader;
        const std::size_t secondary_at = varyings_at + 32;
        const std::size_t primary_at = (secondary_at + 7) & ~std::size_t{7};
        const std::size_t literals_at = primary_at + as.code.size() * 8;
        const std::size_t containers_at = literals_at + literal_list.size() * 8;
        const std::size_t buffers_at = containers_at + 8;
        const std::size_t params_at = buffers_at + 8;
        std::size_t names_at = params_at + params.size() * 16;
        std::size_t size = names_at;
        for (const Param& p : params) {
            size += p.name.size() + 1;
        }
        std::vector<u8> data((size + 3) & ~std::size_t{3}, 0);

        const auto rel = [&](std::size_t field, std::size_t target) {
            Put32(data, field, static_cast<u32>(target - field));
        };
        std::memcpy(data.data(), "GXP\0", 4);
        data[4] = 1;
        data[5] = 5;
        Put16(data, 6, 0x300);
        Put32(data, 0x08, static_cast<u32>(size));
        Put32(data, 0x14, 0x00190000);
        Put32(data, 0x18, 0x20000000); // el buffer 14, solo en memoria
        Put32(data, 0x24, static_cast<u32>(params.size()));
        rel(0x28, params_at);
        rel(0x2C, varyings_at);
        Put16(data, 0x30, static_cast<u16>(pa_words));
        Put16(data, 0x32, static_cast<u16>(sa_words));
        Put32(data, 0x34, temp_words);
        Put16(data, 0x38, 0);
        Put16(data, 0x3A, 1);
        Put32(data, 0x3C, static_cast<u32>(as.code.size()));
        rel(0x40, primary_at);
        Put32(data, 0x44, 0);
        rel(0x48, secondary_at);
        rel(0x4C, secondary_at);
        Put32(data, 0x5C, sa_words); // lo que ocupa el contenedor 19
        Put32(data, 0x64, buffer_words);
        rel(0x68, containers_at);
        Put32(data, 0x6C, 0x33A90);
        Put32(data, 0x70, static_cast<u32>(literal_list.size()));
        rel(0x74, literals_at);
        Put32(data, 0x78, 1);
        rel(0x7C, buffers_at);
        Put32(data, 0x80, 0);
        rel(0x84, containers_at);
        Put32(data, 0x88, 0);
        rel(0x8C, containers_at);
        Put32(data, 0x90, 1);
        rel(0x94, containers_at);
        Put32(data, 0x98, 0);

        // Salidas: las de la variante fija (ver kVertexSource* en rasterizer_gxm.cpp).
        const bool lit = rq.lit || rq.debug_position_input >= 0;
        u32 outputs1 = 0x0E001800;
        u32 outputs2 = 0x49;
        if (lit && rq.proj) {
            outputs1 = 0x17001800;
            outputs2 = 0xBE49;
        } else if (lit) {
            outputs1 = 0x15001800;
            outputs2 = 0x3E49;
        } else if (rq.proj) {
            outputs1 = 0x10001800;
            outputs2 = 0x8049;
        }
        std::memcpy(data.data() + varyings_at, &attribute_mask, 8);
        Put32(data, varyings_at + 16, outputs1);
        Put32(data, varyings_at + 20, outputs2);

        for (std::size_t i = 0; i < as.code.size(); i++) {
            std::memcpy(data.data() + primary_at + i * 8, &as.code[i].word, 8);
        }
        for (std::size_t i = 0; i < literal_list.size(); i++) {
            Put32(data, literals_at + i * 8, literal_list[i].first);
            Put32(data, literals_at + i * 8 + 4, literal_list[i].second);
        }
        // Contenedor 19 (literales y puntero): desde sa0, sa_words palabras.
        Put16(data, containers_at, 19);
        Put16(data, containers_at + 2, 0);
        Put16(data, containers_at + 4, 0);
        Put16(data, containers_at + 6, static_cast<u16>(sa_words));
        // Buffer 14: su direccion en sa0; el -4 es el que usa el compilador
        // cuando lo primero que se lee esta en el byte 0.
        Put16(data, buffers_at, 14);
        Put16(data, buffers_at + 2, 0);
        Put32(data, buffers_at + 4, static_cast<u32>(-4));

        for (std::size_t i = 0; i < params.size(); i++) {
            const Param& p = params[i];
            const std::size_t at = params_at + i * 16;
            Put32(data, at, static_cast<u32>(names_at - at));
            data[at + 4] = p.category; // tipo f32 = 0 en el nibble alto
            data[at + 5] = static_cast<u8>(p.components | (p.container << 4));
            Put32(data, at + 8, p.array_size);
            Put32(data, at + 12, p.resource);
            std::memcpy(data.data() + names_at, p.name.c_str(), p.name.size() + 1);
            names_at += p.name.size() + 1;
        }
        out.gxp = std::move(data);
    }
};

} // Anonymous namespace

bool GenerateVs(const VsRequest& request, VsResult& result) {
    result = VsResult{};
    if (request.code == nullptr || request.swizzle == nullptr ||
        request.main_offset >= kProgramWords) {
        result.reason = "usse: peticion";
        return false;
    }
    Translator translator{request, result};
    return translator.Run();
}

} // namespace Gxm::Usse
