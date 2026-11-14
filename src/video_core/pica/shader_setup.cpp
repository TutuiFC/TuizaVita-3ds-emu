// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <utility>
#include "common/assert.h"
#include "common/bit_set.h"
#include "common/hash.h"
#include "common/logging/log.h"
#include "video_core/pica/regs_shader.h"
#include "video_core/pica/shader_setup.h"

#if defined(CITRA_HAS_SSE42)
#include <nmmintrin.h>
#endif

#if defined(__aarch64__) || defined(__ARM_NEON)
#define CITRA_HAS_NEON
#include <arm_neon.h>

#if !defined(__aarch64__)
// vmaxvq_u32 (reduccion horizontal) solo existe en AArch64. En el NEON de
// ARMv7 del Cortex-A9 de la Vita se consigue lo mismo con dos maximos por
// parejas.
static inline u32 vmaxvq_u32(uint32x4_t v) {
    uint32x2_t m = vpmax_u32(vget_low_u32(v), vget_high_u32(v));
    m = vpmax_u32(m, m);
    return vget_lane_u32(m, 0);
}
#endif
#endif

#if defined(_MSC_VER)
#define DISABLE_VECTORIZE __pragma(loop(no_vector))
#elif defined(__clang__)
#define DISABLE_VECTORIZE _Pragma("clang loop vectorize(disable)")
#elif defined(__GNUC__) && (__GNUC__ >= 14)
#define DISABLE_VECTORIZE _Pragma("GCC novector")
#else
#define DISABLE_VECTORIZE
#endif

