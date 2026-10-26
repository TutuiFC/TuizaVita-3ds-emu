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

const u8* g_base = nullptr;
std::size_t g_size = 0;
std::unique_ptr<std::atomic<u32>[]> g_pages;
/// La fuente del ultimo aviso de cada pagina (diagnostico, 0.4.1.0).
std::unique_ptr<std::atomic<u8>[]> g_page_source;
std::size_t g_page_count = 0;
/// La ultima epoca repartida. Empieza en 1: 0 es "nunca avisada".
std::atomic<u32> g_epoch{1};
/// MarkAll: todo lo verificado antes de esta epoca deja de valer.
std::atomic<u32> g_floor{0};
std::atomic<u32> g_marks{0};
std::atomic<u32> g_source_calls[kSources]{};
std::atomic<u32> g_source_kib[kSources]{};

/// Diferencia con vuelta: 'a' es posterior a 'b'.
bool After(u32 a, u32 b) {
    return static_cast<s32>(a - b) > 0;
}
} // Anonymous namespace

void SetRange(const u8* base, std::size_t size) {
    const std::size_t pages = (size + (std::size_t{1} << kPageBits) - 1) >> kPageBits;
    g_pages.reset(new (std::nothrow) std::atomic<u32>[pages]());
    g_page_source.reset(new (std::nothrow) std::atomic<u8>[pages]());
    if (g_page_source == nullptr) {
        g_pages.reset();
    }
    g_page_count = g_pages != nullptr ? pages : 0;
    g_base = g_pages != nullptr ? base : nullptr;
    g_size = g_pages != nullptr ? size : 0;
    // Lo verificado con la tabla anterior no vale con esta.
    MarkAll();
}

void MarkHost(const void* pointer, std::size_t size, u32 source) {
    if (size == 0 || g_page_count == 0) {
        return;
    }
    const u8* const p = static_cast<const u8*>(pointer);
    if (p < g_base || p >= g_base + g_size) {
        return;
    }
    if (source < kSources) {
        g_source_calls[source].fetch_add(1, std::memory_order_relaxed);
        g_source_kib[source].fetch_add(static_cast<u32>((size + 1023) >> 10),
                                       std::memory_order_relaxed);
    }
    const std::size_t first = static_cast<std::size_t>(p - g_base) >> kPageBits;
    const std::size_t end_offset =
        std::min<std::size_t>(static_cast<std::size_t>(p - g_base) + size, g_size);
    const std::size_t last = (end_offset - 1) >> kPageBits;
    // release: quien lea esta epoca con Now() ve tambien lo escrito antes.
    const u32 epoch = g_epoch.fetch_add(1, std::memory_order_release) + 1;
    for (std::size_t page = first; page <= last && page < g_page_count; page++) {
        g_page_source[page].store(static_cast<u8>(source), std::memory_order_relaxed);
        g_pages[page].store(epoch, std::memory_order_relaxed);
    }
    g_marks.fetch_add(1, std::memory_order_relaxed);
}

void MarkAll() {
    const u32 epoch = g_epoch.fetch_add(1, std::memory_order_release) + 1;
    g_floor.store(epoch, std::memory_order_release);
    g_marks.fetch_add(1, std::memory_order_relaxed);
}

u32 Now() {
    return g_epoch.load(std::memory_order_acquire);
}

bool CleanSince(const void* pointer, std::size_t size, u32 epoch, u32* culprit) {
    if (culprit != nullptr) {
        *culprit = kSources;
    }
    if (!enabled.load(std::memory_order_relaxed) || g_page_count == 0 || size == 0) {
        return false;
    }
    const u8* const p = static_cast<const u8*>(pointer);
    if (p < g_base || p + size > g_base + g_size) {
        return false;
    }
    if (After(g_floor.load(std::memory_order_acquire), epoch)) {
        return false;
    }
    const std::size_t first = static_cast<std::size_t>(p - g_base) >> kPageBits;
    const std::size_t last = (static_cast<std::size_t>(p - g_base) + size - 1) >> kPageBits;
    for (std::size_t page = first; page <= last; page++) {
        if (After(g_pages[page].load(std::memory_order_acquire), epoch)) {
            if (culprit != nullptr) {
                *culprit = g_page_source[page].load(std::memory_order_relaxed);
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

u32 Marks() {
    return g_marks.load(std::memory_order_relaxed);
}

} // namespace Common::WriteEpochs
