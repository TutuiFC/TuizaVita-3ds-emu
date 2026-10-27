// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/write_epochs.h"

#include <algorithm>
#include <memory>
#include <new>

namespace Common::WriteEpochs {

std::atomic<bool> enabled{true};

namespace {
constexpr std::size_t kPageBits = 12;

/**
 * Las zonas seguidas: la FCRAM (0) y, desde 0.4.1.1, la VRAM (1). Zafiro Alfa y
 * Super Mario 3D Land tienen los vertices en VRAM ("copiado por: todo" en
 * 0.4.1.0), y alli tambien escriben las DMA, los rellenos y las
 * transferencias, que pasan por aqui.
 */
struct Region {
    const u8* base = nullptr;
    std::size_t size = 0;
    std::size_t page_count = 0;
    std::unique_ptr<std::atomic<u32>[]> pages;
    /// La fuente del ultimo aviso de cada pagina (diagnostico, 0.4.1.0).
    std::unique_ptr<std::atomic<u8>[]> page_source;
    /// Se deja de confiar en toda la zona (DistrustRegionOf).
    std::atomic<bool> distrusted{false};
};
Region g_regions[2];

/// La ultima epoca repartida. Empieza en 1: 0 es "nunca avisada".
std::atomic<u32> g_epoch{1};
/// MarkAll: todo lo verificado antes de esta epoca deja de valer.
std::atomic<u32> g_floor{0};
std::atomic<u32> g_marks{0};
std::atomic<u32> g_mark_alls{0};
std::atomic<u32> g_source_calls[kSources]{};
std::atomic<u32> g_source_kib[kSources]{};

/// Diferencia con vuelta: 'a' es posterior a 'b'.
bool After(u32 a, u32 b) {
    return static_cast<s32>(a - b) > 0;
}

Region* RegionOf(const u8* p) {
    for (Region& region : g_regions) {
        if (region.page_count != 0 && p >= region.base && p < region.base + region.size) {
            return &region;
        }
    }
    return nullptr;
}

void SetRegion(Region& region, const u8* base, std::size_t size) {
    const std::size_t pages = (size + (std::size_t{1} << kPageBits) - 1) >> kPageBits;
    region.page_count = 0;
    region.pages.reset(new (std::nothrow) std::atomic<u32>[pages]());
    region.page_source.reset(new (std::nothrow) std::atomic<u8>[pages]());
    if (region.pages == nullptr || region.page_source == nullptr) {
        region.pages.reset();
        region.page_source.reset();
        region.base = nullptr;
        region.size = 0;
        return;
    }
    region.base = base;
    region.size = size;
    region.distrusted.store(false, std::memory_order_relaxed);
    region.page_count = pages;
}
} // Anonymous namespace

void SetRange(const u8* base, std::size_t size) {
    SetRegion(g_regions[0], base, size);
    // Lo verificado con la tabla anterior no vale con esta.
    MarkAll();
}

void SetVramRange(const u8* base, std::size_t size) {
    SetRegion(g_regions[1], base, size);
    MarkAll();
}

void MarkHost(const void* pointer, std::size_t size, u32 source) {
    if (size == 0) {
        return;
    }
    const u8* const p = static_cast<const u8*>(pointer);
    Region* const region = RegionOf(p);
    if (region == nullptr) {
        return;
    }
    if (source < kSources) {
        g_source_calls[source].fetch_add(1, std::memory_order_relaxed);
        g_source_kib[source].fetch_add(static_cast<u32>((size + 1023) >> 10),
                                       std::memory_order_relaxed);
    }
    const std::size_t offset = static_cast<std::size_t>(p - region->base);
    const std::size_t first = offset >> kPageBits;
    const std::size_t end_offset = std::min<std::size_t>(offset + size, region->size);
    const std::size_t last = (end_offset - 1) >> kPageBits;
    // release: quien lea esta epoca con Now() ve tambien lo escrito antes.
    const u32 epoch = g_epoch.fetch_add(1, std::memory_order_release) + 1;
    for (std::size_t page = first; page <= last && page < region->page_count; page++) {
        region->page_source[page].store(static_cast<u8>(source), std::memory_order_relaxed);
        region->pages[page].store(epoch, std::memory_order_relaxed);
    }
    g_marks.fetch_add(1, std::memory_order_relaxed);
    // Un tramo que se sale de esta zona (0.4.1.4): el resto, a la suya si la hay.
    if (offset + size > region->size) {
        const std::size_t done = region->size - offset;
        MarkHost(p + done, size - done, source);
    }
}

void MarkAll() {
    g_mark_alls.fetch_add(1, std::memory_order_relaxed);
    const u32 epoch = g_epoch.fetch_add(1, std::memory_order_release) + 1;
    g_floor.store(epoch, std::memory_order_release);
    g_marks.fetch_add(1, std::memory_order_relaxed);
}

void DistrustRegionOf(const void* pointer) {
    if (Region* const region = RegionOf(static_cast<const u8*>(pointer))) {
        region->distrusted.store(true, std::memory_order_release);
    }
}

bool InVram(const void* pointer) {
    return RegionOf(static_cast<const u8*>(pointer)) == &g_regions[1];
}

u32 Now() {
    return g_epoch.load(std::memory_order_acquire);
}

bool CleanSince(const void* pointer, std::size_t size, u32 epoch, u32* culprit) {
    if (culprit != nullptr) {
        *culprit = kCulpritOutside;
    }
    if (!enabled.load(std::memory_order_relaxed) || size == 0) {
        return false;
    }
    const u8* const p = static_cast<const u8*>(pointer);
    Region* const region = RegionOf(p);
    if (region == nullptr || p + size > region->base + region->size) {
        return false;
    }
    if (region->distrusted.load(std::memory_order_acquire)) {
        if (culprit != nullptr) {
            *culprit = kCulpritDistrusted;
        }
        return false;
    }
    if (After(g_floor.load(std::memory_order_acquire), epoch)) {
        if (culprit != nullptr) {
            *culprit = kCulpritFloor;
        }
        return false;
    }
    const std::size_t offset = static_cast<std::size_t>(p - region->base);
    const std::size_t first = offset >> kPageBits;
    const std::size_t last = (offset + size - 1) >> kPageBits;
    for (std::size_t page = first; page <= last; page++) {
        if (After(region->pages[page].load(std::memory_order_acquire), epoch)) {
            if (culprit != nullptr) {
                *culprit = region->page_source[page].load(std::memory_order_relaxed);
            }
            return false;
        }
    }
    return true;
}

void TakeSourceStats(u32* calls, u32* kib) {
    for (u32 i = 0; i < kSources; i++) {
        calls[i] = g_source_calls[i].exchange(0, std::memory_order_relaxed);
        kib[i] = g_source_kib[i].exchange(0, std::memory_order_relaxed);
    }
}

u32 MarkAlls() {
    return g_mark_alls.load(std::memory_order_relaxed);
}

u32 Marks() {
    return g_marks.load(std::memory_order_relaxed);
}

} // namespace Common::WriteEpochs
