// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string>
#include <vector>
#include <vita2d.h>
#include "common/common_types.h"

namespace VitaFrontend {

/// Un fichero de juego encontrado en la carpeta de ROMs.
struct HomeGame {
    std::string name; ///< nombre del fichero, con extension
    std::string path;
};

/**
 * Menu de inicio al estilo del HOME de la 3DS (0.3.3.9).
 *
 * Arriba, como la pantalla superior de la consola, la ficha del juego
 * seleccionado: icono grande, titulo y editor sacados del SMDH del ROM. Debajo,
 * la cuadricula de iconos de dos filas que se desplaza en horizontal, con los
 * huecos vacios dibujados y la barra de desplazamiento, como la pantalla
 * inferior. Se maneja con la cruceta, L/R (pagina) y la pantalla tactil:
 * tocar un icono lo selecciona y tocar el seleccionado lo arranca.
 *
 * Los iconos se leen de uno en uno, un juego por fotograma, para que el menu
 * salga al instante aunque haya muchos ROMs. Van todos en una sola textura de
 * 512x512 (vita2d reserva cada textura en bloques grandes de CDRAM, y una por
 * icono se comeria la memoria de video que luego necesita el juego).
 */
class HomeMenu {
public:
    explicit HomeMenu(vita2d_pgf* font);
    ~HomeMenu();

    HomeMenu(const HomeMenu&) = delete;
    HomeMenu& operator=(const HomeMenu&) = delete;

    void SetGames(const std::vector<HomeGame>& games);
    int Count() const {
        return static_cast<int>(entries.size());
    }
    int Selected() const {
        return selected;
    }
    const std::string& SelectedPath() const;

    /// Cruceta, gatillos y tactil. True = arrancar el juego seleccionado.
    bool Update(unsigned int buttons, unsigned int pressed);

    /// Lee el icono y el titulo de un juego que aun no los tenga (uno por llamada).
    void LoadPendingInfo();

    /// Los titulos se eligen segun el idioma del menu: hay que releerlos al cambiarlo.
    void RefreshTitles();

    /// El menu entero, sin vita2d_start_drawing ni swap (el selector de idioma
    /// lo usa de fondo). Las 'hold' van de 0 a 1 mientras se mantiene START/SELECT.
    void DrawScene(float settings_hold, float language_hold);
    void Draw(float settings_hold, float language_hold);

    /// Devuelve la textura de iconos a la CDRAM antes de arrancar un juego.
    void ReleaseTextures();

private:
    struct Entry {
        std::string name;
        std::string path;
        std::string fallback; ///< nombre sin extension
        std::string title;
        std::string publisher;
        std::vector<u16> icon; ///< 48x48 RGB565; vacio = sin icono
        bool info_loaded = false;
        bool uploaded = false;
        u32 tint = 0;
    };

    void LoadInfo(Entry& entry);
    bool EnsureAtlas();
    void UploadIcon(int index);
    void FollowSelection();
    int Columns() const;
    int TileAt(int x, int y) const;
    void MoveSelection(int delta);

    void DrawBackground();
    void DrawTopBar();
    void DrawPanel();
    void DrawGrid();
    void DrawTile(int index, float x, float y, float size, bool is_selected);
    void DrawSlider();
    void DrawBottomBar(float settings_hold, float language_hold);

    vita2d_pgf* font;
    std::vector<Entry> entries;
    int selected = 0;
    vita2d_texture* atlas = nullptr;
    bool atlas_failed = false;

    /// Desplazamiento horizontal de la cuadricula, en pixeles, y adonde va.
    float camera = 0.0f;
    float camera_target = 0.0f;
    /// Mientras se arrastra con el dedo, la camara no sigue a la seleccion.
    bool free_camera = false;
    u32 frame = 0;

    /// Repeticion de la cruceta mantenida.
    unsigned int repeat_buttons = 0;
    u64 repeat_next_us = 0;

    /// Pantalla tactil: donde empezo el toque y si ya es un arrastre.
    bool touching = false;
    bool dragging = false;
    int touch_start_x = 0;
    int touch_start_y = 0;
    int touch_last_x = 0;
    int touch_last_y = 0;
    float touch_start_camera = 0.0f;
};

/**
 * Selector del idioma de los menus, dibujado encima del menu de inicio.
 * Devuelve el idioma elegido (VitaFrontend::I18n::Language) o -1 si se cancela.
 */
int RunLanguagePicker(HomeMenu& menu, vita2d_pgf* font);

} // namespace VitaFrontend
