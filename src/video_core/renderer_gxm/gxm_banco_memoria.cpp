// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

/**
 * EL BANCO DE MEMORIA (0.4.0.9). Solo corre si existe
 * ux0:/data/azahar/banco_mem/hacer.txt: mide, en la consola, lo que cuesta leer
 * memoria fria con y sin 'pld', comparar dos buffers, una huella NEON y copiar
 * a memoria sin cache, escribe banco_mem/resultados.txt y cierra la aplicacion.
 *
 * POR QUE. La comparacion de la cache de subidas de vertices se lleva ~30-60 us
 * por lote (lee el buffer del invitado y su copia, los dos frios), y para
 * decidir entre comparar, una huella o nada hacen falta cifras de esta consola,
 * no suposiciones: con los indices el 'pld' fue un 4,6x y con la copia al
 * anillo no hizo nada.
 */

#include <arm_neon.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <fmt/format.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include "common/common_types.h"
#include "common/hash.h"
#include "common/vita_diag.h"
#include "video_core/renderer_gxm/gxm_cg.h"
#include "video_core/renderer_gxm/gxm_memory.h"

namespace Gxm {
namespace {

constexpr std::size_t kPoolBytes = std::size_t{32} << 20;
constexpr std::size_t kStride = std::size_t{128} << 10;
constexpr u32 kChunks = static_cast<u32>(kPoolBytes / kStride);

volatile u64 g_sink = 0;

unsigned long long Now() {
    return sceKernelGetProcessTimeWide();
}

template <u32 Ahead>
u64 ReadSum(const u8* p, u32 n) {
    uint32x4_t acc = vdupq_n_u32(0);
    for (u32 i = 0; i + 64 <= n; i += 64) {
        if constexpr (Ahead != 0) {
            __builtin_prefetch(p + i + Ahead);
            __builtin_prefetch(p + i + Ahead + 32);
        }
        acc = vaddq_u32(acc, vld1q_u32(reinterpret_cast<const u32*>(p + i)));
        acc = vaddq_u32(acc, vld1q_u32(reinterpret_cast<const u32*>(p + i + 16)));
        acc = vaddq_u32(acc, vld1q_u32(reinterpret_cast<const u32*>(p + i + 32)));
        acc = vaddq_u32(acc, vld1q_u32(reinterpret_cast<const u32*>(p + i + 48)));
    }
    return static_cast<u64>(vgetq_lane_u32(acc, 0)) + vgetq_lane_u32(acc, 1) +
           vgetq_lane_u32(acc, 2) + vgetq_lane_u32(acc, 3);
}

template <u32 Ahead>
bool Same(const u8* a, const u8* b, u32 n) {
    uint8x16_t acc = vdupq_n_u8(0);
    for (u32 i = 0; i + 64 <= n; i += 64) {
        if constexpr (Ahead != 0) {
            __builtin_prefetch(a + i + Ahead);
            __builtin_prefetch(a + i + Ahead + 32);
            __builtin_prefetch(b + i + Ahead);
            __builtin_prefetch(b + i + Ahead + 32);
        }
        const uint8x16_t x0 = veorq_u8(vld1q_u8(a + i), vld1q_u8(b + i));
        const uint8x16_t x1 = veorq_u8(vld1q_u8(a + i + 16), vld1q_u8(b + i + 16));
        const uint8x16_t x2 = veorq_u8(vld1q_u8(a + i + 32), vld1q_u8(b + i + 32));
        const uint8x16_t x3 = veorq_u8(vld1q_u8(a + i + 48), vld1q_u8(b + i + 48));
        acc = vorrq_u8(acc, vorrq_u8(vorrq_u8(x0, x1), vorrq_u8(x2, x3)));
    }
    const uint64x2_t wide = vreinterpretq_u64_u8(acc);
    return (vgetq_lane_u64(wide, 0) | vgetq_lane_u64(wide, 1)) == 0;
}

/**
 * Huella al estilo de XXH3: por cada 16 bytes, el producto de 32x32 bits de
 * sus dos mitades (con una clave) y los propios datos con las mitades
 * cambiadas, en acumuladores de 64 bits. Un cambio en cualquier palabra mueve
 * el producto y la suma. Al final, mezcla de xxh64.
 */
template <u32 Ahead>
u64 NeonHash(const u8* p, u32 n) {
    const uint32x4_t key0 = {0x9E3779B1u, 0x85EBCA77u, 0xC2B2AE3Du, 0x27D4EB2Fu};
    const uint32x4_t key1 = {0x165667B1u, 0xD3A2646Cu, 0xFD7046C5u, 0xB55A4F09u};
    uint64x2_t acc0 = vdupq_n_u64(0x9E3779B97F4A7C15ull);
    uint64x2_t acc1 = vdupq_n_u64(0xC2B2AE3D27D4EB4Full);
    u32 i = 0;
    for (; i + 32 <= n; i += 32) {
        if constexpr (Ahead != 0) {
            __builtin_prefetch(p + i + Ahead);
        }
        const uint32x4_t d0 = vld1q_u32(reinterpret_cast<const u32*>(p + i));
        const uint32x4_t d1 = vld1q_u32(reinterpret_cast<const u32*>(p + i + 16));
        const uint32x4_t k0 = veorq_u32(d0, key0);
        const uint32x4_t k1 = veorq_u32(d1, key1);
        const uint32x2x2_t u0 = vuzp_u32(vget_low_u32(k0), vget_high_u32(k0));
        const uint32x2x2_t u1 = vuzp_u32(vget_low_u32(k1), vget_high_u32(k1));
        acc0 = vaddq_u64(acc0, vmull_u32(u0.val[0], u0.val[1]));
        acc1 = vaddq_u64(acc1, vmull_u32(u1.val[0], u1.val[1]));
        const uint64x2_t s0 = vreinterpretq_u64_u32(d0);
        const uint64x2_t s1 = vreinterpretq_u64_u32(d1);
        acc0 = vaddq_u64(acc0, vextq_u64(s0, s0, 1));
        acc1 = vaddq_u64(acc1, vextq_u64(s1, s1, 1));
    }
    u64 h = vgetq_lane_u64(acc0, 0) ^ (vgetq_lane_u64(acc0, 1) * 0x9E3779B97F4A7C15ull) ^
            (vgetq_lane_u64(acc1, 0) * 0xC2B2AE3D27D4EB4Full) ^
            (vgetq_lane_u64(acc1, 1) * 0x165667B19E3779F9ull) ^ n;
    for (; i < n; i++) {
        h = (h ^ p[i]) * 0x100000001B3ull;
    }
    h ^= h >> 33;
    h *= 0xC2B2AE3D27D4EB4Full;
    h ^= h >> 29;
    h *= 0x165667B19E3779F9ull;
    h ^= h >> 32;
    return h;
}

struct Result {
    double us_per_chunk = 0.0;
    double mb_per_s = 0.0;
};

/// Recorre los kChunks trozos de la reserva (frios) y da el mejor de dos pasadas.
template <typename F>
Result SweepCold(const u8* pool, u32 bytes, F&& f) {
    Result best{1e18, 0.0};
    for (u32 pass = 0; pass < 2; pass++) {
        const unsigned long long begin = Now();
        for (u32 c = 0; c < kChunks; c++) {
            f(pool + std::size_t{c} * kStride, bytes);
        }
        const double us = static_cast<double>(Now() - begin) / kChunks;
        if (us < best.us_per_chunk) {
            best.us_per_chunk = us;
            best.mb_per_s = bytes / us;
        }
    }
    return best;
}

/// El mismo trozo una y otra vez (caliente): lo que cuesta el calculo.
template <typename F>
Result RepeatWarm(const u8* chunk, u32 bytes, F&& f) {
    constexpr u32 kTimes = 256;
    f(chunk, bytes);
    const unsigned long long begin = Now();
    for (u32 t = 0; t < kTimes; t++) {
        f(chunk, bytes);
    }
    const double us = static_cast<double>(Now() - begin) / kTimes;
    return {us, bytes / us};
}

std::string Line(const char* name, u32 bytes, const Result& r) {
    return fmt::format("{}: {} bytes en {:.1f} us ({:.0f} MB/s)", name, bytes, r.us_per_chunk,
                       r.mb_per_s);
}

} // Anonymous namespace

void RunMemBenchIfAny() {
    static constexpr char kDir[] = "ux0:/data/azahar/banco_mem/";
    const std::string order_path = std::string(kDir) + "hacer.txt";
    std::FILE* order = std::fopen(order_path.c_str(), "r");
    if (order == nullptr) {
        return;
    }
    std::fclose(order);
    std::remove(order_path.c_str());
    std::FILE* out = std::fopen((std::string(kDir) + "resultados.txt").c_str(), "a");
    const auto note = [out](const std::string& text) {
        Common::VitaNote("banco mem", text.c_str());
        if (out != nullptr) {
            std::fprintf(out, "%s\n", text.c_str());
            std::fflush(out);
        }
    };
    sceKernelChangeThreadPriority(sceKernelGetThreadId(), 64);

    std::unique_ptr<u8[]> pool_a(new (std::nothrow) u8[kPoolBytes]);
    std::unique_ptr<u8[]> pool_b(new (std::nothrow) u8[kPoolBytes]);
    if (!pool_a || !pool_b) {
        note("sin memoria para el banco");
    } else {
        u32 seed = 0x12345678u;
        for (std::size_t i = 0; i < kPoolBytes; i += 4) {
            seed = seed * 1664525u + 1013904223u;
            std::memcpy(pool_a.get() + i, &seed, 4);
        }
        std::memcpy(pool_b.get(), pool_a.get(), kPoolBytes);
        {
            const unsigned long long begin = Now();
            for (u32 i = 0; i < 1000; i++) {
                g_sink = g_sink + Now();
            }
            note(fmt::format("reloj: {:.2f} us por lectura", (Now() - begin) / 1000.0));
        }
        for (const u32 bytes : {4096u, 12288u, 65536u}) {
            const u8* a = pool_a.get();
            const u8* b = pool_b.get();
            note(Line("leer frio sin pld", bytes, SweepCold(a, bytes, [](const u8* p, u32 n) {
                          g_sink = g_sink + ReadSum<0>(p, n);
                      })));
            note(Line("leer frio pld 128", bytes, SweepCold(a, bytes, [](const u8* p, u32 n) {
                          g_sink = g_sink + ReadSum<128>(p, n);
                      })));
            note(Line("leer frio pld 256", bytes, SweepCold(a, bytes, [](const u8* p, u32 n) {
                          g_sink = g_sink + ReadSum<256>(p, n);
                      })));
            note(Line("leer frio pld 512", bytes, SweepCold(a, bytes, [](const u8* p, u32 n) {
                          g_sink = g_sink + ReadSum<512>(p, n);
                      })));
            note(Line("leer frio pld 1024", bytes, SweepCold(a, bytes, [](const u8* p, u32 n) {
                          g_sink = g_sink + ReadSum<1024>(p, n);
                      })));
            const std::ptrdiff_t delta = b - a;
            note(Line("comparar frio sin pld", bytes,
                      SweepCold(a, bytes, [delta](const u8* p, u32 n) {
                          g_sink = g_sink + Same<0>(p, p + delta, n);
                      })));
            note(Line("comparar frio pld 512", bytes,
                      SweepCold(a, bytes, [delta](const u8* p, u32 n) {
                          g_sink = g_sink + Same<512>(p, p + delta, n);
                      })));
            note(Line("comparar frio pld 256", bytes,
                      SweepCold(a, bytes, [delta](const u8* p, u32 n) {
                          g_sink = g_sink + Same<256>(p, p + delta, n);
                      })));
            note(Line("huella neon frio pld 256", bytes,
                      SweepCold(a, bytes, [](const u8* p, u32 n) {
                          g_sink = g_sink + NeonHash<256>(p, n);
                      })));
            note(Line("huella neon frio pld 512", bytes,
                      SweepCold(a, bytes, [](const u8* p, u32 n) {
                          g_sink = g_sink + NeonHash<512>(p, n);
                      })));
            note(Line("huella neon caliente", bytes, RepeatWarm(a, bytes, [](const u8* p, u32 n) {
                          g_sink = g_sink + NeonHash<0>(p, n);
                      })));
            note(Line("cityhash64 frio", bytes, SweepCold(a, bytes, [](const u8* p, u32 n) {
                          g_sink = g_sink + Common::ComputeHash64(p, n);
                      })));
        }
        // La copia de siempre al anillo (memoria sin cache de la GPU).
        Allocation ring = Allocate(Pool::Host, 1u << 20);
        if (ring.Valid()) {
            u8* dst = static_cast<u8*>(ring.Data());
            for (const u32 bytes : {4096u, 12288u, 65536u}) {
                note(Line("copiar frio a sin cache (memcpy)", bytes,
                          SweepCold(pool_a.get(), bytes, [dst](const u8* p, u32 n) {
                              std::memcpy(dst, p, n);
                          })));
            }
        } else {
            note("sin memoria de la GPU para la prueba de copia");
        }
    }
    if (out != nullptr) {
        std::fprintf(out, "== fin\n");
        std::fclose(out);
    }
    sceKernelDelayThread(1000 * 1000);
    sceKernelExitProcess(0);
}

} // namespace Gxm
