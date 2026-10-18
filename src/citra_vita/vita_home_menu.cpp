// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <psp2/rtc.h>
#include <psp2/touch.h>
#include "citra_vita/vita_home_menu.h"
#include "citra_vita/vita_i18n.h"
#include "citra_vita/vita_version.h"
#include "common/string_util.h"
#include "core/loader/loader.h"
#include "core/loader/smdh.h"

namespace VitaFrontend {
namespace {

namespace I18n = VitaFrontend::I18n;

/// vita2d pinta en ABGR; aqui los colores se escriben como 0xRRGGBB.
constexpr unsigned int Rgb(unsigned int rgb, unsigned int alpha = 0xFF) {
    return (alpha << 24) | ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF);
}

/// Mezcla de dos colores 0xRRGGBB, t de 0 a 1.
unsigned int Mix(unsigned int a, unsigned int b, float t) {
    const auto channel = [t](unsigned int x, unsigned int y) {
        return static_cast<unsigned int>(static_cast<float>(x) +
                                         (static_cast<float>(y) - static_cast<float>(x)) * t);
    };
    return (channel((a >> 16) & 0xFF, (b >> 16) & 0xFF) << 16) |
           (channel((a >> 8) & 0xFF, (b >> 8) & 0xFF) << 8) | channel(a & 0xFF, b & 0xFF);
}

constexpr unsigned int kInk = 0x3A3F46;
constexpr unsigned int kInkDim = 0x7D848D;
constexpr unsigned int kCyan = 0x2FB2F5;
constexpr unsigned int kCyanLight = 0x9EDCFF;

constexpr int kScreenW = 960;
constexpr int kScreenH = 544;
constexpr int kTopBarH = 34;

/// La ficha del juego, como la pantalla superior de la 3DS.
constexpr float kPanelX = 30.0f;
constexpr float kPanelY = 48.0f;
constexpr float kPanelW = 900.0f;
constexpr float kPanelH = 176.0f;

/// La cuadricula: dos filas, rellena por columnas como la de la consola.
constexpr int kRows = 2;
constexpr float kTile = 88.0f;
constexpr float kStep = 104.0f;
constexpr float kGridLeft = 56.0f;
constexpr float kGridTop = 248.0f;
constexpr int kVisibleColumns = 8;
constexpr float kSliderY = 462.0f;
constexpr float kBottomBarY = 488.0f;

/// Iconos de 48x48 en celdas de 50: un pixel de borde repetido para que el
/// filtro lineal no mezcle el icono con el de al lado.
constexpr int kAtlasSize = 512;
constexpr int kCell = 50;
constexpr int kCellsPerRow = kAtlasSize / kCell;
constexpr int kAtlasSlots = kCellsPerRow * kCellsPerRow;
constexpr int kIconSize = 48;

/// Capturas de la pantalla superior (0.3.4.0): 400x240 en RGB565.
constexpr int kShotW = 400;
constexpr int kShotH = 240;
constexpr u32 kShotMagic = 0x43535A41; // "AZSC"
constexpr char kShotDir[] = "ux0:/data/azahar/capturas";

constexpr u64 kRepeatDelayUs = 380'000;
constexpr u64 kRepeatEveryUs = 90'000;

void FillRoundRect(float x, float y, float w, float h, float r, unsigned int color) {
    r = std::min(r, std::min(w, h) * 0.5f);
    if (r < 1.0f) {
        vita2d_draw_rectangle(x, y, w, h, color);
        return;
    }
    vita2d_draw_rectangle(x + r, y, w - 2.0f * r, h, color);
    vita2d_draw_rectangle(x, y + r, r, h - 2.0f * r, color);
    vita2d_draw_rectangle(x + w - r, y + r, r, h - 2.0f * r, color);
    vita2d_draw_fill_circle(x + r, y + r, r, color);
    vita2d_draw_fill_circle(x + w - r, y + r, r, color);
    vita2d_draw_fill_circle(x + r, y + h - r, r, color);
    vita2d_draw_fill_circle(x + w - r, y + h - r, r, color);
}

/// Los botones de la Vita: circulo de color con el simbolo en blanco.
enum class Glyph { Cross, Circle };

void DrawGlyph(Glyph glyph, float cx, float cy) {
    const unsigned int body = glyph == Glyph::Cross ? Rgb(0x5C9FE8) : Rgb(0xF06470);
    vita2d_draw_fill_circle(cx, cy, 11.0f, body);
    if (glyph == Glyph::Cross) {
        for (float o = -1.0f; o <= 1.0f; o += 0.5f) {
            vita2d_draw_line(cx - 5.0f + o, cy - 5.0f, cx + 5.0f + o, cy + 5.0f, Rgb(0xFFFFFF));
            vita2d_draw_line(cx + 5.0f + o, cy - 5.0f, cx - 5.0f + o, cy + 5.0f, Rgb(0xFFFFFF));
        }
    } else {
        vita2d_draw_fill_circle(cx, cy, 6.5f, Rgb(0xFFFFFF));
        vita2d_draw_fill_circle(cx, cy, 4.5f, body);
    }
}

/// START y SELECT: una pastilla gris con el nombre. Devuelve su ancho.
float DrawPill(vita2d_pgf* font, const char* label, float x, float cy) {
    constexpr float kScale = 0.62f;
    const float text_w = static_cast<float>(vita2d_pgf_text_width(font, kScale, label));
    const float w = text_w + 14.0f;
    FillRoundRect(x, cy - 10.0f, w, 20.0f, 10.0f, Rgb(0x5A6068));
    vita2d_pgf_draw_text(font, static_cast<int>(x + 7.0f), static_cast<int>(cy + 5.0f),
                         Rgb(0xFFFFFF), kScale, label);
    return w;
}

/// Color de fondo para un juego sin icono, fijo para cada nombre.
u32 TintFor(const std::string& name) {
    static constexpr unsigned int kTints[] = {0xF2A65A, 0x6CC4A1, 0x7FA7E6, 0xE57F9E,
                                              0xB08AE0, 0xE0C25A, 0x5FB8D6, 0x9BC66B};
    u32 hash = 2166136261u;
    for (const char c : name) {
        hash = (hash ^ static_cast<u8>(c)) * 16777619u;
    }
    return kTints[hash % (sizeof(kTints) / sizeof(kTints[0]))];
}

std::string StripExtension(const std::string& name) {
    const std::size_t dot = name.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

/// El idioma del SMDH que toca para el idioma del menu.
Loader::SMDH::TitleLanguage SmdhLanguage() {
    using L = Loader::SMDH::TitleLanguage;
    switch (I18n::CurrentLanguage()) {
    case I18n::kSpanish:
    case I18n::kCatalan:
        return L::Spanish;
    case I18n::kFrench:
        return L::French;
    case I18n::kGerman:
        return L::German;
    case I18n::kItalian:
        return L::Italian;
    case I18n::kPortuguese:
        return L::Portuguese;
    default:
        return L::English;
    }
}

/// Saltos de linea fuera y espacios de los lados recortados.
std::string CleanTitle(std::string text) {
    for (char& c : text) {
        if (c == '\n' || c == '\r') {
            c = ' ';
        }
    }
    const std::size_t first = text.find_first_not_of(' ');
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(' ');
    return text.substr(first, last - first + 1);
}

/// Recorta con "..." para que quepa en max_width.
std::string Ellipsize(vita2d_pgf* font, float scale, const std::string& text, int max_width) {
    if (vita2d_pgf_text_width(font, scale, text.c_str()) <= max_width) {
        return text;
    }
    std::string cut = text;
    while (!cut.empty()) {
        // Quitar un caracter UTF-8 entero, no medio.
        do {
            cut.pop_back();
        } while (!cut.empty() && (static_cast<u8>(cut.back()) & 0xC0) == 0x80);
        if (!cut.empty()) {
            const std::string candidate = cut + "...";
            if (vita2d_pgf_text_width(font, scale, candidate.c_str()) <= max_width) {
                return candidate;
            }
        }
    }
    return "...";
}

/// ux0:/data/azahar/capturas/<nombre del fichero>.scr
std::string ShotPath(const std::string& rom_path) {
    const std::size_t slash = rom_path.find_last_of('/');
    const std::string name = slash == std::string::npos ? rom_path : rom_path.substr(slash + 1);
    return std::string(kShotDir) + "/" + name + ".scr";
}

struct ShotHeader {
    u32 magic;
    u16 width;
    u16 height;
};

} // Anonymous namespace

void CaptureGameScreen(const std::string& path, int x, int y, int w, int h) {
    SceDisplayFrameBuf frame{};
    frame.size = sizeof(frame);
    if (sceDisplayGetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME) < 0 || frame.base == nullptr ||
        frame.pixelformat != SCE_DISPLAY_PIXELFORMAT_A8B8G8R8 || w <= 0 || h <= 0 || x < 0 ||
        y < 0 || x + w > static_cast<int>(frame.width) || y + h > static_cast<int>(frame.height)) {
        return;
    }
    const auto* pixels = static_cast<const u32*>(frame.base);
    std::vector<u16> image(static_cast<std::size_t>(kShotW * kShotH));
    // Al reducir (pantalla grande) se promedian 2x2 para que no salga dentado.
    const int step = w >= 2 * kShotW - 40 ? 1 : 0;
    for (int row = 0; row < kShotH; row++) {
        const int sy = y + row * h / kShotH;
        const u32* line = pixels + static_cast<std::size_t>(sy) * frame.pitch;
        const u32* next = step != 0 && sy + 1 < y + h ? line + frame.pitch : line;
        for (int column = 0; column < kShotW; column++) {
            const int sx = x + column * w / kShotW;
            const u32 taps[4] = {line[sx], line[sx + step], next[sx], next[sx + step]};
            u32 r = 0;
            u32 g = 0;
            u32 b = 0;
            for (const u32 c : taps) {
                r += c & 0xFF;
                g += (c >> 8) & 0xFF;
                b += (c >> 16) & 0xFF;
            }
            r /= 4;
            g /= 4;
            b /= 4;
            image[static_cast<std::size_t>(row * kShotW + column)] =
                static_cast<u16>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }

    sceIoMkdir(kShotDir, 0777);
    const SceUID fd =
        sceIoOpen(ShotPath(path).c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        return;
    }
    const ShotHeader header{kShotMagic, kShotW, kShotH};
    sceIoWrite(fd, &header, sizeof(header));
    sceIoWrite(fd, image.data(), static_cast<SceSize>(image.size() * sizeof(u16)));
    sceIoClose(fd);
}

HomeMenu::HomeMenu(vita2d_pgf* font_) : font(font_) {
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
}

HomeMenu::~HomeMenu() {
    ReleaseTextures();
}

void HomeMenu::SetGames(const std::vector<HomeGame>& games) {
    ReleaseTextures();
    entries.clear();
    entries.reserve(games.size());
    for (const HomeGame& game : games) {
        Entry entry;
        entry.name = game.name;
        entry.path = game.path;
        entry.fallback = StripExtension(game.name);
        entry.title = entry.fallback;
        entry.tint = TintFor(game.name);
        entries.push_back(std::move(entry));
    }
    selected = 0;
    camera = camera_target = 0.0f;
}

const std::string& HomeMenu::SelectedPath() const {
    static const std::string kEmpty;
    return entries.empty() ? kEmpty : entries[selected].path;
}

void HomeMenu::LoadInfo(Entry& entry) {
    entry.info_loaded = true;
    std::vector<u8> data;
    try {
        auto loader = Loader::GetLoader(entry.path);
        if (loader == nullptr || loader->ReadIcon(data) != Loader::ResultStatus::Success) {
            return;
        }
    } catch (...) {
        return;
    }
    if (data.size() < sizeof(Loader::SMDH) || !Loader::IsValidSMDH(data)) {
        return;
    }
    Loader::SMDH smdh;
    std::memcpy(&smdh, data.data(), sizeof(smdh));

    if (entry.icon.empty()) {
        entry.icon = smdh.GetIcon(true);
        if (entry.icon.size() != static_cast<std::size_t>(kIconSize * kIconSize)) {
            entry.icon.clear();
        }
        entry.uploaded = false;
    }

    // El idioma del menu primero; si el juego no lo trae, el ingles, y luego
    // cualquiera que tenga algo escrito.
    using L = Loader::SMDH::TitleLanguage;
    const L order[] = {SmdhLanguage(), L::English,    L::Spanish, L::French,
                       L::German,      L::Italian,    L::Portuguese, L::Dutch,
                       L::Japanese,    L::Russian};
    std::string title;
    std::string publisher;
    for (const L language : order) {
        const auto& t = smdh.titles[static_cast<std::size_t>(language)];
        if (title.empty()) {
            title = CleanTitle(Common::UTF16BufferToUTF8(t.long_title));
            if (title.empty()) {
                title = CleanTitle(Common::UTF16BufferToUTF8(t.short_title));
            }
        }
        if (publisher.empty()) {
            publisher = CleanTitle(Common::UTF16BufferToUTF8(t.publisher));
        }
        if (!title.empty() && !publisher.empty()) {
            break;
        }
    }
    if (!title.empty()) {
        entry.title = title;
    }
    entry.publisher = publisher;
}

void HomeMenu::LoadPendingInfo() {
    if (entries.empty()) {
        return;
    }
    // Primero el seleccionado, luego los de alrededor, luego el resto.
    const int count = Count();
    for (int distance = 0; distance < count; distance++) {
        for (const int index : {selected + distance, selected - distance}) {
            if (index >= 0 && index < count && !entries[index].info_loaded) {
                LoadInfo(entries[index]);
                return;
            }
        }
    }
}

void HomeMenu::RefreshTitles() {
    for (Entry& entry : entries) {
        entry.info_loaded = false;
        entry.title = entry.fallback;
        entry.publisher.clear();
    }
}

bool HomeMenu::EnsureAtlas() {
    if (atlas != nullptr) {
        return true;
    }
    if (atlas_failed) {
        return false;
    }
    atlas = vita2d_create_empty_texture_format(kAtlasSize, kAtlasSize,
                                               SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB);
    if (atlas == nullptr) {
        atlas_failed = true;
        return false;
    }
    vita2d_texture_set_filters(atlas, SCE_GXM_TEXTURE_FILTER_LINEAR,
                               SCE_GXM_TEXTURE_FILTER_LINEAR);
    for (Entry& entry : entries) {
        entry.uploaded = false;
    }
    return true;
}

void HomeMenu::UploadIcon(int index) {
    Entry& entry = entries[index];
    if (entry.uploaded || entry.icon.empty() || index >= kAtlasSlots || !EnsureAtlas()) {
        return;
    }
    auto* base = static_cast<u8*>(vita2d_texture_get_datap(atlas));
    const unsigned int stride = vita2d_texture_get_stride(atlas);
    const int cell_x = (index % kCellsPerRow) * kCell;
    const int cell_y = (index / kCellsPerRow) * kCell;
    for (int y = -1; y <= kIconSize; y++) {
        const int source_y = std::clamp(y, 0, kIconSize - 1);
        auto* row = reinterpret_cast<u16*>(base + (cell_y + 1 + y) * stride) + cell_x + 1;
        for (int x = -1; x <= kIconSize; x++) {
            const int source_x = std::clamp(x, 0, kIconSize - 1);
            row[x] = entry.icon[source_y * kIconSize + source_x];
        }
    }
    entry.uploaded = true;
}

void HomeMenu::ReleaseTextures() {
    if (atlas != nullptr || shot != nullptr) {
        vita2d_wait_rendering_done();
    }
    if (atlas != nullptr) {
        vita2d_free_texture(atlas);
        atlas = nullptr;
    }
    if (shot != nullptr) {
        vita2d_free_texture(shot);
        shot = nullptr;
    }
    shot_for = -1;
    atlas_failed = false;
    for (Entry& entry : entries) {
        entry.uploaded = false;
    }
}

void HomeMenu::ForgetScreenshot(const std::string& path) {
    for (Entry& entry : entries) {
        if (entry.path == path) {
            entry.shot_state = 0;
        }
    }
    shot_for = -1;
    shown_selection = -1;
}

bool HomeMenu::LoadScreenshot(int index) {
    Entry& entry = entries[index];
    if (entry.shot_state == 1) {
        return false;
    }
    if (shot_for == index && shot != nullptr) {
        return true;
    }
    const SceUID fd = sceIoOpen(ShotPath(entry.path).c_str(), SCE_O_RDONLY, 0);
    if (fd < 0) {
        entry.shot_state = 1;
        return false;
    }
    ShotHeader header{};
    bool ok = sceIoRead(fd, &header, sizeof(header)) == static_cast<int>(sizeof(header)) &&
              header.magic == kShotMagic && header.width == kShotW && header.height == kShotH;
    if (ok && shot == nullptr) {
        shot = vita2d_create_empty_texture_format(kShotW, kShotH,
                                                  SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB);
        if (shot != nullptr) {
            vita2d_texture_set_filters(shot, SCE_GXM_TEXTURE_FILTER_LINEAR,
                                       SCE_GXM_TEXTURE_FILTER_LINEAR);
        }
    }
    ok = ok && shot != nullptr;
    if (ok) {
        // La textura la puede estar leyendo el fotograma anterior.
        vita2d_wait_rendering_done();
        auto* base = static_cast<u8*>(vita2d_texture_get_datap(shot));
        const unsigned int stride = vita2d_texture_get_stride(shot);
        for (int row = 0; row < kShotH && ok; row++) {
            ok = sceIoRead(fd, base + row * stride, kShotW * sizeof(u16)) ==
                 static_cast<int>(kShotW * sizeof(u16));
        }
    }
    sceIoClose(fd);
    entry.shot_state = ok ? 2 : 1;
    shot_for = ok ? index : -1;
    return ok;
}

int HomeMenu::Columns() const {
    return (Count() + kRows - 1) / kRows;
}

void HomeMenu::FollowSelection() {
    free_camera = false;
    const int column = selected / kRows;
    int first = static_cast<int>(camera_target / kStep + 0.5f);
    if (column < first + 1) {
        first = column - 1;
    } else if (column > first + kVisibleColumns - 2) {
        first = column - kVisibleColumns + 2;
    }
    first = std::clamp(first, 0, std::max(0, Columns() - kVisibleColumns));
    camera_target = static_cast<float>(first) * kStep;
}

void HomeMenu::MoveSelection(int delta) {
    if (entries.empty()) {
        return;
    }
    selected = std::clamp(selected + delta, 0, Count() - 1);
    FollowSelection();
}

int HomeMenu::TileAt(int x, int y) const {
    for (int row = 0; row < kRows; row++) {
        const float top = kGridTop + static_cast<float>(row) * kStep;
        if (y < top || y >= top + kTile) {
            continue;
        }
        const float local = static_cast<float>(x) - kGridLeft + camera;
        if (local < 0.0f) {
            return -1;
        }
        const int column = static_cast<int>(local / kStep);
        if (local - static_cast<float>(column) * kStep >= kTile) {
            return -1;
        }
        const int index = column * kRows + row;
        return index < Count() ? index : -1;
    }
    return -1;
}

bool HomeMenu::Update(unsigned int buttons, unsigned int pressed) {
    frame++;
    bool launch = false;

    // Cruceta con repeticion al mantener, como en la consola.
    constexpr unsigned int kDirections = SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT |
                                         SCE_CTRL_RIGHT | SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER;
    const u64 now = sceKernelGetProcessTimeWide();
    unsigned int act = pressed & kDirections;
    if (act != 0) {
        repeat_buttons = act;
        repeat_next_us = now + kRepeatDelayUs;
    } else if ((buttons & repeat_buttons) != 0 && repeat_buttons != 0 && now >= repeat_next_us) {
        act = buttons & repeat_buttons;
        repeat_next_us = now + kRepeatEveryUs;
    } else if ((buttons & repeat_buttons) == 0) {
        repeat_buttons = 0;
    }

    if (act & SCE_CTRL_UP) {
        if (selected % kRows != 0) {
            MoveSelection(-1);
        }
    }
    if (act & SCE_CTRL_DOWN) {
        if (selected % kRows != kRows - 1 && selected + 1 < Count()) {
            MoveSelection(1);
        }
    }
    if (act & SCE_CTRL_LEFT) {
        if (selected >= kRows) {
            MoveSelection(-kRows);
        }
    }
    if (act & SCE_CTRL_RIGHT) {
        if (selected / kRows < Columns() - 1) {
            // A la ultima columna aunque este a medias.
            MoveSelection(std::min(kRows, Count() - 1 - selected));
        }
    }
    if (act & SCE_CTRL_LTRIGGER) {
        MoveSelection(-kRows * (kVisibleColumns - 1));
    }
    if (act & SCE_CTRL_RTRIGGER) {
        MoveSelection(kRows * (kVisibleColumns - 1));
    }
    if ((pressed & SCE_CTRL_CROSS) && !entries.empty()) {
        launch = true;
    }

    // Tactil: tocar selecciona, tocar el seleccionado arranca, arrastrar desplaza.
    SceTouchData touch{};
    const bool down = sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) >= 0 && touch.reportNum > 0;
    if (down) {
        const int x = touch.report[0].x / 2;
        const int y = touch.report[0].y / 2;
        if (!touching) {
            touching = true;
            dragging = false;
            touch_start_x = x;
            touch_start_y = y;
            touch_start_camera = camera;
        }
        if (!dragging && std::abs(x - touch_start_x) > 14 &&
            touch_start_y >= static_cast<int>(kGridTop) - 10 && touch_start_y < kBottomBarY) {
            dragging = true;
        }
        if (dragging) {
            free_camera = true;
            const float max_camera =
                static_cast<float>(std::max(0, Columns() - kVisibleColumns)) * kStep;
            camera_target = std::clamp(
                touch_start_camera - static_cast<float>(x - touch_start_x), 0.0f, max_camera);
            camera = camera_target;
        }
        touch_last_x = x;
        touch_last_y = y;
    } else if (touching) {
        touching = false;
        if (!dragging) {
            const int index = TileAt(touch_last_x, touch_last_y);
            if (index >= 0) {
                if (index == selected) {
                    launch = true;
                } else {
                    selected = index;
                    FollowSelection();
                }
            } else if (touch_last_y >= static_cast<int>(kPanelY) &&
                       touch_last_y < static_cast<int>(kPanelY + kPanelH) &&
                       touch_last_x >= static_cast<int>(kPanelX) &&
                       touch_last_x < static_cast<int>(kPanelX + kPanelW) && !entries.empty()) {
                // La ficha de arriba tambien arranca, como tocar el titulo en la 3DS.
                launch = true;
            }
        }
        dragging = false;
    }

