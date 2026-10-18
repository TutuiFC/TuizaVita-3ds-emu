// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <atomic>
#include "citra_vita/vita_i18n.h"

namespace VitaFrontend::I18n {
namespace {

struct Entry {
    const char* text[kLanguageCount];
};

constexpr Entry kTable[kStrCount] = {
#define TR(id, en, es, ca, fr, de, it, pt) {{en, es, ca, fr, de, it, pt}},
#include "vita_strings.inc"
#undef TR
};

constexpr const char* kNativeNames[kLanguageCount] = {
    "English", "Español", "Català", "Français", "Deutsch", "Italiano", "Português",
};

std::atomic<int> g_language{kEnglish};

/// Latin-1, Latin extendido y la puntuacion general van a la fuente latina.
int InLatinGroup(unsigned int c) {
    return c <= 0x024F || (c >= 0x2000 && c <= 0x206F) ? 1 : 0;
}

int InOtherGroup(unsigned int c) {
    return InLatinGroup(c) ? 0 : 1;
}

} // Anonymous namespace

void SetLanguage(int language) {
    g_language.store(std::clamp(language, 0, kLanguageCount - 1), std::memory_order_relaxed);
}

int CurrentLanguage() {
    return g_language.load(std::memory_order_relaxed);
}

const char* NativeName(int language) {
    return kNativeNames[std::clamp(language, 0, kLanguageCount - 1)];
}

const char* TrIn(int language, Str id) {
    if (id < 0 || id >= kStrCount) {
        return "";
    }
    return kTable[id].text[std::clamp(language, 0, kLanguageCount - 1)];
}

const char* Tr(Str id) {
    return TrIn(CurrentLanguage(), id);
}

vita2d_pgf* LoadFont() {
    // Grupos explicitos en las dos: el resultado no depende del orden en que
    // vita2d recorra la lista.
    const vita2d_system_pgf_config configs[] = {
        {SCE_FONT_LANGUAGE_LATIN, InLatinGroup},
        {SCE_FONT_LANGUAGE_JAPANESE, InOtherGroup},
    };
    vita2d_pgf* font = vita2d_load_system_pgf(2, configs);
    if (font == nullptr) {
        font = vita2d_load_default_pgf();
    }
    return font;
}

void DrawTextFit(vita2d_pgf* font, int x, int y, unsigned int color, float scale,
                 const char* text, int max_width) {
    if (text == nullptr || text[0] == '\0') {
        return;
    }
    const int width = vita2d_pgf_text_width(font, scale, text);
    if (width > max_width && width > 0) {
        scale *= static_cast<float>(max_width) / static_cast<float>(width);
    }
    vita2d_pgf_draw_text(font, x, y, color, scale, text);
}

} // namespace VitaFrontend::I18n
