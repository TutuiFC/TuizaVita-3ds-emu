// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

namespace Memory {
class MemorySystem;
}

namespace Service::Y2R {
struct ConversionConfiguration;
} // namespace Service::Y2R

namespace HW::Y2R {
void PerformConversion(Memory::MemorySystem& memory, Service::Y2R::ConversionConfiguration cvt);
/// El banco de la conversion (0.4.1.4): solo con ux0:/data/azahar/banco_mem/y2r.txt.
void RunBenchIfAny();
} // namespace HW::Y2R