    if (!free_camera) {
        camera += (camera_target - camera) * 0.25f;
        if (std::abs(camera_target - camera) < 0.5f) {
            camera = camera_target;
        }
    }
    return launch;
}

void HomeMenu::DrawBackground() {
    constexpr int kBands = 34;
    for (int i = 0; i < kBands; i++) {
        const float t = static_cast<float>(i) / static_cast<float>(kBands - 1);
        const float y = static_cast<float>(i * kScreenH) / kBands;
        vita2d_draw_rectangle(0.0f, y, static_cast<float>(kScreenW),
                              static_cast<float>(kScreenH) / kBands + 1.0f,
                              Rgb(Mix(0xF1F3F5, 0xD3D8DE, t)));
    }
    // Las rayas finas del tema por defecto de la consola.
    for (int y = kTopBarH + 3; y < kScreenH; y += 6) {
        vita2d_draw_rectangle(0.0f, static_cast<float>(y), static_cast<float>(kScreenW), 1.0f,
                              Rgb(0xFFFFFF, 0x30));
    }
}

void HomeMenu::DrawTopBar() {
    vita2d_draw_rectangle(0.0f, 0.0f, static_cast<float>(kScreenW), kTopBarH, Rgb(0x2B3036));
    vita2d_draw_rectangle(0.0f, kTopBarH, static_cast<float>(kScreenW), 2.0f, Rgb(kCyan));
    vita2d_pgf_draw_text(font, 16, 24, Rgb(0xFFFFFF), 1.0f, "Azahar");
    const int title_w = vita2d_pgf_text_width(font, 1.0f, "Azahar");
    vita2d_pgf_draw_text(font, 16 + title_w + 10, 23, Rgb(0x9AA3AD), 0.72f, kVersion);

    if (!entries.empty()) {
        char count[24];
        std::snprintf(count, sizeof(count), "%d / %d", selected + 1, Count());
        const int w = vita2d_pgf_text_width(font, 0.8f, count);
        vita2d_pgf_draw_text(font, (kScreenW - w) / 2, 23, Rgb(0xC9D0D7), 0.8f, count);
    }

    // Bateria (si la hay: la PS TV no tiene) y reloj, a la derecha.
    float right = static_cast<float>(kScreenW) - 16.0f;
    const int battery = scePowerGetBatteryLifePercent();
    if (battery >= 0 && battery <= 100) {
        const bool charging = scePowerIsBatteryCharging() != 0;
        const float bw = 28.0f;
        const float bx = right - bw - 3.0f;
        const float by = 11.0f;
        vita2d_draw_rectangle(bx, by, bw, 13.0f, Rgb(0xE6EAEE));
        vita2d_draw_rectangle(bx + bw, by + 3.5f, 3.0f, 6.0f, Rgb(0xE6EAEE));
        vita2d_draw_rectangle(bx + 1.5f, by + 1.5f, bw - 3.0f, 10.0f, Rgb(0x2B3036));
        const unsigned int fill =
            charging ? 0xF5C542 : battery <= 15 ? 0xF0535E : 0x5CD08A;
        vita2d_draw_rectangle(bx + 2.5f, by + 2.5f,
                              (bw - 5.0f) * static_cast<float>(battery) / 100.0f, 8.0f, Rgb(fill));
        char text[8];
        std::snprintf(text, sizeof(text), "%d%%", battery);
        const int w = vita2d_pgf_text_width(font, 0.72f, text);
        right = bx - 8.0f - static_cast<float>(w);
        vita2d_pgf_draw_text(font, static_cast<int>(right), 23, Rgb(0xC9D0D7), 0.72f, text);
        right -= 16.0f;
    }
    SceDateTime now{};
    if (sceRtcGetCurrentClockLocalTime(&now) >= 0) {
        char clock[16];
        std::snprintf(clock, sizeof(clock), "%02u:%02u", static_cast<unsigned int>(now.hour),
                      static_cast<unsigned int>(now.minute));
        const int w = vita2d_pgf_text_width(font, 0.95f, clock);
        vita2d_pgf_draw_text(font, static_cast<int>(right) - w, 24, Rgb(0xFFFFFF), 0.95f, clock);
        char date[16];
        std::snprintf(date, sizeof(date), "%02u/%02u", static_cast<unsigned int>(now.day),
                      static_cast<unsigned int>(now.month));
        const int dw = vita2d_pgf_text_width(font, 0.72f, date);
        vita2d_pgf_draw_text(font, static_cast<int>(right) - w - 10 - dw, 23, Rgb(0x9AA3AD),
                             0.72f, date);
    }
}

