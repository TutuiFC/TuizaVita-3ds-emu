// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#include "common/assert.h"
#include "common/color.h"
#include "common/common_types.h"
#include "common/microprofile.h"
#include "common/vector_math.h"
#include "core/core.h"
#include "core/hle/service/cam/y2r_u.h"
#include "core/hw/y2r.h"
#include "core/memory.h"
#ifdef __PSVITA__
#include <cstdio>
#include <new>
#include <fmt/format.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include "common/vita_diag.h"
#endif

namespace HW::Y2R {

using namespace Service::Y2R;

#ifdef __PSVITA__
namespace {
/**
 * DONDE SE VAN LOS ~10 MS DE CADA IMAGEN (0.4.0.0, diagnostico). En la intro
 * de Kirby Triple Deluxe, ya sin esperar a la GPU (0.3.9.9), StartConversion
 * sigue costando 9-12 ms por imagen, unas cinco veces lo que deberia con la
 * conversion en NEON. La configuracion de cada tipo de imagen una vez, y las
 * fases de una de cada cuatro conversiones, sumadas y escritas cada 64. Solo
 * el hilo de emulacion convierte, asi que basta con globales sueltas.
 */
unsigned long long g_phase_total[4] = {};
unsigned long long g_phase_now[4] = {};
unsigned long long g_phase_mark = 0;
bool g_phase_timed = false;
u32 g_phase_samples = 0;
u32 g_phase_tick = 0;
u32 g_seen[8] = {};
u32 g_seen_count = 0;

/// Cierra la fase 'index' (0 recibir, 1 convertir, 2 colocar, 3 enviar).
void Phase(u32 index) {
    if (g_phase_timed) {
        const unsigned long long now = Common::VitaMicros();
        g_phase_now[index] += now - g_phase_mark;
        g_phase_mark = now;
    }
}

void NoteConfig(const ConversionConfiguration& cvt) {
    const u32 key = cvt.input_line_width | (u32{cvt.input_lines} << 12) |
                    (static_cast<u32>(cvt.input_format) << 24) |
                    (static_cast<u32>(cvt.output_format) << 27) |
                    (static_cast<u32>(cvt.rotation) << 29) |
                    (static_cast<u32>(cvt.block_alignment) << 31);
    for (u32 i = 0; i < g_seen_count; i++) {
        if (g_seen[i] == key) {
            return;
        }
    }
    if (g_seen_count == 8) {
        return;
    }
    g_seen[g_seen_count++] = key;
    Common::VitaNote(
        "y2r",
        fmt::format("{}x{} entrada {} salida {} giro {} bloques8x8 {} | origen Y {:#010x} "
                    "(unidad {} hueco {}) YUYV {:#010x} | destino {:#010x} (unidad {} hueco {})",
                    cvt.input_line_width, cvt.input_lines,
                    static_cast<u32>(cvt.input_format), static_cast<u32>(cvt.output_format),
                    static_cast<u32>(cvt.rotation),
                    cvt.block_alignment == BlockAlignment::Block8x8 ? 1 : 0,
                    cvt.src_Y.address, cvt.src_Y.transfer_unit, cvt.src_Y.gap,
                    cvt.src_YUYV.address, cvt.dst.address, cvt.dst.transfer_unit,
                    cvt.dst.gap)
            .c_str());
}

struct PhaseReport {
    ~PhaseReport() {
        if (!g_phase_timed) {
            return;
        }
        Phase(3);
        g_phase_timed = false;
        for (u32 i = 0; i < 4; i++) {
            g_phase_total[i] += g_phase_now[i];
            g_phase_now[i] = 0;
        }
        if (++g_phase_samples < 64) {
            return;
        }
        Common::VitaNote("y2r", fmt::format("us por imagen: recibir {} convertir {} colocar {} "
                                             "enviar {} (64 muestras)",
                                             g_phase_total[0] / 64, g_phase_total[1] / 64,
                                             g_phase_total[2] / 64, g_phase_total[3] / 64)
                                     .c_str());
        for (u32 i = 0; i < 4; i++) {
            g_phase_total[i] = 0;
        }
        g_phase_samples = 0;
    }
};
} // Anonymous namespace
#endif

using namespace Service::Y2R;

static const std::size_t MAX_TILES = 1024 / 8;
static const std::size_t TILE_SIZE = 8 * 8;
using ImageTile = std::array<u32, TILE_SIZE>;

/// Converts a image strip from the source YUV format into individual 8x8 RGB32 tiles.
template <InputFormat input_format>
static void ConvertYUVToRGB(const u8* input_Y, const u8* input_U, const u8* input_V,
                            ImageTile output[], unsigned int width, unsigned int height,
                            const CoefficientSet& coefficients) {

    for (unsigned int y = 0; y < height; ++y) {
        for (unsigned int x = 0; x < width; ++x) {
            s32 Y;
            s32 U;
            s32 V;
            if constexpr (input_format == InputFormat::YUV422_Indiv8 ||
                          input_format == InputFormat::YUV422_Indiv16) {
                Y = input_Y[y * width + x];
                U = input_U[(y * width + x) / 2];
                V = input_V[(y * width + x) / 2];
            } else if constexpr (input_format == InputFormat::YUV420_Indiv8 ||
                                 input_format == InputFormat::YUV420_Indiv16) {
                Y = input_Y[y * width + x];
                U = input_U[((y / 2) * width + x) / 2];
                V = input_V[((y / 2) * width + x) / 2];
            } else if constexpr (input_format == InputFormat::YUYV422_Interleaved) {
                Y = input_Y[(y * width + x) * 2];
                U = input_Y[(y * width + (x / 2) * 2) * 2 + 1];
                V = input_Y[(y * width + (x / 2) * 2) * 2 + 3];
            } else {
                UNREACHABLE_MSG("Unknown Y2R input format {}", input_format);
                return;
            }

            // This conversion process is bit-exact with hardware, as far as could be tested.
            auto& c = coefficients;
            s32 cY = c[0] * Y;

            s32 r = cY + c[1] * V;
            s32 g = cY - c[2] * V - c[3] * U;
            s32 b = cY + c[4] * U;

            const s32 rounding_offset = 0x18;
            r = (r >> 3) + c[5] + rounding_offset;
            g = (g >> 3) + c[6] + rounding_offset;
            b = (b >> 3) + c[7] + rounding_offset;

            unsigned int tile = x / 8;
            unsigned int tile_x = x % 8;
            u32* out = &output[tile][y * 8 + tile_x];
            *out = ((u32)std::clamp(r >> 5, 0, 0xFF) << 24) |
                   ((u32)std::clamp(g >> 5, 0, 0xFF) << 16) |
                   ((u32)std::clamp(b >> 5, 0, 0xFF) << 8);
        }
    }
}

#if defined(__ARM_NEON)
/**
 * Y2R CON NEON (0.3.3.0). Yo-kai Watch convierte unas 10 imagenes por segundo
 * de 13,7 ms cada una (y2r:u::StartConversion en crash.txt de 0.3.2.6, el 14 %
 * del hilo de emulacion) e Inazuma Eleven GO 8,5 de 9,7 ms: ~60 ciclos por
 * pixel en cinco pasadas escalares. Aqui ocho pixeles de una vez con las mismas
 * cuentas: productos en 32 bits (vmull/vmlal/vmlsl), los dos desplazamientos
 * de antes y la saturacion de vqshrun + vqmovn, que es el clamp a 0-255 de
 * arriba. Comprobado en el PC contra el codigo escalar con 8,8 millones de
 * pixeles (los seis juegos de coeficientes estandar y coeficientes al azar).
 */
static inline void ConvertEightRGB(uint8x8_t y8, uint8x8_t u8x, uint8x8_t v8x,
                                   const CoefficientSet& c, uint8x8_t& r8, uint8x8_t& g8,
                                   uint8x8_t& b8) {
    const int16x8_t y = vreinterpretq_s16_u16(vmovl_u8(y8));
    const int16x8_t u = vreinterpretq_s16_u16(vmovl_u8(u8x));
    const int16x8_t v = vreinterpretq_s16_u16(vmovl_u8(v8x));
    const int32x4_t kr = vdupq_n_s32(c[5] + 0x18);
    const int32x4_t kg = vdupq_n_s32(c[6] + 0x18);
    const int32x4_t kb = vdupq_n_s32(c[7] + 0x18);
    const auto half = [&](int16x4_t yh, int16x4_t uh, int16x4_t vh, uint16x4_t& r16,
                          uint16x4_t& g16, uint16x4_t& b16) {
        const int32x4_t cy = vmull_n_s16(yh, c[0]);
        const int32x4_t r = vmlal_n_s16(cy, vh, c[1]);
        const int32x4_t g = vmlsl_n_s16(vmlsl_n_s16(cy, vh, c[2]), uh, c[3]);
        const int32x4_t b = vmlal_n_s16(cy, uh, c[4]);
        r16 = vqshrun_n_s32(vaddq_s32(vshrq_n_s32(r, 3), kr), 5);
        g16 = vqshrun_n_s32(vaddq_s32(vshrq_n_s32(g, 3), kg), 5);
        b16 = vqshrun_n_s32(vaddq_s32(vshrq_n_s32(b, 3), kb), 5);
    };
    uint16x4_t r_lo, g_lo, b_lo, r_hi, g_hi, b_hi;
    half(vget_low_s16(y), vget_low_s16(u), vget_low_s16(v), r_lo, g_lo, b_lo);
    half(vget_high_s16(y), vget_high_s16(u), vget_high_s16(v), r_hi, g_hi, b_hi);
    b8 = vqmovn_u16(vcombine_u16(b_lo, b_hi));
    g8 = vqmovn_u16(vcombine_u16(g_lo, g_hi));
    r8 = vqmovn_u16(vcombine_u16(r_lo, r_hi));
}

static inline void ConvertEight(uint8x8_t y8, uint8x8_t u8x, uint8x8_t v8x,
                                const CoefficientSet& c, u32* out) {
    const int16x8_t y = vreinterpretq_s16_u16(vmovl_u8(y8));
    const int16x8_t u = vreinterpretq_s16_u16(vmovl_u8(u8x));
    const int16x8_t v = vreinterpretq_s16_u16(vmovl_u8(v8x));
    const int32x4_t kr = vdupq_n_s32(c[5] + 0x18);
    const int32x4_t kg = vdupq_n_s32(c[6] + 0x18);
    const int32x4_t kb = vdupq_n_s32(c[7] + 0x18);
    const auto half = [&](int16x4_t yh, int16x4_t uh, int16x4_t vh, uint16x4_t& r16,
                          uint16x4_t& g16, uint16x4_t& b16) {
        const int32x4_t cy = vmull_n_s16(yh, c[0]);
        const int32x4_t r = vmlal_n_s16(cy, vh, c[1]);
        const int32x4_t g = vmlsl_n_s16(vmlsl_n_s16(cy, vh, c[2]), uh, c[3]);
        const int32x4_t b = vmlal_n_s16(cy, uh, c[4]);
        r16 = vqshrun_n_s32(vaddq_s32(vshrq_n_s32(r, 3), kr), 5);
        g16 = vqshrun_n_s32(vaddq_s32(vshrq_n_s32(g, 3), kg), 5);
        b16 = vqshrun_n_s32(vaddq_s32(vshrq_n_s32(b, 3), kb), 5);
    };
    uint16x4_t r_lo, g_lo, b_lo, r_hi, g_hi, b_hi;
    half(vget_low_s16(y), vget_low_s16(u), vget_low_s16(v), r_lo, g_lo, b_lo);
    half(vget_high_s16(y), vget_high_s16(u), vget_high_s16(v), r_hi, g_hi, b_hi);
    // Bytes 0, B, G, R: en little-endian, R << 24 | G << 16 | B << 8.
    uint8x8x4_t pixels;
    pixels.val[0] = vdup_n_u8(0);
    pixels.val[1] = vqmovn_u16(vcombine_u16(b_lo, b_hi));
    pixels.val[2] = vqmovn_u16(vcombine_u16(g_lo, g_hi));
    pixels.val[3] = vqmovn_u16(vcombine_u16(r_lo, r_hi));
    vst4_u8(reinterpret_cast<u8*>(out), pixels);
}

/// Cuatro bytes de croma, cada uno en dos carriles seguidos (un valor por par
/// de pixeles).
static inline uint8x8_t LoadChromaPairs(const u8* in) {
    u32 word;
    std::memcpy(&word, in, sizeof(word));
    const uint8x8_t v = vreinterpret_u8_u32(vdup_n_u32(word));
    return vzip_u8(v, v).val[0];
}

/// Ocho pixeles de la fila 'y' desde la columna 'x', como los lee ConvertYUVToRGBNeon.
template <InputFormat input_format>
static inline void LoadEight(const u8* input_Y, const u8* input_U, const u8* input_V,
                             unsigned int width, unsigned int y, unsigned int x,
                             uint8x8_t& y8, uint8x8_t& u8x, uint8x8_t& v8x) {
    if constexpr (input_format == InputFormat::YUYV422_Interleaved) {
        const uint8x8x2_t yc = vld2_u8(input_Y + (y * width + x) * 2);
        y8 = yc.val[0];
        const uint8x8x2_t uv = vuzp_u8(yc.val[1], yc.val[1]);
        u8x = vzip_u8(uv.val[0], uv.val[0]).val[0];
        v8x = vzip_u8(uv.val[1], uv.val[1]).val[0];
    } else if constexpr (input_format == InputFormat::YUV422_Indiv8 ||
                         input_format == InputFormat::YUV422_Indiv16) {
        y8 = vld1_u8(input_Y + y * width + x);
        u8x = LoadChromaPairs(input_U + (y * width + x) / 2);
        v8x = LoadChromaPairs(input_V + (y * width + x) / 2);
    } else {
        y8 = vld1_u8(input_Y + y * width + x);
        u8x = LoadChromaPairs(input_U + ((y / 2) * width + x) / 2);
        v8x = LoadChromaPairs(input_V + ((y / 2) * width + x) / 2);
    }
}

/**
 * UNA FRANJA EN UNA PASADA (0.4.0.1). Con bloques de 8x8 y sin giro, que es lo
 * que piden los videos (texturas de la PICA), la franja hacia cuatro pasadas:
 * convertir a mosaicos de RGB32, reordenar cada mosaico en Z con la tabla,
 * copiarlo al bufer de salida y codificarlo al formato final. En la intro de
 * Kirby Triple Deluxe (400x240, RGB8) eran ~4,1 ms convertir y ~3,7 ms
 * colocar por imagen (crash.txt de 0.4.0.0). Aqui cada par de filas se
 * convierte y se escribe ya en su sitio del mosaico y en su formato: en el
 * orden Z de 8x8, las filas 2k y 2k+1 de las columnas 0-3 son ocho pixeles
 * seguidos (pares de columnas alternando fila), y las columnas 4-7 los ocho
 * siguientes a 16 de distancia. Eso es un vzip de pares de bytes por canal.
 */
template <OutputFormat output_format>
static inline void StoreEight(u8* out, uint8x8_t r, uint8x8_t g, uint8x8_t b, u8 alpha) {
    if constexpr (output_format == OutputFormat::RGBA8) {
        // Bytes A, B, G, R: ver EncodeRGBA8.
        uint8x8x4_t px;
        px.val[0] = vdup_n_u8(alpha);
        px.val[1] = b;
        px.val[2] = g;
        px.val[3] = r;
        vst4_u8(out, px);
    } else if constexpr (output_format == OutputFormat::RGB8) {
        uint8x8x3_t px;
        px.val[0] = b;
        px.val[1] = g;
        px.val[2] = r;
        vst3_u8(out, px);
    } else if constexpr (output_format == OutputFormat::RGB565) {
        uint16x8_t x = vshll_n_u8(r, 8);
        x = vsriq_n_u16(x, vshll_n_u8(g, 8), 5);
        x = vsriq_n_u16(x, vshll_n_u8(b, 8), 11);
        vst1q_u8(out, vreinterpretq_u8_u16(x));
    } else {
        uint16x8_t x = vshll_n_u8(r, 8);
        x = vsriq_n_u16(x, vshll_n_u8(g, 8), 5);
        x = vsriq_n_u16(x, vshll_n_u8(b, 8), 10);
        x = vsriq_n_u16(x, vshll_n_u8(vdup_n_u8(alpha), 8), 15);
        vst1q_u8(out, vreinterpretq_u8_u16(x));
    }
}

/**
 * LAS CUENTAS, MAS CORTAS (0.4.1.4). El banco de Y2R (RunBenchIfAny) midio 5,3 ms
 * por imagen de 400x240 SOLO de convertir, con todo en la cache: ~24 ciclos por
 * pixel, porque GCC guardaba y recargaba de la pila mas de 600 bytes de
 * registros NEON. Dos cambios, con el mismo resultado bit a bit:
 *  - ((x >> 3) + k) >> 5 es (x + 8k) >> 8 para cualquier entero x: floor(x/8) + k
 *    es entero, y lo que (x/8 + k) tiene de mas es menor que 1, que no cambia
 *    la division por 32. Asi la constante de cada canal va ya dentro de la
 *    parte de la croma y queda un solo desplazamiento con saturacion.
 *  - La croma, con su constante, se cuenta una vez por par de filas en YUV420
 *    (lo que piden los videos): las dos filas de un par la comparten.
 * Los coeficientes van en carriles de registros NEON (vmlal_lane), no como
 * escalares que GCC recargaba. Y las primeras kFusedChecks imagenes se siguen
 * comparando con el camino de siempre (ver PerformConversion).
 */
struct ChromaTerms {
    int32x4_t r_lo, r_hi, g_lo, g_hi, b_lo, b_hi;
};

/// La croma de ocho pixeles (cada muestra en dos carriles) con la constante de
/// cada canal ya sumada; k14 lleva c[1]..c[4] en sus carriles 0..3.
static inline ChromaTerms ChromaFor(uint8x8_t u8x, uint8x8_t v8x, int16x4_t k14, int32x4_t kr,
                                    int32x4_t kg, int32x4_t kb) {
    const int16x8_t u = vreinterpretq_s16_u16(vmovl_u8(u8x));
    const int16x8_t v = vreinterpretq_s16_u16(vmovl_u8(v8x));
    const int16x4_t ul = vget_low_s16(u);
    const int16x4_t uh = vget_high_s16(u);
    const int16x4_t vl = vget_low_s16(v);
    const int16x4_t vh = vget_high_s16(v);
    ChromaTerms t;
    t.r_lo = vmlal_lane_s16(kr, vl, k14, 0);
    t.r_hi = vmlal_lane_s16(kr, vh, k14, 0);
    t.g_lo = vmlsl_lane_s16(vmlsl_lane_s16(kg, vl, k14, 1), ul, k14, 2);
    t.g_hi = vmlsl_lane_s16(vmlsl_lane_s16(kg, vh, k14, 1), uh, k14, 2);
    t.b_lo = vmlal_lane_s16(kb, ul, k14, 3);
    t.b_hi = vmlal_lane_s16(kb, uh, k14, 3);
    return t;
}

/// Ocho pixeles: la luma (c[0] en el carril 0 de k0) sobre la croma ya contada.
static inline void LumaApply(uint8x8_t y8, const ChromaTerms& t, int16x4_t k0, uint8x8_t& r8,
                             uint8x8_t& g8, uint8x8_t& b8) {
    const int16x8_t y = vreinterpretq_s16_u16(vmovl_u8(y8));
    const int16x4_t yl = vget_low_s16(y);
    const int16x4_t yh = vget_high_s16(y);
    r8 = vqmovn_u16(vcombine_u16(vqshrun_n_s32(vmlal_lane_s16(t.r_lo, yl, k0, 0), 8),
                                 vqshrun_n_s32(vmlal_lane_s16(t.r_hi, yh, k0, 0), 8)));
    g8 = vqmovn_u16(vcombine_u16(vqshrun_n_s32(vmlal_lane_s16(t.g_lo, yl, k0, 0), 8),
                                 vqshrun_n_s32(vmlal_lane_s16(t.g_hi, yh, k0, 0), 8)));
    b8 = vqmovn_u16(vcombine_u16(vqshrun_n_s32(vmlal_lane_s16(t.b_lo, yl, k0, 0), 8),
                                 vqshrun_n_s32(vmlal_lane_s16(t.b_hi, yh, k0, 0), 8)));
}

template <InputFormat input_format, OutputFormat output_format>
static void ConvertStripTiled(const u8* input_Y, const u8* input_U, const u8* input_V,
                              unsigned int width, const CoefficientSet& c, u8 alpha,
                              u8* out) {
    constexpr u32 bpp = output_format == OutputFormat::RGBA8  ? 4
                        : output_format == OutputFormat::RGB8 ? 3
                                                              : 2;
    // Las dos filas de un par comparten la croma (ver LAS CUENTAS, MAS CORTAS).
    constexpr bool kSharedChroma = input_format == InputFormat::YUV420_Indiv8 ||
                                   input_format == InputFormat::YUV420_Indiv16;
    // Primer pixel en Z de las filas 0-1, 2-3, 4-5 y 6-7 (ver morton_lut).
    static constexpr u8 kPairBase[4] = {0, 8, 32, 40};
    const int16x4_t k0 = vdup_n_s16(c[0]);
    const int16x4_t k14 = vld1_s16(c.data() + 1);
    const int32x4_t kr = vdupq_n_s32((c[5] + 0x18) * 8);
    const int32x4_t kg = vdupq_n_s32((c[6] + 0x18) * 8);
    const int32x4_t kb = vdupq_n_s32((c[7] + 0x18) * 8);
    for (unsigned int x = 0; x < width; x += 8) {
        u8* const tile = out + (x / 8) * 64 * bpp;
        for (unsigned int pair = 0; pair < 4; pair++) {
            uint8x8_t y8, u8x, v8x, r0, g0, b0, r1, g1, b1;
            LoadEight<input_format>(input_Y, input_U, input_V, width, pair * 2, x, y8, u8x,
                                    v8x);
            const ChromaTerms t0 = ChromaFor(u8x, v8x, k14, kr, kg, kb);
            LumaApply(y8, t0, k0, r0, g0, b0);
            if constexpr (kSharedChroma) {
                y8 = vld1_u8(input_Y + (pair * 2 + 1) * width + x);
                LumaApply(y8, t0, k0, r1, g1, b1);
            } else {
                LoadEight<input_format>(input_Y, input_U, input_V, width, pair * 2 + 1, x, y8,
                                        u8x, v8x);
                const ChromaTerms t1 = ChromaFor(u8x, v8x, k14, kr, kg, kb);
                LumaApply(y8, t1, k0, r1, g1, b1);
            }
            const uint16x4x2_t rz = vzip_u16(vreinterpret_u16_u8(r0), vreinterpret_u16_u8(r1));
            const uint16x4x2_t gz = vzip_u16(vreinterpret_u16_u8(g0), vreinterpret_u16_u8(g1));
            const uint16x4x2_t bz = vzip_u16(vreinterpret_u16_u8(b0), vreinterpret_u16_u8(b1));
            StoreEight<output_format>(tile + kPairBase[pair] * bpp,
                                      vreinterpret_u8_u16(rz.val[0]),
                                      vreinterpret_u8_u16(gz.val[0]),
                                      vreinterpret_u8_u16(bz.val[0]), alpha);
            StoreEight<output_format>(tile + (kPairBase[pair] + 16) * bpp,
                                      vreinterpret_u8_u16(rz.val[1]),
                                      vreinterpret_u8_u16(gz.val[1]),
                                      vreinterpret_u8_u16(bz.val[1]), alpha);
        }
    }
}

template <InputFormat input_format>
static void FusedStrip(const u8* input_Y, const u8* input_U, const u8* input_V,
                       const ConversionConfiguration& cvt, u8* out) {
    const u8 alpha = static_cast<u8>(cvt.alpha);
    switch (cvt.output_format) {
    case OutputFormat::RGBA8:
        ConvertStripTiled<input_format, OutputFormat::RGBA8>(
            input_Y, input_U, input_V, cvt.input_line_width, cvt.coefficients, alpha, out);
        break;
    case OutputFormat::RGB8:
        ConvertStripTiled<input_format, OutputFormat::RGB8>(
            input_Y, input_U, input_V, cvt.input_line_width, cvt.coefficients, alpha, out);
        break;
    case OutputFormat::RGB5A1:
        ConvertStripTiled<input_format, OutputFormat::RGB5A1>(
            input_Y, input_U, input_V, cvt.input_line_width, cvt.coefficients, alpha, out);
        break;
    case OutputFormat::RGB565:
        ConvertStripTiled<input_format, OutputFormat::RGB565>(
            input_Y, input_U, input_V, cvt.input_line_width, cvt.coefficients, alpha, out);
        break;
    }
}

/// Lo mismo que ConvertYUVToRGB, de ocho en ocho (el ancho siempre es multiplo
/// de 8).
template <InputFormat input_format>
static void ConvertYUVToRGBNeon(const u8* input_Y, const u8* input_U, const u8* input_V,
                                ImageTile output[], unsigned int width, unsigned int height,
                                const CoefficientSet& coefficients) {
    for (unsigned int y = 0; y < height; ++y) {
        for (unsigned int x = 0; x < width; x += 8) {
            uint8x8_t y8;
            uint8x8_t u8x;
            uint8x8_t v8x;
            if constexpr (input_format == InputFormat::YUYV422_Interleaved) {
                // Y0 U0 Y1 V0 Y2 U1 Y3 V1...: pares los Y, impares U y V alternados.
                const uint8x8x2_t yc = vld2_u8(input_Y + (y * width + x) * 2);
                y8 = yc.val[0];
                const uint8x8x2_t uv = vuzp_u8(yc.val[1], yc.val[1]);
                u8x = vzip_u8(uv.val[0], uv.val[0]).val[0];
                v8x = vzip_u8(uv.val[1], uv.val[1]).val[0];
            } else if constexpr (input_format == InputFormat::YUV422_Indiv8 ||
                                 input_format == InputFormat::YUV422_Indiv16) {
                y8 = vld1_u8(input_Y + y * width + x);
                u8x = LoadChromaPairs(input_U + (y * width + x) / 2);
                v8x = LoadChromaPairs(input_V + (y * width + x) / 2);
            } else {
                y8 = vld1_u8(input_Y + y * width + x);
                u8x = LoadChromaPairs(input_U + ((y / 2) * width + x) / 2);
                v8x = LoadChromaPairs(input_V + ((y / 2) * width + x) / 2);
            }
            ConvertEight(y8, u8x, v8x, coefficients, &output[x / 8][y * 8]);
        }
    }
}
#endif

static bool g_skip_convert = false;

template <InputFormat input_format>
static void ConvertYUV(const u8* input_Y, const u8* input_U, const u8* input_V, ImageTile output[],
                       unsigned int width, unsigned int height,
                       const CoefficientSet& coefficients) {
    if (g_skip_convert) {
        return;
    }
#ifdef __PSVITA__
    Phase(0); // ReceiveData acaba de traer la franja
    struct EndConvert {
        ~EndConvert() {
            Phase(1);
        }
    } const end_convert;
#endif
#if defined(__ARM_NEON)
    ConvertYUVToRGBNeon<input_format>(input_Y, input_U, input_V, output, width, height,
                                      coefficients);
#else
    ConvertYUVToRGB<input_format>(input_Y, input_U, input_V, output, width, height, coefficients);
#endif
}

/// Simulates an incoming CDMA transfer. The N parameter is used to automatically convert 16-bit
/// formats to 8-bit.
template <std::size_t N>
static void ReceiveData(Memory::MemorySystem& memory, u8* output, ConversionBuffer& buf,
                        std::size_t amount_of_data) {
    const u8* input = memory.GetPointer(buf.address);

    std::size_t output_unit = buf.transfer_unit / N;
    ASSERT(amount_of_data % output_unit == 0);

    while (amount_of_data > 0) {
        if constexpr (N == 1) {
            std::memcpy(output, input, output_unit);
        } else {
            for (std::size_t i = 0; i < output_unit; ++i) {
                output[i] = input[i * N];
            }
        }

        output += output_unit;
        input += buf.transfer_unit + buf.gap;

        buf.address += buf.transfer_unit + buf.gap;
        buf.image_size -= buf.transfer_unit;
        amount_of_data -= output_unit;
    }
}

#if defined(__ARM_NEON)
/// 'count' pixeles del formato intermedio al de salida, ocho a ocho con NEON y el
/// resto como SendData. Comprobado en el PC contra Encode* para los cuatro.
template <OutputFormat output_format>
static void EncodePixels(const u32* input, u8* output, int count, u8 alpha) {
    int i = 0;
    if constexpr (output_format == OutputFormat::RGBA8) {
        const uint32x4_t a = vdupq_n_u32(alpha);
        for (; i + 4 <= count; i += 4) {
            vst1q_u8(output + i * 4, vreinterpretq_u8_u32(vorrq_u32(vld1q_u32(input + i), a)));
        }
    } else {
        for (; i + 8 <= count; i += 8) {
            // Carriles 0, B, G, R (ver ConvertEight).
            const uint8x8x4_t px = vld4_u8(reinterpret_cast<const u8*>(input + i));
            if constexpr (output_format == OutputFormat::RGB8) {
                uint8x8x3_t bgr;
                bgr.val[0] = px.val[1];
                bgr.val[1] = px.val[2];
                bgr.val[2] = px.val[3];
                vst3_u8(output + i * 3, bgr);
            } else if constexpr (output_format == OutputFormat::RGB565) {
                uint16x8_t x = vshll_n_u8(px.val[3], 8);
                x = vsriq_n_u16(x, vshll_n_u8(px.val[2], 8), 5);
                x = vsriq_n_u16(x, vshll_n_u8(px.val[1], 8), 11);
                vst1q_u8(output + i * 2, vreinterpretq_u8_u16(x));
            } else {
                uint16x8_t x = vshll_n_u8(px.val[3], 8);
                x = vsriq_n_u16(x, vshll_n_u8(px.val[2], 8), 5);
                x = vsriq_n_u16(x, vshll_n_u8(px.val[1], 8), 10);
                x = vsriq_n_u16(x, vshll_n_u8(vdup_n_u8(alpha), 8), 15);
                vst1q_u8(output + i * 2, vreinterpretq_u8_u16(x));
            }
        }
    }
    for (; i < count; ++i) {
        const u32 color = input[i];
        const Common::Vec4<u8> col_vec{(u8)(color >> 24), (u8)(color >> 16), (u8)(color >> 8),
                                       alpha};
        if constexpr (output_format == OutputFormat::RGBA8) {
            Common::Color::EncodeRGBA8(col_vec, output + i * 4);
        } else if constexpr (output_format == OutputFormat::RGB8) {
            Common::Color::EncodeRGB8(col_vec, output + i * 3);
        } else if constexpr (output_format == OutputFormat::RGB5A1) {
            Common::Color::EncodeRGB5A1(col_vec, output + i * 2);
        } else {
            Common::Color::EncodeRGB565(col_vec, output + i * 2);
        }
    }
}
#endif

/// Convert intermediate RGB32 format to the final output format while simulating an outgoing CDMA
/// transfer.
template <OutputFormat output_format>
static void SendData(Memory::MemorySystem& memory, const u32* input, ConversionBuffer& buf,
                     int amount_of_data, u8 alpha) {

    u8* output = memory.GetPointer(buf.address);
#if defined(__ARM_NEON)
    /**
     * Con unidades de transferencia de pixeles enteros que reparten la franja
     * justa, el bucle de abajo escribe unit_pixels pixeles por unidad y salta
     * el hueco: lo mismo, de ocho en ocho. Si no, como siempre.
     */
    constexpr int bpp = output_format == OutputFormat::RGBA8  ? 4
                        : output_format == OutputFormat::RGB8 ? 3
                                                              : 2;
    const int unit_pixels = buf.transfer_unit / bpp;
    if (buf.transfer_unit % bpp == 0 && unit_pixels > 0 && amount_of_data % unit_pixels == 0) {
        while (amount_of_data > 0) {
            EncodePixels<output_format>(input, output, unit_pixels, alpha);
            input += unit_pixels;
            output += buf.transfer_unit + buf.gap;
            buf.address += buf.transfer_unit + buf.gap;
            buf.image_size -= buf.transfer_unit;
            amount_of_data -= unit_pixels;
        }
        return;
    }
#endif

    while (amount_of_data > 0) {
        u8* unit_end = output + buf.transfer_unit;
        while (output < unit_end) {
            u32 color = *input++;
            Common::Vec4<u8> col_vec{(u8)(color >> 24), (u8)(color >> 16), (u8)(color >> 8), alpha};

            if constexpr (output_format == OutputFormat::RGBA8) {
                Common::Color::EncodeRGBA8(col_vec, output);
                output += 4;
            } else if constexpr (output_format == OutputFormat::RGB8) {
                Common::Color::EncodeRGB8(col_vec, output);
                output += 3;
            } else if constexpr (output_format == OutputFormat::RGB5A1) {
                Common::Color::EncodeRGB5A1(col_vec, output);
                output += 2;
            } else if constexpr (output_format == OutputFormat::RGB565) {
                Common::Color::EncodeRGB565(col_vec, output);
                output += 2;
            } else {
                UNREACHABLE_MSG("Unknown Y2R output format {}", output_format);
            }

            amount_of_data -= 1;
        }

        output += buf.gap;
        buf.address += buf.transfer_unit + buf.gap;
        buf.image_size -= buf.transfer_unit;
    }
}

static const u8 linear_lut[TILE_SIZE] = {
    // clang-format off
     0,  1,  2,  3,  4,  5,  6,  7,
     8,  9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23,
    24, 25, 26, 27, 28, 29, 30, 31,
    32, 33, 34, 35, 36, 37, 38, 39,
    40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55,
    56, 57, 58, 59, 60, 61, 62, 63,
    // clang-format on
};

static const u8 morton_lut[TILE_SIZE] = {
    // clang-format off
     0,  1,  4,  5, 16, 17, 20, 21,
     2,  3,  6,  7, 18, 19, 22, 23,
     8,  9, 12, 13, 24, 25, 28, 29,
    10, 11, 14, 15, 26, 27, 30, 31,
    32, 33, 36, 37, 48, 49, 52, 53,
    34, 35, 38, 39, 50, 51, 54, 55,
    40, 41, 44, 45, 56, 57, 60, 61,
    42, 43, 46, 47, 58, 59, 62, 63,
    // clang-format on
};

static void RotateTile0(const ImageTile& input, ImageTile& output, int height,
                        const u8 out_map[64]) {
    for (int i = 0; i < height * 8; ++i) {
        output[out_map[i]] = input[i];
    }
}

static void RotateTile90(const ImageTile& input, ImageTile& output, int height,
                         const u8 out_map[64]) {
    int out_i = 0;
    for (int x = 0; x < 8; ++x) {
        for (int y = height - 1; y >= 0; --y) {
            output[out_map[out_i++]] = input[y * 8 + x];
        }
    }
}

static void RotateTile180(const ImageTile& input, ImageTile& output, int height,
                          const u8 out_map[64]) {
    int out_i = 0;
    for (int i = height * 8 - 1; i >= 0; --i) {
        output[out_map[out_i++]] = input[i];
    }
}

static void RotateTile270(const ImageTile& input, ImageTile& output, int height,
                          const u8 out_map[64]) {
    int out_i = 0;
    for (int x = 8 - 1; x >= 0; --x) {
        for (int y = 0; y < height; ++y) {
            output[out_map[out_i++]] = input[y * 8 + x];
        }
    }
}

static void WriteTileToOutput(u32* output, const ImageTile& tile, int height, int line_stride) {
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < 8; ++x) {
            output[y * line_stride + x] = tile[y * 8 + x];
        }
    }
}