namespace Pica {

ShaderSetup::ShaderSetup() = default;

ShaderSetup::~ShaderSetup() = default;

void ShaderSetup::WriteUniformBoolReg(u32 value) {
    const auto bits = BitSet32(value);
    for (u32 i = 0; i < uniforms.b.size(); ++i) {
        const bool prev = std::exchange(uniforms.b[i], bits[i]);
        uniforms_dirty |= prev != bits[i];
    }
}

void ShaderSetup::WriteUniformIntReg(u32 index, const Common::Vec4<u8> values) {
    ASSERT(index < uniforms.i.size());
    const auto prev = std::exchange(uniforms.i[index], values);
    uniforms_dirty |= prev != values;
}

std::optional<u32> ShaderSetup::WriteUniformFloatReg(ShaderRegs& config, u32 value) {
    auto& uniform_setup = config.uniform_setup;
    const bool is_float32 = uniform_setup.IsFloat32();
    if (!uniform_queue.Push(value, is_float32)) {
        return std::nullopt;
    }

    const auto uniform = uniform_queue.Get(is_float32);
    if (uniform_setup.index >= uniforms.f.size()) {
        // Some games may submit OOB indexes (mainly 0x7F), which is
        // ignored on real HW.
        return std::nullopt;
    }

    const u32 index = uniform_setup.index.Value();
    const auto prev = std::exchange(uniforms.f[index], uniform);
    uniforms_dirty |= prev != uniform;
    uniform_raw_mode[index] = 0;
    uniform_setup.index.Assign(index + 1);
    return index;
}

/**
 * De f24 a los bits de un float, solo con enteros (0.4.6.2): f24::FromRaw acaba en un
 * registro de VFP y de ahi a memoria por cada componente. Lo mismo que FromRaw para los
 * normales y el cero; los raros (denormales, infinitos) van por el.
 */
static inline u32 F24ToF32Bits(u32 hex) {
    const u32 sign = (hex >> 23) << 31;
    if ((hex & 0x7FFFFF) == 0) {
        return sign;
    }
    const u32 exponent = (hex >> 16) & 0x7F;
    if (exponent - 1 < 0x7E) [[likely]] {
        return sign | ((exponent + 64) << 23) | ((hex & 0xFFFF) << 7);
    }
    return std::bit_cast<u32>(f24::FromRaw(hex).ToFloat32());
}

std::optional<ShaderSetup::UniformWriteRange> ShaderSetup::WriteUniformFloatRegRange(
    ShaderRegs& config, const u32* values, u32 count) {

    if (count == 0) [[unlikely]] {
        return std::nullopt;
    }

    auto& uniform_setup = config.uniform_setup;
    const bool is_float32 = uniform_setup.IsFloat32();

    std::optional<u32> first_index;
    u32 written = 0;
    u32 i = 0;

    /**
     * LOS UNIFORMES QUE NO CAMBIAN, SIN CONVERTIR (0.4.5.4). Cada uno pasaba por la cola,
     * cuatro conversiones de f24 a float con sus ramas y una comparacion de coma flotante
     * por componente, y luego se copiaba al de geometria; y los juegos reenvian casi los
     * mismos antes de cada lote. Comparar lo que llega con lo que llego la otra vez son
     * tres o cuatro enteros.
     *
     * LA COLA NUNCA ESTA VACIA AQUI (0.4.6.2). La primera palabra de una orden va antes de
     * su cabecera, asi que entra sola por WriteUniformFloatReg y esto llega con ella en la
     * cola: con "solo si la cola esta vacia" el atajo casi nunca se usaba. Ahora se acaba
     * primero ese uniforme con las palabras que faltan.
     */
    if (raw_uniforms) {
        const u32 words = is_float32 ? 4 : 3;
        const u8 mode = is_float32 ? 2 : 1;
        const u32 first = uniform_setup.index.Value();
        u32 index = first;
        const auto store = [&](const u32* in) {
            auto& raw = uniform_raw[index];
            if (uniform_raw_mode[index] == mode && raw[0] == in[0] && raw[1] == in[1] &&
                raw[2] == in[2] && (!is_float32 || raw[3] == in[3])) {
                return;
            }
            uniform_raw_mode[index] = mode;
            raw[0] = in[0];
            raw[1] = in[1];
            raw[2] = in[2];
            u32 bits[4];
            if (is_float32) {
                raw[3] = in[3];
                for (u32 word = 0; word < 4; word++) {
                    uniform_queue.buffer[word] = in[word];
                }
                const auto uniform = uniform_queue.Get(true);
                std::memcpy(bits, &uniform, sizeof(bits));
            } else {
                bits[0] = F24ToF32Bits(in[2] & 0xFFFFFF);
                bits[1] = F24ToF32Bits(((in[1] & 0xFFFF) << 8) | (in[2] >> 24));
                bits[2] = F24ToF32Bits(((in[0] & 0xFF) << 16) | (in[1] >> 16));
                bits[3] = F24ToF32Bits(in[0] >> 8);
            }
            auto* out = reinterpret_cast<u32*>(&uniforms.f[index]);
            if (out[0] != bits[0] || out[1] != bits[1] || out[2] != bits[2] || out[3] != bits[3]) {
                out[0] = bits[0];
                out[1] = bits[1];
                out[2] = bits[2];
                out[3] = bits[3];
                uniforms_dirty = true;
            }
        };
        if (uniform_queue.index != 0 && uniform_queue.index < words && index < uniforms.f.size()) {
            u32 queued = uniform_queue.index;
            while (queued < words && i < count) {
                uniform_queue.buffer[queued++] = values[i++];
            }
            uniform_queue.index = queued;
            if (queued == words) {
                uniform_queue.index = 0;
                const std::array<u32, 4> whole = uniform_queue.buffer;
                store(whole.data());
                ++index;
            }
        }
        if (uniform_queue.index == 0) {
            while (i + words <= count && index < uniforms.f.size()) {
                store(values + i);
                ++index;
                i += words;
            }
        }
        if (index != first) {
            uniform_setup.index.Assign(index);
            first_index = first;
            written = index - first;
        }
    }

    for (; i < count; ++i) {
        if (!uniform_queue.Push(values[i], is_float32)) {
            continue;
        }

        const auto uniform = uniform_queue.Get(is_float32);
        if (uniform_setup.index >= uniforms.f.size()) [[unlikely]] {
            // Some games may submit OOB indexes (mainly 0x7F), which is
            // ignored on real HW.
            break;
        }

        const u32 index = uniform_setup.index.Value();
        const auto prev = std::exchange(uniforms.f[index], uniform);
        uniforms_dirty |= prev != uniform;
        uniform_raw_mode[index] = 0;
        uniform_setup.index.Assign(index + 1);

        if (!first_index) {
            first_index = index;
        }
        ++written;
    }

    if (written == 0) {
        return std::nullopt;
    }
    return UniformWriteRange{*first_index, written};
}

u64 ShaderSetup::GetProgramCodeHash() {
    if (program_code_hash_dirty) {
        const auto& prog_code = GetProgramCode();
        program_code_hash =
            Common::ComputeHash64(prog_code.data(), biggest_program_size * sizeof(u32));
        program_code_hash_dirty = false;
    }
    return program_code_hash;
}

u64 ShaderSetup::GetSwizzleDataHash() {
    if (swizzle_data_hash_dirty) {
        swizzle_data_hash =
            Common::ComputeHash64(swizzle_data.data(), biggest_swizzle_size * sizeof(u32));
        swizzle_data_hash_dirty = false;
    }
    return swizzle_data_hash;
}

// Returns the index of the highest most significant bit set.
// 0001 -> 0, 0010 -> 1, 1000 -> 3...
inline u32 HighestSetBitIndex(u32 mask) {
#if defined(_MSC_VER)
    unsigned long index;
    _BitScanReverse(&index, mask);
    return static_cast<u32>(index);
#else
    return 31u - static_cast<u32>(__builtin_clz(mask));
#endif
}

// Process 32 bit words in groups of 4. If any of the words changed,
// store the new value and return true, plus the index of the word
// that changed.
#if defined(CITRA_HAS_SSE42)
static inline u32 ProcessBlockSSE42(u32* dst, const u32* values) {
    // Load 4 old values into old_vals.
    const __m128i old_vals = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst));
    // Load 4 new values into new_vals.
    const __m128i new_vals = _mm_loadu_si128(reinterpret_cast<const __m128i*>(values));
    // Compare both, eq now holds 4 words where each word X is either
    // 0xFFFFFFFF if words at X are equal or 0x0 if words at X differ.
    const __m128i eq = _mm_cmpeq_epi32(old_vals, new_vals);

    // Load all 0xF values
    const __m128i ones = _mm_set1_epi32(-1);

    // If eq is all 0xF, old_vals and new_vals are equal, return.
    if (_mm_testc_si128(eq, ones)) {
        return std::numeric_limits<u32>::max();
    }

    // Store the new values to the destination.
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), new_vals);

    // First negate the result to make 0 signify equality instead of 1
    // (using a XOR of all F).
    // Then reduce the four 0x0/0xFFFFFFFF words to 4 bits
    // stored to the 4 LSB of res.
    const u32 res = static_cast<u32>(_mm_movemask_ps(_mm_castsi128_ps(_mm_xor_si128(eq, ones))));
    // Return the index of the highest 1, which corresponds to the index of the
    // last new word that differs (taking into account endianness).
    return HighestSetBitIndex(res);
}
#endif