void HomeMenu::DrawMiniScreen(float x, float y, float w, float h) {
    // El marco negro de la pantalla superior de la consola.
    FillRoundRect(x - 6.0f, y - 6.0f, w + 12.0f, h + 12.0f, 10.0f, Rgb(0x23272D));
    vita2d_draw_rectangle(x, y, w, h, Rgb(0x0E1013));
    if (entries.empty()) {
        return;
    }
    if (shown_selection != selected) {
        shown_selection = selected;
        shot_fade = 0.0f;
    }
    shot_fade = std::min(1.0f, shot_fade + 0.12f);
    const unsigned int alpha = static_cast<unsigned int>(shot_fade * 255.0f);

    if (LoadScreenshot(selected)) {
        vita2d_draw_texture_tint_part_scale(shot, x, y, 0.0f, 0.0f, kShotW, kShotH,
                                            w / static_cast<float>(kShotW),
                                            h / static_cast<float>(kShotH),
                                            Rgb(0xFFFFFF, alpha));
        // Reflejo suave arriba, como el cristal de la pantalla.
        vita2d_draw_rectangle(x, y, w, h * 0.18f, Rgb(0xFFFFFF, 0x12));
        return;
    }

    // Aun sin captura (no se ha jugado): degradado del color del juego con el
    // icono en grande y una pista de que la imagen sale al jugar.
    const Entry& entry = entries[selected];
    const unsigned int top = Mix(entry.tint, 0xFFFFFF, 0.35f);
    const unsigned int bottom = Mix(entry.tint, 0x101418, 0.55f);
    constexpr int kBands = 12;
    for (int i = 0; i < kBands; i++) {
        const float t = static_cast<float>(i) / static_cast<float>(kBands - 1);
        vita2d_draw_rectangle(x, y + h * static_cast<float>(i) / kBands, w, h / kBands + 1.0f,
                              Rgb(Mix(top, bottom, t), alpha));
    }
    const float big = h * 0.6f;
    DrawTile(selected, x + (w - big) / 2.0f, y + (h - big) / 2.0f - 8.0f, big, false);
    I18n::DrawTextFit(font, static_cast<int>(x) + 10, static_cast<int>(y + h) - 8,
                      Rgb(0xFFFFFF, 0xD0), 0.6f, I18n::Tr(I18n::kHomeNoShot),
                      static_cast<int>(w) - 20);
}