MICROPROFILE_DEFINE(Y2R_PerformConversion, "Y2R", "PerformConversion", MP_RGB(185, 66, 245));

/**
 * Performs a Y2R colorspace conversion.
 *
 * The Y2R hardware implements hardware-accelerated YUV to RGB colorspace conversions. It is most
 * commonly used for video playback or to display camera input to the screen.
 *
 * The conversion process is quite configurable, and can be divided in distinct steps. From
 * observation, it appears that the hardware buffers a single 8-pixel tall strip of image data
 * internally and converts it in one go before writing to the output and loading the next strip.
 *
 * The steps taken to convert one strip of image data are:
 *
 * - The hardware receives data via CDMA (http://3dbrew.org/wiki/Corelink_DMA_Engines), which is
 *   presumably stored in one or more internal buffers. This process can be done in several separate
 *   transfers, as long as they don't exceed the size of the internal image buffer. This allows
 *   flexibility in input strides.
 * - The input data is decoded into a YUV tuple. Several formats are suported, see the `InputFormat`
 *   enum.
 * - The YUV tuple is converted, using fixed point calculations, to RGB. This step can be configured
 *   using a set of coefficients to support different colorspace standards. See `CoefficientSet`.
 * - The strip can be optionally rotated 90, 180 or 270 degrees. Since each strip is processed
 *   independently, this notably rotates each *strip*, not the entire image. This means that for 90
 *   or 270 degree rotations, the output will be in terms of several 8 x height images, and for any
 *   non-zero rotation the strips will have to be re-arranged so that the parts of the image will
 *   not be shuffled together. This limitation makes this a feature of somewhat dubious utility. 90
 *   or 270 degree rotations in images with non-even height don't seem to work properly.
 * - The data is converted to the output RGB format. See the `OutputFormat` enum.
 * - The data can be output either linearly line-by-line or in the swizzled 8x8 tile format used by
 *   the PICA. This is decided by the `BlockAlignment` enum. If 8x8 alignment is used, then the
 *   image must have a height divisible by 8. The image width must always be divisible by 8.
 * - The final data is then CDMAed out to main memory and the next image strip is processed. This
 *   offers the same flexibility as the input stage.
 *
 * In this implementation, to avoid the combinatorial explosion of parameter combinations, common
 * intermediate formats are used and where possible tables or parameters are used instead of
 * diverging code paths to keep the amount of branches in check. Some steps are also merged to
 * increase efficiency.
 *
 * Output for all valid settings combinations matches hardware, however output in some edge-cases
 * differs:
 *
 * - `Block8x8` alignment with non-mod8 height produces different garbage patterns on the last
 *   strip, especially when combined with rotation.
 * - Hardware, when using `Linear` alignment with a non-even height and 90 or 270 degree rotation
 *   produces misaligned output on the last strip. This implmentation produces output with the
 *   correct "expected" alignment.
 *
 * Hardware behaves strangely (doesn't fire the completion interrupt, for example) in these cases,
 * so they are believed to be invalid configurations anyway.
 */