#if defined(CITRA_HAS_NEON)
static inline u32 ProcessBlockNEON(u32* dst, const u32* values) {
    // Load 4 old values into old_vals.
    const uint32x4_t old_vals = vld1q_u32(dst);
    // Load 4 new values into new_vals.
    const uint32x4_t new_vals = vld1q_u32(values);
    // Compare both, neq now holds 4 words where each word X is either
    // 0x0 if words at X are equal or 0xFFFFFFFF if words at X differ.
    const uint32x4_t neq = vmvnq_u32(vceqq_u32(old_vals, new_vals));

    // If neq is all 0, old_vals and new_vals are equal, return.
    if (vmaxvq_u32(neq) == 0) {
        return std::numeric_limits<u32>::max();
    }

    // Store the new values to the destination.
    vst1q_u32(dst, new_vals);

    // Extract the index of the highest value that was different.
    // This is achieved by ANDing the neq array with (0,1,2,3).
    // If word is equal, 0 is produced, otherwise the index
    // is produced. Then the max index is selected from the
    // result.
    static constexpr u32 index_arr[4] = {0, 1, 2, 3};
    const uint32x4_t indices = vld1q_u32(index_arr);
    return vmaxvq_u32(vandq_u32(neq, indices));
}
#endif

void ShaderSetup::UpdateProgramCodeRange(size_t offset, const u32* __restrict values, u32 count) {
    if (count == 0) [[unlikely]] {
        return;
    }

    u32* __restrict dst = &program_code[offset];
    bool any_changed = false;
    size_t last_changed_offset = offset;
    u32 i = 0;

    // SIMD block.
#if defined(CITRA_HAS_SSE42) || defined(CITRA_HAS_NEON)
    for (; i + 4 <= count; i += 4) {
#if defined(CITRA_HAS_SSE42)
        u32 index = ProcessBlockSSE42(dst + i, values + i);
#else
        u32 index = ProcessBlockNEON(dst + i, values + i);
#endif
        if (index != std::numeric_limits<u32>::max()) {
            any_changed = true;
            last_changed_offset = offset + i + index;
        }
    }
#endif

    // Scalar remainder (and whole loop when SIMD is not available).
    DISABLE_VECTORIZE
    for (; i < count; ++i) {
        u32& inst = dst[i];
        const u32 value = values[i];
        if (inst != value) {
            inst = value;
            any_changed = true;
            last_changed_offset = offset + i;
        }
    }

    if (any_changed) {
        if (!program_code_hash_dirty) {
            MakeProgramCodeDirty();
        }
        if ((last_changed_offset + 1) > biggest_program_size) {
            biggest_program_size = last_changed_offset + 1;
        }
    }
}