void HomeMenu::DrawPanel() {
    FillRoundRect(kPanelX, kPanelY + 4.0f, kPanelW, kPanelH, 16.0f, Rgb(0xBDC3CA));
    FillRoundRect(kPanelX - 1.0f, kPanelY - 1.0f, kPanelW + 2.0f, kPanelH + 2.0f, 16.0f,
                  Rgb(0xD2D7DD));
    FillRoundRect(kPanelX, kPanelY, kPanelW, kPanelH, 15.0f, Rgb(0xFFFFFF));
    // Franja de color arriba, como el marco del titulo en la pantalla superior.
    vita2d_draw_rectangle(kPanelX + 15.0f, kPanelY, kPanelW - 30.0f, 4.0f, Rgb(kCyan));

    // La pantalla superior en pequeno, en la proporcion de la de la 3DS (5:3).
    constexpr float kMiniH = 148.0f;
    constexpr float kMiniW = kMiniH * 5.0f / 3.0f;
    const float mini_x = kPanelX + 20.0f;
    const float mini_y = kPanelY + (kPanelH - kMiniH) / 2.0f + 2.0f;
    DrawMiniScreen(mini_x, mini_y, kMiniW, kMiniH);

    const float info_x = mini_x + kMiniW + 30.0f;
    const int line_w = static_cast<int>(kPanelX + kPanelW - info_x) - 24;
    if (entries.empty()) {
        I18n::DrawTextFit(font, static_cast<int>(info_x), static_cast<int>(kPanelY) + 80,
                          Rgb(kInk), 1.1f, I18n::Tr(I18n::kHomeNoGames), line_w);
        I18n::DrawTextFit(font, static_cast<int>(info_x), static_cast<int>(kPanelY) + 116,
                          Rgb(kInkDim), 0.8f, I18n::Tr(I18n::kHomeNoGamesHint), line_w);
        return;
    }

    const Entry& entry = entries[selected];
    constexpr float kIcon = 84.0f;
    const float icon_y = kPanelY + 24.0f;
    DrawTile(selected, info_x, icon_y, kIcon, false);

    const int text_x = static_cast<int>(info_x + kIcon + 22.0f);
    const int text_w = static_cast<int>(kPanelX + kPanelW) - text_x - 24;
    const std::string title = Ellipsize(font, 1.2f, entry.title, text_w);
    vita2d_pgf_draw_text(font, text_x, static_cast<int>(icon_y) + 34, Rgb(kInk), 1.2f,
                         title.c_str());
    if (!entry.publisher.empty()) {
        const std::string publisher = Ellipsize(font, 0.88f, entry.publisher, text_w);
        vita2d_pgf_draw_text(font, text_x, static_cast<int>(icon_y) + 66, Rgb(kInkDim), 0.88f,
                             publisher.c_str());
    } else if (entry.info_loaded && entry.icon.empty()) {
        I18n::DrawTextFit(font, text_x, static_cast<int>(icon_y) + 66, Rgb(kInkDim), 0.72f,
                          I18n::Tr(I18n::kHomeNoInfo), text_w);
    }
    vita2d_draw_rectangle(info_x, kPanelY + 124.0f, static_cast<float>(line_w), 1.0f,
                          Rgb(0xE1E5E9));
    const std::string file = Ellipsize(font, 0.72f, entry.name, line_w);
    vita2d_pgf_draw_text(font, static_cast<int>(info_x), static_cast<int>(kPanelY) + 150,
                         Rgb(0xA3AAB2), 0.72f, file.c_str());
}