void PerformConversion(Memory::MemorySystem& memory, ConversionConfiguration cvt) {
    MICROPROFILE_SCOPE(Y2R_PerformConversion);
#ifdef __PSVITA__
    NoteConfig(cvt);
    g_phase_timed = (++g_phase_tick & 3u) == 0;
    g_phase_mark = g_phase_timed ? Common::VitaMicros() : 0;
    const PhaseReport phase_report;
#endif

    ASSERT(cvt.input_line_width % 8 == 0);
    ASSERT(cvt.block_alignment != BlockAlignment::Block8x8 || cvt.input_lines % 8 == 0);
    // Tiles per row
    std::size_t num_tiles = cvt.input_line_width / 8;
    ASSERT(num_tiles <= MAX_TILES);

#if defined(__ARM_NEON)
    /**
     * LA FRANJA FUSIONADA (0.4.0.1, ver ConvertStripTiled): bloques de 8x8, sin
     * giro, y la salida de cada franja en una sola unidad de transferencia (o
     * sin huecos), dentro de memoria seguida. LA RED: las primeras kFusedChecks
     * imagenes se hacen por los dos caminos -- la fusionada en un bufer aparte
     * y la de siempre en la memoria del juego -- y se comparan byte a byte. Una
     * diferencia la apaga para toda la sesion y dice donde en crash.txt.
     */
    static constexpr u32 kFusedChecks = 6;
    static u32 fused_checked = 0;
    static bool fused_off = false;
    u8* fused_out = nullptr;
    std::vector<u8> fused_scratch;
    u8* fused_guest = nullptr;
    std::size_t fused_stride = 0;
    std::size_t fused_strip_bytes = 0;
    {
        const u32 bpp = cvt.output_format == OutputFormat::RGBA8  ? 4
                        : cvt.output_format == OutputFormat::RGB8 ? 3
                                                                  : 2;
        const std::size_t strip_bytes = std::size_t{cvt.input_line_width} * 8 * bpp;
        const bool unit_ok = cvt.dst.gap == 0 || cvt.dst.transfer_unit == strip_bytes;
        const std::size_t stride = cvt.dst.gap == 0 ? strip_bytes : strip_bytes + cvt.dst.gap;
        const std::size_t strips = cvt.input_lines / 8;
        const std::size_t span = strips == 0 ? 0 : (strips - 1) * stride + strip_bytes;
        if (!fused_off && cvt.block_alignment == BlockAlignment::Block8x8 &&
            cvt.rotation == Rotation::None && cvt.input_lines % 8 == 0 && strips != 0 &&
            unit_ok && cvt.input_format <= InputFormat::YUYV422_Interleaved &&
            cvt.output_format <= OutputFormat::RGB565) {
            u8* const first = memory.GetPointer(cvt.dst.address);
            u8* const last = memory.GetPointer(cvt.dst.address + static_cast<u32>(span) - 1);
            if (first != nullptr && last == first + span - 1) {
                fused_guest = first;
                fused_stride = stride;
                fused_strip_bytes = strip_bytes;
                if (fused_checked < kFusedChecks) {
                    fused_scratch.assign(span, 0);
                    fused_out = fused_scratch.data();
                } else {
                    fused_out = first;
                }
            }
        }
    }
    const bool fused_only = fused_out != nullptr && fused_out == fused_guest;
    g_skip_convert = fused_only;
    struct SkipReset {
        ~SkipReset() {
            g_skip_convert = false;
        }
    } const skip_reset;
#endif

    // Buffer used as a CDMA source/target.
    std::unique_ptr<u8[]> data_buffer(new u8[cvt.input_line_width * 8 * 4]);
    // Intermediate storage for decoded 8x8 image tiles. Always stored as RGB32.
    std::unique_ptr<ImageTile[]> tiles(new ImageTile[num_tiles]);
    ImageTile tmp_tile;

    // LUT used to remap writes to a tile. Used to allow linear or swizzled output without
    // requiring two different code paths.
    const u8* tile_remap = nullptr;
    switch (cvt.block_alignment) {
    case BlockAlignment::Linear:
        tile_remap = linear_lut;
        break;
    case BlockAlignment::Block8x8:
        tile_remap = morton_lut;
        break;
    }

    for (unsigned int y = 0; y < cvt.input_lines; y += 8) {
        unsigned int row_height = std::min(cvt.input_lines - y, 8u);

        // Total size in pixels of incoming data required for this strip.
        const std::size_t row_data_size = row_height * cvt.input_line_width;

#ifdef __PSVITA__
        Phase(3); // lo de antes de la franja cuenta como enviar (la anterior)
#endif
        u8* input_Y = data_buffer.get();
        u8* input_U = input_Y + 8 * cvt.input_line_width;
        u8* input_V = input_U + 8 * cvt.input_line_width / 2;

        switch (cvt.input_format) {
        case InputFormat::YUV422_Indiv8:
            ReceiveData<1>(memory, input_Y, cvt.src_Y, row_data_size);
            ReceiveData<1>(memory, input_U, cvt.src_U, row_data_size / 2);
            ReceiveData<1>(memory, input_V, cvt.src_V, row_data_size / 2);
            ConvertYUV<InputFormat::YUV422_Indiv8>(input_Y, input_U, input_V, tiles.get(),
                                                   cvt.input_line_width, row_height,
                                                   cvt.coefficients);
            break;
        case InputFormat::YUV420_Indiv8:
            ReceiveData<1>(memory, input_Y, cvt.src_Y, row_data_size);
            ReceiveData<1>(memory, input_U, cvt.src_U, row_data_size / 4);
            ReceiveData<1>(memory, input_V, cvt.src_V, row_data_size / 4);
            ConvertYUV<InputFormat::YUV420_Indiv8>(input_Y, input_U, input_V, tiles.get(),
                                                   cvt.input_line_width, row_height,
                                                   cvt.coefficients);
            break;
        case InputFormat::YUV422_Indiv16:
            ReceiveData<2>(memory, input_Y, cvt.src_Y, row_data_size);
            ReceiveData<2>(memory, input_U, cvt.src_U, row_data_size / 2);
            ReceiveData<2>(memory, input_V, cvt.src_V, row_data_size / 2);
            ConvertYUV<InputFormat::YUV422_Indiv16>(input_Y, input_U, input_V, tiles.get(),
                                                    cvt.input_line_width, row_height,
                                                    cvt.coefficients);
            break;
        case InputFormat::YUV420_Indiv16:
            ReceiveData<2>(memory, input_Y, cvt.src_Y, row_data_size);
            ReceiveData<2>(memory, input_U, cvt.src_U, row_data_size / 4);
            ReceiveData<2>(memory, input_V, cvt.src_V, row_data_size / 4);
            ConvertYUV<InputFormat::YUV420_Indiv16>(input_Y, input_U, input_V, tiles.get(),
                                                    cvt.input_line_width, row_height,
                                                    cvt.coefficients);
            break;
        case InputFormat::YUYV422_Interleaved:
            input_U = nullptr;
            input_V = nullptr;
            ReceiveData<1>(memory, input_Y, cvt.src_YUYV, row_data_size * 2);
            ConvertYUV<InputFormat::YUYV422_Interleaved>(input_Y, input_U, input_V, tiles.get(),
                                                         cvt.input_line_width, row_height,
                                                         cvt.coefficients);
            break;
        default:
            UNREACHABLE_MSG("Unknown Y2R input format {}", cvt.input_format);
            return;
        }

#if defined(__ARM_NEON)
        if (fused_out != nullptr) {
            u8* const strip_out = fused_out + (y / 8) * fused_stride;
            switch (cvt.input_format) {
            case InputFormat::YUV422_Indiv8:
                FusedStrip<InputFormat::YUV422_Indiv8>(input_Y, input_U, input_V, cvt,
                                                       strip_out);
                break;
            case InputFormat::YUV420_Indiv8:
                FusedStrip<InputFormat::YUV420_Indiv8>(input_Y, input_U, input_V, cvt,
                                                       strip_out);
                break;
            case InputFormat::YUV422_Indiv16:
                FusedStrip<InputFormat::YUV422_Indiv16>(input_Y, input_U, input_V, cvt,
                                                        strip_out);
                break;
            case InputFormat::YUV420_Indiv16:
                FusedStrip<InputFormat::YUV420_Indiv16>(input_Y, input_U, input_V, cvt,
                                                        strip_out);
                break;
            case InputFormat::YUYV422_Interleaved:
                FusedStrip<InputFormat::YUYV422_Interleaved>(input_Y, nullptr, nullptr, cvt,
                                                             strip_out);
                break;
            }
            if (fused_only) {
#ifdef __PSVITA__
                Phase(2);
#endif
                continue;
            }
        }
#endif
        u32* output_buffer = reinterpret_cast<u32*>(data_buffer.get());

        // Sin rotacion y en lineal, RotateTile0 con linear_lut es una copia
        // tal cual: la fila del mosaico va directa a la salida.
        const bool direct =
            cvt.rotation == Rotation::None && cvt.block_alignment == BlockAlignment::Linear;
        for (std::size_t i = 0; direct && i < num_tiles; ++i) {
            WriteTileToOutput(output_buffer, tiles[i], row_height, cvt.input_line_width);
            output_buffer += 8;
        }
        for (std::size_t i = 0; !direct && i < num_tiles; ++i) {
            int image_strip_width = 0;
            int output_stride = 0;

            switch (cvt.rotation) {
            case Rotation::None:
                RotateTile0(tiles[i], tmp_tile, row_height, tile_remap);
                image_strip_width = cvt.input_line_width;
                output_stride = 8;
                break;
            case Rotation::Clockwise_90:
                RotateTile90(tiles[i], tmp_tile, row_height, tile_remap);
                image_strip_width = 8;
                output_stride = 8 * row_height;
                break;
            case Rotation::Clockwise_180:
                // For 180 and 270 degree rotations we also invert the order of tiles in the strip,
                // since the rotates are done individually on each tile.
                RotateTile180(tiles[num_tiles - i - 1], tmp_tile, row_height, tile_remap);
                image_strip_width = cvt.input_line_width;
                output_stride = 8;
                break;
            case Rotation::Clockwise_270:
                RotateTile270(tiles[num_tiles - i - 1], tmp_tile, row_height, tile_remap);
                image_strip_width = 8;
                output_stride = 8 * row_height;
                break;
            }

            switch (cvt.block_alignment) {
            case BlockAlignment::Linear:
                WriteTileToOutput(output_buffer, tmp_tile, row_height, image_strip_width);
                output_buffer += output_stride;
                break;
            case BlockAlignment::Block8x8:
                WriteTileToOutput(output_buffer, tmp_tile, 8, 8);
                output_buffer += TILE_SIZE;
                break;
            }
        }

#ifdef __PSVITA__
        Phase(2);
#endif
        switch (cvt.output_format) {
        case OutputFormat::RGBA8:
            SendData<OutputFormat::RGBA8>(memory, reinterpret_cast<u32*>(data_buffer.get()),
                                          cvt.dst, static_cast<int>(row_data_size),
                                          static_cast<u8>(cvt.alpha));
            break;
        case OutputFormat::RGB8:
            SendData<OutputFormat::RGB8>(memory, reinterpret_cast<u32*>(data_buffer.get()), cvt.dst,
                                         static_cast<int>(row_data_size),
                                         static_cast<u8>(cvt.alpha));
            break;
        case OutputFormat::RGB5A1:
            SendData<OutputFormat::RGB5A1>(memory, reinterpret_cast<u32*>(data_buffer.get()),
                                           cvt.dst, static_cast<int>(row_data_size),
                                           static_cast<u8>(cvt.alpha));
            break;
        case OutputFormat::RGB565:
            SendData<OutputFormat::RGB565>(memory, reinterpret_cast<u32*>(data_buffer.get()),
                                           cvt.dst, static_cast<int>(row_data_size),
                                           static_cast<u8>(cvt.alpha));
            break;
        default:
            UNREACHABLE_MSG("Unknown Y2R output format {}", cvt.output_format);
            return;
        }
    }
#if defined(__ARM_NEON)
    if (fused_out != nullptr && !fused_only) {
        const std::size_t strips = cvt.input_lines / 8;
        for (std::size_t s = 0; s < strips && !fused_off; s++) {
            const u8* const want = fused_guest + s * fused_stride;
            const u8* const got = fused_out + s * fused_stride;
            if (std::memcmp(want, got, fused_strip_bytes) == 0) {
                continue;
            }
            std::size_t at = 0;
            while (at < fused_strip_bytes && want[at] == got[at]) {
                at++;
            }
            fused_off = true;
#ifdef __PSVITA__
            Common::VitaNote(
                "y2r", fmt::format("franja fusionada DISTINTA (se apaga): {}x{} entrada {} "
                                   "salida {}, franja {} byte {}: {:02x} en vez de {:02x}",
                                   cvt.input_line_width, cvt.input_lines,
                                   static_cast<u32>(cvt.input_format),
                                   static_cast<u32>(cvt.output_format), s, at, got[at],
                                   want[at])
                           .c_str());
#endif
        }
        if (!fused_off && ++fused_checked == kFusedChecks) {
#ifdef __PSVITA__
            Common::VitaNote("y2r", fmt::format("franja fusionada comprobada en {} imagenes: "
                                                "a partir de ahora va sola",
                                                kFusedChecks)
                                        .c_str());
#endif
        }
    }
#endif
}

