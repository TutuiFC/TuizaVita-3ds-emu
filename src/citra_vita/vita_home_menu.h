// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <string>
#include <vector>
#include <vita2d.h>
#include "common/common_types.h"

namespace VitaFrontend {

/**
 * EL CUELGUE DEL MENU, ATACADO EN SU CAUSA (0.4.5.0).
 *
 * 0.4.4.6 lo tapo esperando a la GPU en CADA fotograma durante 120 fotogramas
 * tras mover la seleccion. 0.4.4.8 quito el tapon creyendo que el origen era el
 * atlas de iconos, y el cuelgue volvio al desplazarse la cuadricula: el origen es
 * el pool de vita2d, ver StartMenuDrawing en vita_home_menu.cpp. La espera antes
 * de escribir una textura se queda porque hace falta (el atlas lo puede estar
 * leyendo el fotograma anterior), y fuera de la escena: HomeMenu::PrepareFrame.
 *
 * Ajustes (solo ajustes.txt):
 *   menu_espera_atlas=0  vuelve a escribir el atlas sin esperar (0.4.4.7).
 *   menu_tapon=1         vuelve al tapon de 120 fotogramas de 0.4.4.6.
 *   menu_notas=1         vuelve a anotar cada carga del menu en crash.txt.
 */
extern std::atomic<bool> g_menu_wait_atlas;
extern std::atomic<bool> g_menu_hold_frames;
extern std::atomic<bool> g_menu_notes;

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

    /// El piloto (0.4.4.6): deja seleccionado ese juego. False si no esta.
    bool SelectPath(const std::string& path);

    /// Cruceta, gatillos y tactil. True = arrancar el juego seleccionado.
    bool Update(unsigned int buttons, unsigned int pressed);

    /// Lee el icono y el titulo de un juego que aun no los tenga (uno por llamada).
    void LoadPendingInfo();

    /// Los titulos se eligen segun el idioma del menu: hay que releerlos al cambiarlo.
    void RefreshTitles();

    /// Lee y sube lo que haga falta para este fotograma. Espera a la GPU y escribe
    /// texturas: siempre antes de vita2d_start_drawing, nunca con la escena abierta.
    void PrepareFrame();

    /// El menu entero, sin vita2d_start_drawing ni swap (el selector de idioma
    /// lo usa de fondo). Las 'hold' van de 0 a 1 mientras se mantiene START/SELECT.
    void DrawScene(float settings_hold, float language_hold);
    void Draw(float settings_hold, float language_hold);

    /// Devuelve las texturas (iconos y captura) a la CDRAM antes de arrancar un juego.
    void ReleaseTextures();

    /// Al volver de un juego: su captura es nueva y hay que volver a leerla.
    void ForgetScreenshot(const std::string& path);

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
        /// Captura de la pantalla superior: 0 sin mirar, 1 no hay, 2 hay.
        int shot_state = 0;
        /// Codigo de 4 letras (ECLA) para la caratula, y si esta: 0/1/2 como arriba.
        std::string product_id;
        int cover_state = 0;
        u32 cover_retry = 0;
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
    /// La "pantalla superior" de la ficha: la captura del juego, o su icono en grande.
    void DrawMiniScreen(float x, float y, float w, float h);
    bool LoadScreenshot(int index);
    bool LoadCover(int index);
    void DrawGrid();
    void DrawTile(int index, float x, float y, float size, bool is_selected);
    void DrawSlider();
    void DrawBottomBar(float settings_hold, float language_hold);

    vita2d_pgf* font;
    std::vector<Entry> entries;
    int selected = 0;
    vita2d_texture* atlas = nullptr;
    bool atlas_failed = false;

    /// Una sola textura para la captura del seleccionado (se cambia al moverse).
    vita2d_texture* shot = nullptr;
    int shot_for = -1;
    /// Fundido de entrada de la captura, de 0 a 1.
    float shot_fade = 0.0f;
    int shown_selection = -1;

    /// La caratula del seleccionado (0.3.4.1), igual: una sola textura.
    vita2d_texture* cover = nullptr;
    int cover_for = -1;

    /// Desplazamiento horizontal de la cuadricula, en pixeles, y adonde va.
    float camera = 0.0f;
    float camera_target = 0.0f;
    /// Mientras se arrastra con el dedo, la camara no sigue a la seleccion.
    bool free_camera = false;
    u32 frame = 0;
    /// Fotogramas que aun se anotan en crash.txt tras mover la seleccion (0.4.4.6).
    int diag_frames = 0;

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
 * Guarda la pantalla superior del juego tal y como se ve ahora en la Vita
 * (0.3.4.0). Se llama al salir con START+SELECT, con el ultimo fotograma
 * todavia en pantalla. Se lee la imagen que esta mostrando la consola, asi que
 * vale igual con GXM que con software y en cualquier disposicion: x, y, w, h
 * dicen donde esta la superior. Va a ux0:/data/azahar/capturas/<fichero>.scr,
 * en RGB565 a 400x240.
 */
void CaptureGameScreen(const std::string& path, int x, int y, int w, int h);

/**
 * Selector del idioma de los menus, dibujado encima del menu de inicio.
 * Devuelve el idioma elegido (VitaFrontend::I18n::Language) o -1 si se cancela.
 */
int RunLanguagePicker(HomeMenu& menu, vita2d_pgf* font);

} // namespace VitaFrontend