void HomeMenu::DrawTile(int index, float x, float y, float size, bool is_selected) {
    const Entry& entry = entries[index];
    const float radius = size * 0.14f;
    if (is_selected) {
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(frame) * 0.12f);
        FillRoundRect(x - 6.0f, y - 6.0f, size + 12.0f, size + 12.0f, radius + 6.0f,
                      Rgb(Mix(kCyan, kCyanLight, pulse)));
    } else {
        FillRoundRect(x, y + 3.0f, size, size, radius, Rgb(0xB7BDC5));
    }
    FillRoundRect(x, y, size, size, radius, Rgb(0xFFFFFF));

    const float inset = size * 0.09f;
    const float icon = size - 2.0f * inset;
    if (!entry.icon.empty()) {
        UploadIcon(index);
    }
    if (entry.uploaded && atlas != nullptr) {
        const float scale = icon / static_cast<float>(kIconSize);
        vita2d_draw_texture_part_scale(atlas, x + inset, y + inset,
                                       static_cast<float>((index % kCellsPerRow) * kCell + 1),
                                       static_cast<float>((index / kCellsPerRow) * kCell + 1),
                                       kIconSize, kIconSize, scale, scale);
        return;
    }

    // Sin icono: azulejo de color con la inicial.
    FillRoundRect(x + inset, y + inset, icon, icon, radius * 0.6f, Rgb(entry.tint));
    char letter[2] = {'?', '\0'};
    for (const char c : entry.fallback) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            letter[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            break;
        }
    }
    const float scale = size / 44.0f;
    const int w = vita2d_pgf_text_width(font, scale, letter);
    const int h = vita2d_pgf_text_height(font, scale, letter);
    vita2d_pgf_draw_text(font, static_cast<int>(x + (size - static_cast<float>(w)) / 2.0f),
                         static_cast<int>(y + (size + static_cast<float>(h)) / 2.0f),
                         Rgb(0xFFFFFF), scale, letter);
}