#if defined(__ARM_NEON) && defined(__PSVITA__)
/**
 * EL BANCO DE Y2R (0.4.1.4). En la intro de Kirby Triple Deluxe y en el video
 * de Smash, "colocar" (traer la franja y convertirla ya en su sitio) cuesta
 * 7,5-8 ms por imagen de 400x240 YUV420 a RGB8: ~35 ciclos por pixel, cuando
 * las cuentas NEON son unos pocos. Esto lo mide aislado en la consola, con la
 * misma forma que esos dos juegos (origen con unidad 400 y hueco 112, destino
 * en bloques de 8x8 con unidad 9600 y hueco 2688): la imagen entera en frio y
 * en caliente, y sus partes. Se pide con ux0:/data/azahar/banco_mem/y2r.txt
 * (se borra al empezar) y las lineas van a banco_mem/resultados.txt.
 */
void RunBenchIfAny() {
    constexpr const char* kFlag = "ux0:/data/azahar/banco_mem/y2r.txt";
    SceIoStat flag{};
    if (sceIoGetstat(kFlag, &flag) < 0) {
        return;
    }
    sceIoRemove(kFlag);
    std::FILE* out = std::fopen("ux0:/data/azahar/banco_mem/resultados.txt", "a");
    if (out == nullptr) {
        return;
    }
    constexpr unsigned kW = 400;
    constexpr unsigned kH = 240;
    constexpr std::size_t kYStride = 512;    // unidad 400 + hueco 112
    constexpr std::size_t kCStride = 256;    // la croma, con su hueco
    constexpr std::size_t kDstStride = 12288; // unidad 9600 + hueco 2688 por franja
    constexpr std::size_t kSet = 1024 * 1024; // un juego de origen y destino
    constexpr std::size_t kSets = 12;         // 12 MB: no cabe en la cache
    std::unique_ptr<u8[]> pool(new (std::nothrow) u8[kSet * kSets]);
    std::unique_ptr<u8[]> strip(new (std::nothrow) u8[kW * 8 * 4]);
    if (pool == nullptr || strip == nullptr) {
        std::fprintf(out, "y2r banco: sin memoria\n");
        std::fclose(out);
        return;
    }
    u32 seed = 0x12345678u;
    for (std::size_t i = 0; i < kSet * kSets; i++) {
        seed = seed * 1664525u + 1013904223u;
        pool[i] = static_cast<u8>(seed >> 24);
    }
    ConversionConfiguration cvt{};
    cvt.input_format = InputFormat::YUV420_Indiv8;
    cvt.output_format = OutputFormat::RGB8;
    cvt.input_line_width = kW;
    cvt.input_lines = kH;
    cvt.alpha = 0xFF;
    cvt.coefficients = {{0x100, 0x166, 0xB6, 0x58, 0x1C5, -0x166F, 0x10EE, -0x1C5B}};
    const auto set_at = [&](std::size_t k) { return pool.get() + (k % kSets) * kSet; };
    // Origen Y 0..122880, U desde 128 KB, V desde 192 KB, destino desde 256 KB.
    const auto receive = [&](const u8* base, unsigned s) {
        u8* const in_y = strip.get();
        u8* const in_u = in_y + 8 * kW;
        u8* const in_v = in_u + 8 * kW / 2;
        for (unsigned r = 0; r < 8; r++) {
            std::memcpy(in_y + r * kW, base + (s * 8 + r) * kYStride, kW);
        }
        for (unsigned r = 0; r < 4; r++) {
            std::memcpy(in_u + r * (kW / 2), base + 128 * 1024 + (s * 4 + r) * kCStride, kW / 2);
            std::memcpy(in_v + r * (kW / 2), base + 192 * 1024 + (s * 4 + r) * kCStride, kW / 2);
        }
    };
    const auto convert = [&](u8* dst, unsigned s) {
        u8* const in_y = strip.get();
        FusedStrip<InputFormat::YUV420_Indiv8>(in_y, in_y + 8 * kW, in_y + 8 * kW + 4 * kW, cvt,
                                               dst + s * kDstStride);
    };
    const auto image = [&](std::size_t k, bool do_receive, bool do_convert) {
        u8* const base = set_at(k);
        for (unsigned s = 0; s < kH / 8; s++) {
            if (do_receive) {
                receive(base, s);
            }
            if (do_convert) {
                convert(base + 256 * 1024, s);
            }
        }
    };
    const auto time = [&](bool cold, bool do_receive, bool do_convert) {
        constexpr unsigned kRuns = 24;
        image(0, do_receive, do_convert); // calentar codigo
        const unsigned long long begin = Common::VitaMicros();
        for (unsigned run = 0; run < kRuns; run++) {
            image(cold ? run + 1 : 0, do_receive, do_convert);
        }
        return static_cast<double>(Common::VitaMicros() - begin) / kRuns;
    };
    for (const OutputFormat format : {OutputFormat::RGB8, OutputFormat::RGBA8}) {
        cvt.output_format = format;
        const char* name = format == OutputFormat::RGB8 ? "RGB8" : "RGBA8";
        const double full_cold = time(true, true, true);
        const double full_warm = time(false, true, true);
        const double conv_warm = time(false, false, true);
        const double conv_cold = time(true, false, true);
        const double recv_cold = time(true, true, false);
        std::fprintf(out,
                     "y2r 400x240 YUV420 a %s: imagen entera fria %.0f us, caliente %.0f us | "
                     "solo convertir caliente %.0f us, con destino frio %.0f us | solo traer "
                     "frio %.0f us\n",
                     name, full_cold, full_warm, conv_warm, conv_cold, recv_cold);
    }
    std::fprintf(out, "== fin y2r\n");
    std::fclose(out);
}
#else
void RunBenchIfAny() {}
#endif
} // namespace HW::Y2R
