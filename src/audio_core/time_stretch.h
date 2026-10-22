// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <vector>
#include "common/common_types.h"

namespace soundtouch {
class SoundTouch;
}

namespace AudioCore {

class TimeStretcher {
public:
    TimeStretcher();
    ~TimeStretcher();

    void SetOutputSampleRate(unsigned int sample_rate);

    /// @param in       Input sample buffer
    /// @param num_in   Number of input frames in `in`
    /// @param out      Output sample buffer
    /// @param num_out  Desired number of output frames in `out`
    /// @returns Actual number of frames written to `out`
    std::size_t Process(const s16* in, std::size_t num_in, s16* out, std::size_t num_out);

    void Clear();

    void Flush();

private:
    std::unique_ptr<soundtouch::SoundTouch> sound_touch;
    double stretch_ratio = 1.0;
    /// Conversion a y desde float, reutilizada entre bloques (0.3.4.6): antes
    /// eran dos std::vector nuevos en cada bloque de salida de audio.
    std::vector<float> float_in;
    std::vector<float> float_out;
};

} // namespace AudioCore