void HomeMenu::DrawGrid() {
    // Columnas a la vista, con huecos vacios hasta llenar la pantalla.
    const int first = std::max(0, static_cast<int>(camera / kStep) - 1);
    const int last = std::max(Columns(), kVisibleColumns + 1) + 1;
    for (int column = first; column < last; column++) {
        const float x = kGridLeft + static_cast<float>(column) * kStep - camera;
        if (x > static_cast<float>(kScreenW)) {
            break;
        }
        if (x + kTile < 0.0f) {
            continue;
        }
        for (int row = 0; row < kRows; row++) {
            const int index = column * kRows + row;
            const float y = kGridTop + static_cast<float>(row) * kStep;
            if (index >= Count()) {
                FillRoundRect(x, y, kTile, kTile, 12.0f, Rgb(0xC8CED5));
                FillRoundRect(x + 2.0f, y + 2.0f, kTile - 4.0f, kTile - 4.0f, 10.0f,
                              Rgb(0xE2E6EA));
            } else if (index != selected) {
                DrawTile(index, x, y, kTile, false);
            }
        }
    }
    // El seleccionado al final y un poco mas grande, por encima de los vecinos.
    if (!entries.empty()) {
        const int column = selected / kRows;
        const int row = selected % kRows;
        constexpr float kGrow = 6.0f;
        const float x = kGridLeft + static_cast<float>(column) * kStep - camera - kGrow / 2.0f;
        const float y = kGridTop + static_cast<float>(row) * kStep - kGrow / 2.0f;
        DrawTile(selected, x, y, kTile + kGrow, true);
    }
}