void ShaderSetup::UpdateSwizzleDataRange(size_t offset, const u32* __restrict values, u32 count) {
    if (count == 0) [[unlikely]] {
        return;
    }

    u32* __restrict dst = &swizzle_data[offset];
    bool any_changed = false;
    size_t last_changed_offset = offset;
    u32 i = 0;

    // SIMD block.
#if defined(CITRA_HAS_SSE42) || defined(CITRA_HAS_NEON)
    for (; i + 4 <= count; i += 4) {
#if defined(CITRA_HAS_SSE42)
        u32 index = ProcessBlockSSE42(dst + i, values + i);
#else
        u32 index = ProcessBlockNEON(dst + i, values + i);
#endif
        if (index != std::numeric_limits<u32>::max()) {
            any_changed = true;
            last_changed_offset = offset + i + index;
        }
    }
#endif

    // Scalar remainder (and whole loop when SIMD is not available).
    DISABLE_VECTORIZE
    for (; i < count; ++i) {
        u32& inst = dst[i];
        const u32 value = values[i];
        if (inst != value) {
            inst = value;
            any_changed = true;
            last_changed_offset = offset + i;
        }
    }

    if (any_changed) {
        if (!swizzle_data_hash_dirty) {
            MakeSwizzleDataDirty();
        }
        if ((last_changed_offset + 1) > biggest_swizzle_size) {
            biggest_swizzle_size = last_changed_offset + 1;
        }
    }
}

void ShaderSetup::DoProgramCodeFixup() {
    // This function holds shader fixups that are required for proper emulation of some games. As
    // emulation gets more accurate the goal is to have this function as empty as possible.
    // WARNING: If the hashing method is changed, the hashes of the different shaders will need
    // adjustment.

    /*
    if (!requires_fixup || !program_code_pending_fixup) {
        return;
    }
    program_code_pending_fixup = false;

    auto prepare_for_fixup = [this]() {
        memcpy(program_code_fixup.data(), program_code.data(), program_code.size());
        has_fixup = true;
        program_code_hash_dirty = true;
    };
    */

    // No games require shader fixups right now
}

} // namespace Pica