void HomeMenu::DrawSlider() {
    constexpr float kTrackX = 200.0f;
    constexpr float kTrackW = 560.0f;
    FillRoundRect(kTrackX, kSliderY, kTrackW, 8.0f, 4.0f, Rgb(0xBCC2C9));
    const int columns = std::max(Columns(), 1);
    const float fraction =
        std::min(1.0f, static_cast<float>(kVisibleColumns) / static_cast<float>(columns));
    const float handle_w = std::max(40.0f, kTrackW * fraction);
    const float max_camera = static_cast<float>(std::max(0, columns - kVisibleColumns)) * kStep;
    const float position = max_camera > 0.0f ? std::clamp(camera / max_camera, 0.0f, 1.0f) : 0.0f;
    FillRoundRect(kTrackX + (kTrackW - handle_w) * position, kSliderY - 1.0f, handle_w, 10.0f,
                  5.0f, Rgb(0x6B727B));
}

void HomeMenu::DrawBottomBar(float settings_hold, float language_hold) {
    vita2d_draw_rectangle(0.0f, kBottomBarY, static_cast<float>(kScreenW),
                          static_cast<float>(kScreenH) - kBottomBarY, Rgb(0xFAFBFC));
    vita2d_draw_rectangle(0.0f, kBottomBarY, static_cast<float>(kScreenW), 1.0f, Rgb(0xC5CBD2));

    // Si en algun idioma no cabe, se reduce el texto de todas las pistas.
    const char* play = I18n::Tr(I18n::kHomePlay);
    const char* exit = I18n::Tr(I18n::kHomeExit);
    const char* settings = I18n::Tr(I18n::kHomeSettingsHold);
    const char* language = I18n::Tr(I18n::kHomeLanguageHold);
    float scale = 0.85f;
    const auto total_width = [&](float s) {
        const float pills = static_cast<float>(vita2d_pgf_text_width(font, 0.62f, "START") +
                                               vita2d_pgf_text_width(font, 0.62f, "SELECT")) +
                            28.0f;
        return 2.0f * 22.0f + pills + 4.0f * 8.0f + 3.0f * 30.0f +
               static_cast<float>(vita2d_pgf_text_width(font, s, play) +
                                  vita2d_pgf_text_width(font, s, exit) +
                                  vita2d_pgf_text_width(font, s, settings) +
                                  vita2d_pgf_text_width(font, s, language));
    };
    const float available = static_cast<float>(kScreenW) - 48.0f;
    const float needed = total_width(scale);
    if (needed > available) {
        scale *= available / needed;
    }

    constexpr float kCy = 516.0f;
    const int text_y = static_cast<int>(kCy + 6.0f);
    const unsigned int ink = Rgb(kInk);
    float x = 24.0f;
    const auto hint_text = [&](const char* text) {
        vita2d_pgf_draw_text(font, static_cast<int>(x), text_y, ink, scale, text);
        const float w = static_cast<float>(vita2d_pgf_text_width(font, scale, text));
        x += w + 30.0f;
        return w;
    };
    const auto hold_bar = [&](float start, float end, float hold) {
        if (hold <= 0.0f) {
            return;
        }
        vita2d_draw_rectangle(start, kCy + 13.0f, end - start, 4.0f, Rgb(0xD5DADF));
        vita2d_draw_rectangle(start, kCy + 13.0f, (end - start) * std::min(hold, 1.0f), 4.0f,
                              Rgb(kCyan));
    };

    if (!entries.empty()) {
        DrawGlyph(Glyph::Cross, x + 11.0f, kCy);
        x += 30.0f;
        hint_text(play);
    }
    DrawGlyph(Glyph::Circle, x + 11.0f, kCy);
    x += 30.0f;
    hint_text(exit);

    const float settings_start = x;
    x += DrawPill(font, "START", x, kCy) + 8.0f;
    hint_text(settings);
    hold_bar(settings_start, x - 30.0f, settings_hold);

    const float language_start = x;
    x += DrawPill(font, "SELECT", x, kCy) + 8.0f;
    hint_text(language);
    hold_bar(language_start, x - 30.0f, language_hold);
}

void HomeMenu::DrawScene(float settings_hold, float language_hold) {
    for (int i = 0; i < 2 && !entries.empty(); i++) {
        // Que los iconos de la vista se suban antes de dibujar.
        LoadPendingInfo();
    }
    DrawBackground();
    DrawPanel();
    DrawGrid();
    DrawSlider();
    DrawTopBar();
    DrawBottomBar(settings_hold, language_hold);
}

void HomeMenu::Draw(float settings_hold, float language_hold) {
    vita2d_start_drawing();
    vita2d_clear_screen();
    DrawScene(settings_hold, language_hold);
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

int RunLanguagePicker(HomeMenu& menu, vita2d_pgf* font) {
    namespace I18n = VitaFrontend::I18n;
    const int current = I18n::CurrentLanguage();
    int highlight = current;

    constexpr float kRowH = 38.0f;
    constexpr float kW = 440.0f;
    constexpr float kH = 112.0f + kRowH * I18n::kLanguageCount + 52.0f;
    constexpr float kX = (kScreenW - kW) / 2.0f;
    constexpr float kY = (kScreenH - kH) / 2.0f;
    constexpr float kListY = kY + 104.0f;

    // Lo que ya este pulsado (el SELECT que lo abrio) no cuenta como pulsacion.
    unsigned int previous = ~0u;
    bool touching = false;
    int touch_x = 0;
    int touch_y = 0;
    while (true) {
        SceCtrlData pad{};
        sceCtrlPeekBufferPositive(0, &pad, 1);
        const unsigned int pressed = pad.buttons & ~previous;
        previous = pad.buttons;

        if (pressed & SCE_CTRL_CIRCLE) {
            return -1;
        }
        if (pressed & SCE_CTRL_CROSS) {
            return highlight;
        }
        if (pressed & SCE_CTRL_DOWN) {
            highlight = (highlight + 1) % I18n::kLanguageCount;
        }
        if (pressed & SCE_CTRL_UP) {
            highlight = (highlight + I18n::kLanguageCount - 1) % I18n::kLanguageCount;
        }

        SceTouchData touch{};
        const bool down = sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) >= 0 && touch.reportNum > 0;
        if (down) {
            touching = true;
            touch_x = touch.report[0].x / 2;
            touch_y = touch.report[0].y / 2;
            const int row = static_cast<int>((static_cast<float>(touch_y) - kListY) / kRowH);
            if (touch_x >= kX && touch_x < kX + kW && touch_y >= kListY && row >= 0 &&
                row < I18n::kLanguageCount) {
                highlight = row;
            }
        } else if (touching) {
            touching = false;
            const int row = static_cast<int>((static_cast<float>(touch_y) - kListY) / kRowH);
            if (touch_x >= kX && touch_x < kX + kW && touch_y >= kListY && row >= 0 &&
                row < I18n::kLanguageCount) {
                return row;
            }
        }

        vita2d_start_drawing();
        vita2d_clear_screen();
        menu.DrawScene(0.0f, 0.0f);
        vita2d_draw_rectangle(0.0f, 0.0f, kScreenW, kScreenH, Rgb(0x10141A, 0x99));

        FillRoundRect(kX, kY + 5.0f, kW, kH, 18.0f, Rgb(0x1B1F25));
        FillRoundRect(kX, kY, kW, kH, 18.0f, Rgb(0xFFFFFF));
        vita2d_draw_rectangle(kX + 18.0f, kY, kW - 36.0f, 4.0f, Rgb(kCyan));

        // Titulo y explicacion en el idioma resaltado: se lee lo que se va a elegir.
        vita2d_pgf_draw_text(font, static_cast<int>(kX) + 28, static_cast<int>(kY) + 52,
                             Rgb(kInk), 1.3f, I18n::TrIn(highlight, I18n::kLanguageTitle));
        I18n::DrawTextFit(font, static_cast<int>(kX) + 28, static_cast<int>(kY) + 82,
                          Rgb(kInkDim), 0.85f, I18n::TrIn(highlight, I18n::kLanguageSubtitle),
                          static_cast<int>(kW) - 56);

        for (int i = 0; i < I18n::kLanguageCount; i++) {
            const float y = kListY + static_cast<float>(i) * kRowH;
            if (i == highlight) {
                FillRoundRect(kX + 16.0f, y + 2.0f, kW - 32.0f, kRowH - 4.0f, 10.0f,
                              Rgb(0xDDF2FF));
                vita2d_draw_rectangle(kX + 16.0f, y + 8.0f, 4.0f, kRowH - 16.0f, Rgb(kCyan));
            }
            vita2d_pgf_draw_text(font, static_cast<int>(kX) + 36, static_cast<int>(y) + 26,
                                 Rgb(i == highlight ? 0x1478B8 : kInk), 1.0f,
                                 I18n::NativeName(i));
            if (i == current) {
                // El que esta puesto ahora: un punto a la derecha.
                vita2d_draw_fill_circle(kX + kW - 40.0f, y + kRowH / 2.0f, 6.0f, Rgb(kCyan));
            }
        }

        const float footer_y = kY + kH - 28.0f;
        float x = kX + 28.0f;
        DrawGlyph(Glyph::Cross, x + 11.0f, footer_y);
        x += 30.0f;
        const char* confirm = I18n::TrIn(highlight, I18n::kConfirm);
        vita2d_pgf_draw_text(font, static_cast<int>(x), static_cast<int>(footer_y) + 6,
                             Rgb(kInk), 0.85f, confirm);
        x += static_cast<float>(vita2d_pgf_text_width(font, 0.85f, confirm)) + 30.0f;
        DrawGlyph(Glyph::Circle, x + 11.0f, footer_y);
        x += 30.0f;
        vita2d_pgf_draw_text(font, static_cast<int>(x), static_cast<int>(footer_y) + 6,
                             Rgb(kInk), 0.85f, I18n::TrIn(highlight, I18n::kBack));

        vita2d_end_drawing();
        vita2d_swap_buffers();
    }
}

} // namespace VitaFrontend
